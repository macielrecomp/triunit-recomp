"""Run finite MK2 translated frontiers with the shared MK1 DX12 presenter.

This is a diagnostic, not a playable replacement. An explicit --profile can persist
the supported finite valid-CMOS boot; other routes leave profiles untouched.
Presentation accepts the existing MK1_* environment contract, including CRT parameters.
--settings reads the Launcher/MK2 INI with its selected video profile and isolates presentation from ambient overrides.
"""
from pathlib import Path
import argparse
import errno
import hashlib
import json
import os
import shutil
import struct
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.mk2 import run as foundation
from tools.mk2 import profile as cmos_profile
from tools.quality import run as quality

REFERENCE_INPUTS=[[8096368,1,65535,0],[9139625,1,65519,1],[9140064,0,65519,0],[10967550,0,65535,1],
        [51181900,1,65535,2],[53009825,1,65534,3],[54837750,0,65534,2],[56665675,0,65535,3],
        [62149450,1,65535,2],[63977375,1,65534,3],[65805300,0,65534,2],[67633225,0,65535,3],
        [365585000,1,65535,4],[367412925,1,65531,5],[369240850,0,65531,4],[371068775,0,65535,5],
        [438702000,1,65535,0],[440529925,1,65519,1],[442357850,0,65519,0],[444185775,0,65535,1]]
REFERENCE_COIN_INPUT_LUA=r'''
local coin_port=assert(machine.ioport.ports[":IN1"])
local coin=assert(coin_port.fields["Coin 1"])
assert(coin.mask==1)
local coin_frame,coin_previous,coin_requested=0,coin_port:read(),0
mk2_reference_coin_subscription=emu.add_machine_frame_notifier(function()
    coin_frame=coin_frame+1
    local value=coin_port:read()
    if (value & 1)~=(coin_previous & 1) then
        events[#events+1]={tick(),coin_requested,value,3};coin_previous=value
    end
    if coin_frame==28 or coin_frame==30 or coin_frame==34 or coin_frame==36 then
        coin_requested=(coin_frame==28 or coin_frame==34) and 1 or 0
        coin:set_value(coin_requested)
        events[#events+1]={tick(),coin_requested,coin_port:read(),2}
    end
end)
'''


def compare_frames(native,reference,*,expected_frames=11):
    """Compare integral BGRA rasters at the same 100 MHz boundary, never hashes alone."""
    rows=[]
    for number in range(expected_frames):
        headers=[stream.read(16) for stream in (native,reference)]
        if any(len(header)!=16 for header in headers) or headers[0]!=headers[1]:
            raise ValueError(f'Frame {number}: missing or different raster boundary/geometry')
        tick,width,height=struct.unpack('<QII',headers[0])
        if not width or not height or width>506 or height>289 or (rows and tick<=rows[-1]['tick_100mhz']):
            raise ValueError(f'Frame {number}: invalid raster geometry or order')
        pixels=[stream.read(width*height*4) for stream in (native,reference)]
        if any(len(data)!=width*height*4 for data in pixels):
            raise ValueError(f'Frame {number}: truncated pixel payload')
        if pixels[0]!=pixels[1]:
            offset=next(i for i,(left,right) in enumerate(zip(*pixels)) if left!=right)//4
            raise ValueError(f'Frame {number}: pixel ({offset%width},{offset//width}) differs')
        rows.append({'tick_100mhz':tick,'width':width,'height':height,
                     'sha256':hashlib.sha256(pixels[0]).hexdigest()})
    if native.read(1) or reference.read(1):
        raise ValueError('Unexpected extra raster frames')
    return {'passed':True,'pixels_compared':sum(row['width']*row['height'] for row in rows),'frames':rows}


def prepare_reference_roms(directory,roms,manifest):
    roms.mkdir()
    for rom in manifest['roms']:
        name=rom['upstream_name']
        if Path(name).name!=name or any(char in name for char in '/\\:'):
            raise ValueError('Invalid reference ROM name')
        try:
            os.link(directory/rom['path'],roms/name)
        except OSError as error:
            if error.errno!=errno.EXDEV and getattr(error,'winerror',None)!=17:
                raise
            shutil.copyfile(directory/rom['path'],roms/name)


def capture_screen(scratch,directory,manifest,through='first-draw',*,cmos_image=None):
    persisted=through in ('cmos-boot','cmos-startup','cmos-draw')
    if (persisted and (not isinstance(cmos_image,bytes) or len(cmos_image)!=cmos_profile.SIZE)) or (not persisted and cmos_image is not None):
        raise ValueError('Raster reference CMOS must match the selected persisted boot')
    expected=frontier(through,cmos_image=cmos_image)
    end_cycle,end_pc=expected['cycle'],expected['pc']
    expected_frames=end_cycle*16//1827925
    from tools.mk2 import batch_reference,reference
    executable=batch_reference.MAME
    if quality.sha(executable)!=batch_reference.MAME_SHA:
        raise ValueError('Reference executable identity differs')
    prepare_reference_roms(directory,scratch/'mk2',manifest)
    if persisted:
        nvram=scratch/'nvram/mk2'
        nvram.mkdir(parents=True)
        (nvram/'nvram').write_bytes(cmos_image)
    lua=r'''local machine=manager.machine
local main=machine.devices[":maincpu"]
local screen=machine.screens[":screen"]
local port=machine.ioport.ports[":IN0"]
local button=port.fields["P1 High Punch"]
assert(button.mask==0x10)
local events,requested,previous={},0,port:read()
local function tick() return math.floor(machine.time:as_double()*100000000+0.5) end
local tap=main.spaces["program"]:install_read_tap(0x01a80000,0x01a8000f,"mk2-frame-input",function()
    local pc,t=main.state["PC"].value,tick()
    if pc==0xffa5aef0 and t==8096368 then
        assert(requested==0 and #events==0);requested=1
    elseif pc==0xffa5b130 and t==9140064 then
        assert(requested==1 and #events==2);requested=0
    else return end
    button:set_value(requested)
    events[#events+1]={t,requested,port:read(),0}
end)
local subscription=emu.add_machine_frame_notifier(function()
    local value=port:read()
    if value~=previous then events[#events+1]={tick(),requested,value,1};previous=value end
end)
local frames=assert(io.open("reference.frames","wb"))
coroutine.wrap(function()
    -- Sample the real scanline after its timer, before the next instruction or bitmap swap.
    -- The initial snapshot viewport lags HEBLNK by ten pixels until hblank stabilises.
    for frame=0,10 do
        local ending=(frame+1)*1827925
        local rows={}
        for y=20,273 do
            local at=ending-(274-y)*6325
            emu.wait(emu.attotime.from_ticks(at,100000000)-machine.time+emu.attotime(0,1))
            assert(tick()==at)
            local bus=main.spaces["program"]
            local first,last=bus:read_u16(0xc0000010)*2,bus:read_u16(0xc0000020)*2
            assert(first==90 and last==490 and bus:read_u16(0xc0000050)==20 and bus:read_u16(0xc0000060)==274)
            local row={}
            for x=first,last-1 do row[#row+1]=string.pack("<I4",screen:pixel(x,y)) end
            rows[#rows+1]=table.concat(row)
        end
        emu.wait(emu.attotime.from_ticks(ending,100000000)-machine.time+emu.attotime(0,1))
        assert(tick()==ending)
        assert(frames:write(string.pack("<I8I4I4",ending,400,254),table.concat(rows)))
    end
end)()
mk2_frame_stop_subscription=emu.add_machine_stop_notifier(function()
    tap:remove()
    assert(subscription)
    assert(frames:close())
    local file=assert(io.open("frame-inputs.json","w"))
    local rows={}
    for _,row in ipairs(events) do rows[#rows+1]="["..table.concat(row,",").."]" end
    assert(file:write("["..table.concat(rows,",").."]"));assert(file:close())
end)
'''
    if through!='first-draw':
        if lua.count('for frame=0,10 do')!=1:raise ValueError('Original raster sample loop changed')
        lua=lua.replace('for frame=0,10 do',f'for frame=0,{expected_frames-1} do')
        if not persisted:lua+=REFERENCE_COIN_INPUT_LUA
    if persisted:
        begin=lua.index('local tap=main.spaces[')
        end=lua.index('local subscription=',begin)
        lua=lua[:begin]+'local tap=nil\n'+lua[end:]
        lua=lua.replace('    tap:remove()','    if tap then tap:remove() end')
    if through=='first-fight':
        lua+='\nlocal start_requested=0\n'+batch_reference.START_INPUT_LUA.replace('input_events','events')
        lua+=batch_reference.SELECTION_INPUT_LUA.replace('input_events','events').replace('input_port','port')
    (scratch/'frames.lua').write_text(lua,encoding='ascii')
    registers=[f'a{n}' for n in range(15)]+['sp']+[f'b{n}' for n in range(15)]
    fmt='MK2_FRAME_CPU '+' '.join(['%X']*34)+'\\n'
    script='focus :maincpu\n'
    # Preserve the historical read-only debugger triggers for the legal button requests.
    for pc,cycle in (() if persisted else ((0xffa5aef0,506023),(0xffa5b130,571254))):
        script+=f'bpset {pc:x},totalcycles=={cycle:x},{{printf "MK2_FRAME_INPUT %X\\n",w@1a80000;g}}\n'
    saved_cmos=';save frame-cmos.bin,1400000,20000' if persisted else ''
    script+=f'bpset {end_pc:08x},totalcycles=={end_cycle:x},{{printf "{fmt}",pc,st,totalcycles,'+','.join(registers)+saved_cmos+';quit}\ngo\n'
    (scratch/'frames.cmd').write_text(script,encoding='ascii')
    args=[executable,'mk2','-noreadconfig','-nowriteconfig','-rompath',scratch,
          '-video','none','-sound','none','-nothrottle','-skip_gameinfo','-noplugins','-frameskip','0',
          '-debug','-debugger','windows','-debuglog','-debugscript','frames.cmd',
          '-autoboot_delay','0','-autoboot_script','frames.lua',
          '-nvram_directory',scratch/'nvram','-cfg_directory',scratch/'cfg',
          '-snapshot_directory',scratch/'snap','-homepath',scratch/'home','-seconds_to_run',str(end_cycle*16//100000000+1)]
    result=quality.command(args,scratch,timeout=600 if through=='first-fight' else 120,cwd=scratch)
    for line in result['stdout'].splitlines():reference.reject_mame_diagnostic(line)
    if not result['passed'] or result['discarded_output_bytes']:
        raise RuntimeError('Reference screen capture failed: '+result['stdout'][-2000:])
    lines=(scratch/'debug.log').read_text().splitlines()
    for line in lines:reference.reject_mame_diagnostic(line)
    cpu=[[int(value,16) for value in line.split()[1:]] for line in lines if line.startswith('MK2_FRAME_CPU ')]
    inputs=json.loads((scratch/'frame-inputs.json').read_text())
    if (len(cpu)!=1 or not frontier_matches(cpu[0],expected) or
        inputs!=([] if persisted else REFERENCE_INPUTS[:20 if through=='first-fight' else 12 if through=='next-wait' else 4])):
        raise ValueError(f'Reference CPU boundary or legal input timeline differs: {cpu}, {inputs}')
    if quality.sha(executable)!=batch_reference.MAME_SHA:
        raise ValueError('Reference executable changed during capture')
    return {'execution':quality.compact(result),'executable_sha256':batch_reference.MAME_SHA,
            'scope':f'{expected_frames} complete active-signal rasters from '+('exact persisted CMOS reset' if persisted else 'cold boot')+'; real screen pixels sampled per scanline, no state writes or viewport normalisation',
            'cpu34':cpu[0],'inputs':inputs,'lua_sha256':quality.sha(scratch/'frames.lua'),
            'debug_script_sha256':quality.sha(scratch/'frames.cmd')}


def read_settings(path):
    """Read the Launcher INI without its MAME-only MK2 capability normalisation."""
    with Path(path).open('rb') as stream:raw=stream.read(8193)
    if len(raw)>8192 or b'\0' in raw:raise ValueError('Launcher Settings must be text of at most 8192 bytes')
    text=raw.decode('utf-8-sig')
    legacy_video={'scalefx':0,'scalefxamount':100,'scanlines':0,'filter':0,'sync':1,'color':0,
                  'brightness':100,'bezel':1,'highrefresh':0}
    video_defaults={**legacy_video,'fullscreen':1,'scalemode':0,'cas':0,'bilateral':0,'smaa':0}
    values={'launcher':{'fps':60,'vsync':1,'reducedmotion':0,'uivolume':0,'fullscreen':1,'videoeffects':0},
            'mk2':{**legacy_video,'audiolatency':1,'remastered':0,'gamevolume':100,'blood':1,'violence':1,'cmoscoinage':0}}
    profile_sections=('video.original','video.remastered')
    profiles={}
    allowed={'fps':(30,60,120,240),'vsync':(0,1),'reducedmotion':(0,1),'uivolume':range(101),'videoeffects':(0,1),
             'scalefx':(0,1),'scalefxamount':range(101),'fullscreen':(0,1),'scanlines':range(9),'filter':(0,1),'sync':range(3),
             'color':range(3),'brightness':(80,90,100,110,120),'audiolatency':(1,2),
             'remastered':(0,1),'bezel':(0,1),'highrefresh':(0,1),'gamevolume':range(101),'blood':(0,1),'violence':(0,1),'cmoscoinage':(0,1)}
    profile_allowed={**allowed,'scanlines':range(10),'filter':range(4),'scalemode':range(6),
                     'cas':range(101),'bilateral':range(101),'smaa':(0,1)}
    section=None;seen=set()
    for line in text.splitlines():
        line=line.strip()
        if not line or line[0] in ';#':continue
        if line.startswith('[') and line.endswith(']'):
            section=line[1:-1].strip().lower()
            if section in profile_sections:profiles.setdefault(section,{})
            elif section not in values:raise ValueError('Settings section must be Launcher, MK2, Video.Original or Video.Remastered')
            continue
        if section is None or '=' not in line:raise ValueError('Invalid Launcher Settings syntax')
        key,value=(part.strip() for part in line.split('=',1));key=key.lower()
        modern_section=section in profiles
        if key not in (video_defaults if modern_section else values[section]) or (section,key) in seen:
            raise ValueError('Unknown or duplicate Launcher Settings key: '+key)
        seen.add((section,key))
        if not value or not value.lstrip('+-').isascii() or not value.lstrip('+-').isdigit():
            raise ValueError('Launcher Settings requires integer: '+key)
        number=int(value)
        if number not in (profile_allowed if modern_section else allowed)[key]:
            raise ValueError('Launcher Settings outside validated range: '+key)
        (profiles if modern_section else values)[section][key]=number
    if 'mk2' not in {part for part,_ in seen}:raise ValueError('Launcher Settings requires MK2 values')
    game=values['mk2'];launcher=values['launcher']
    edition=profile_sections[game['remastered']]
    video={**video_defaults,**{key:game[key] for key in legacy_video},'fullscreen':launcher['fullscreen'],**profiles.get(edition,{})}
    scanlines=video['scanlines'];scalefx=video['scalefxamount'] if video['scalefx'] else 0
    filter_mode=('nearest','linear','sharp-bilinear','bicubic')[video['filter']]
    if not profiles and (scanlines>=4 or scalefx):filter_mode='nearest'
    env={'MK1_SCALEFX':str(scalefx),'MK1_SCANLINES':('0','0.25','0.5','0.75','0','0','0','0','0','0')[scanlines],
         'MK1_FILTER':filter_mode,
         'MK1_CRT_PRESET':('off','off','off','off','bvm-1080','bvm-4k','bvm-soft','energy','pvm','phosphor')[scanlines],
         'MK1_SYNC':('vsync','vrr','immediate')[video['sync']],
         'MK1_COLOR':('original','vivid','warm')[video['color']],
         'MK1_BRIGHTNESS':str(video['brightness']),'MK1_BEZEL':str(video['bezel']),
         'MK1_HIGH_REFRESH':str(video['highrefresh']),'MK1_FULLSCREEN':str(video['fullscreen']),'MK1_GAME_VOLUME':str(game['gamevolume'])}
    if profiles:
        env.update(MK1_MUSIC_MODE='original',MK1_SCALE_MODE=str(video['scalemode']),MK1_CAS=str(video['cas']),
                   MK1_BILATERAL=str(video['bilateral']),MK1_SMAA=str(video['smaa']))
    if game['remastered']:env.update(MK1_MUSIC_MODE='remastered',MK1_MUSIC_DIRECTORY=str(quality.ROOT/'custom_audio/mk2'))
    return {'path':str(Path(path).absolute()),'sha256':hashlib.sha256(raw).hexdigest(),
            'requested':{**values,**profiles},'video':video,'environment':env,
            'dsw_word':(0xfc7d&~0xc000)|(0x4000 if game['blood'] else 0)|(0x8000 if game['violence'] else 0),
            'launcher_only':{key:launcher[key] for key in ('fps','vsync','reducedmotion','uivolume')},
            'audio_latency':{'requested':('low','balanced','safe')[game['audiolatency']],
                             'applied':False,'reason':'audio_playback_not_connected'}}


def environment(settings=None):
    # A selected profile is complete: ambient CRT tuning or legacy pacing must not override it.
    env=(dict(settings['environment']) if settings is not None else
         {key:value for key,value in os.environ.items() if key.startswith("MK1_")})
    env.update(PATH=str(quality.DEFAULT_CC.parent)+os.pathsep+os.environ.get("PATH",""),
               MK1_GRAPHICS_VALIDATE="1")
    env.setdefault("MK1_FULLSCREEN","0")
    return env


def diagnostic_output(text):
    rows=[json.loads(line) for line in text.splitlines() if line.strip()]
    if (not rows or not isinstance(rows[-1],dict) or rows[-1].get('scope') not in
        ('mk2-native-first-draw-check','mk2-native-translated-continuation-check','mk2-native-host-controls-check','mk2-native-cmos-boot-check','mk2-native-cmos-startup-check','mk2-native-cmos-draw-check','mk2-native-cmos-host-check')):
        raise ValueError('Missing terminal native diagnostic')
    observed=rows.pop()
    for event in rows:
        if (not isinstance(event,dict) or set(event)!={'audio_underrun_sample','producer_gap_ms'} or
            any(type(value) is not int or value<0 for value in event.values())):
            raise ValueError('Unexpected native diagnostic event')
    audio=observed.get('audio')
    underruns=audio.get('underruns') if isinstance(audio,dict) and observed.get('audio_playback') is True else 0
    if type(underruns) is not int or underruns<0 or len(rows)!=min(underruns,8):
        raise ValueError('Incomplete native audio events')
    return observed,rows


def startup_profile(image):
    if len(image)!=16384:raise ValueError('Startup profile requires complete CMOS')
    if any(image[0x1090:0x1098:2]):return 1
    credits=image[540]
    if not credits:return 0
    price=int.from_bytes(bytes(image[0x1058:0x1060:2]),'big')
    return 2 if credits<price else 3


def frontier(through,dsw_word=0xfc7d,free_play=False,*,cmos_image=None):
    """Expected translated frontier of the Original: PC, cycle and the SHA-256 (tools/mk2/oracle_digest.py) of the first
    `words` CPU34 words. The raw rows are regenerated locally by the oracle (pinned MAME + the user's ROMs)."""
    if type(dsw_word) is not int or dsw_word&~0xc000!=0x3c7d:
        raise ValueError('Only original MK2 blood/violence DIP variations are supported')
    if type(free_play) is not bool:raise ValueError('Free Play boundary selection must be boolean')
    selected=startup_profile(cmos_image)if cmos_image is not None else int(free_play)
    if dsw_word!=0xfc7d or selected:
        if through not in ('cmos-boot','cmos-startup','cmos-draw'):
            raise ValueError('Operator DIP requires the original persisted CMOS boot')
        from tools.mk2 import generate_operator_boundaries as operator
        stage={'cmos-boot':'Reset','cmos-startup':'Setup','cmos-draw':'Draw'}[through]
        row=operator.load()[operator.FAMILIES[selected]][str(dsw_word)]['boundaries'][stage]
        return {'pc':row['pc'],'cycle':row['cycle'],'words':34,'sha256':row['cpu34_sha256']}
    if through=='first-draw':
        return {'pc':0xff80dd30,'cycle':1261260,'words':3,'sha256':'2ae089a0311b245d2fecd4b9df5afc503ff832209489069b52d0f4abe34451f9'}
    if through=='cmos-boot':
        return {'pc':0xff811340,'cycle':6607529,'words':34,'sha256':'b172188b3180d47465680d2fa515e7621bf5703285197e847503f9b6fa0a4054'}
    if through=='cmos-startup':
        path=json.loads((quality.ROOT/'tools/mk2/valid_cmos_startup_path.json').read_text())
        return {'pc':path['dispatcher_end_CPU34_pc'],'cycle':path['dispatcher_end_CPU34_cycle'],'words':34,'sha256':path['dispatcher_end_CPU34_sha256']}
    if through=='cmos-draw':
        path=json.loads((quality.ROOT/'tools/mk2/valid_cmos_draw_path.json').read_text())
        return {'pc':path['end_PC'],'cycle':path['end_cycle'],'words':34,'sha256':path['end_CPU34_sha256']}
    if through=='next-wait':
        return {'pc':0xff80dd30,'cycle':7005800,'words':34,'sha256':'462a678e1e3037dc8310234e3627e3127f890e139ee1fcac48c68b20bba9b0a4'}
    if through=='first-fight':
        window=json.loads((quality.ROOT/'tools/mk2/first_fight_path.json').read_text())['windows'][-1]
        return {'pc':0xff80dd30,'cycle':window['end'],'words':34,'sha256':window['end_cpu34_sha256']}
    raise ValueError('Unknown translated frontier: '+through)


def frontier_matches(cpu34,expected):
    """True when an observed CPU34 list reaches `expected` (frontier()): same PC, cycle and digest."""
    from tools.mk2.oracle_digest import words_digest
    return (isinstance(cpu34,list) and len(cpu34)==34 and all(type(v) is int for v in cpu34) and
            cpu34[0]==expected['pc'] and cpu34[2]==expected['cycle'] and
            words_digest(cpu34[:expected['words']])==expected['sha256'])


def validate_boundary(observed,through,dsw_word=0xfc7d,free_play=False,*,cmos_image=None):
    expected=frontier(through,dsw_word,free_play,cmos_image=cmos_image)
    extended=through!='first-draw'
    frames=expected['cycle']*16//1827925
    cmos=through in ('cmos-boot','cmos-startup','cmos-draw')
    if (observed.get('scope')!=(f'mk2-native-{through}-check' if cmos else 'mk2-native-translated-continuation-check' if extended else 'mk2-native-first-draw-check') or
        observed.get('gameplay_complete') is not False or observed.get('execution_continues') is not False or
        observed.get('termination')!=('finite_valid_cmos_draw_complete' if through=='cmos-draw' else 'finite_valid_cmos_startup_complete' if through=='cmos-startup' else 'finite_valid_cmos_boot_complete' if cmos else 'translated_frontier_reached' if extended else 'finite_diagnostic_complete') or
        (extended and observed.get('through')!=through) or observed.get('frontier_pc')!=expected['pc'] or
        not frontier_matches(observed.get('cpu34'),expected) or
        observed.get('frames')!=frames or observed.get('width')!=400 or observed.get('height')!=254 or
        len(observed.get('frame_sha256',[]))!=frames):
        raise ValueError('Native diagnostic boundary or complete-frame cardinality differs')


def host_control_range():
    windows=json.loads((quality.ROOT/'tools/mk2/first_fight_path.json').read_text())['windows']
    return {'start_window':0,'stop_window':len(windows)-2,'start_cycle':windows[0]['end'],
            'minimum_stop_cycle':windows[-2]['end'],'maximum_cycle':windows[-1]['end']}


def validate_host_boundary(observed):
    cpu=observed.get('cpu34',[])
    host=observed.get('host_controls')
    bounds=host_control_range()
    if (observed.get('scope')!='mk2-native-host-controls-check' or
        observed.get('gameplay_complete') is not False or observed.get('execution_continues') is not False or
        observed.get('termination')!='finite_host_cooperative_boundary' or
        observed.get('input')!='host-keyboard-xinput-finite' or observed.get('through')!='first-fight' or
        observed.get('frontier_pc')!=0xff80dd30 or
        len(cpu)!=34 or cpu[0]!=0xff80dd30 or not bounds['minimum_stop_cycle']<=cpu[2]<=bounds['maximum_cycle'] or
        not isinstance(host,dict) or
        any(host.get(key)!=value for key,value in bounds.items()) or
        not host.get('events') or any(len(row)!=4 or row[3] not in (6,7) for row in host['events']) or
        any(row[0]<bounds['start_cycle']*16 or row[0]>cpu[2]*16 for row in host['events']) or
        any(left[0]>right[0] for left,right in zip(host['events'],host['events'][1:])) or
        observed.get('frames')!=cpu[2]*16//1827925 or observed.get('width')!=400 or
        observed.get('height')!=254 or len(observed.get('frame_sha256',[]))!=observed.get('frames')):
        raise ValueError('Native finite host boundary, inputs or raster cardinality differs')


def valid_host_control_range():
    return {'origin':'valid_cmos_draw','start_cycle':frontier('cmos-draw')['cycle'],
            'stop_after_cycle':148081944,'maximum_cycle':150023965}


def validate_valid_host_boundary(observed):
    bounds=valid_host_control_range()
    cpu=observed.get('cpu34',[]);host=observed.get('host_controls')
    def uint(value,bits):
        return type(value) is int and 0<=value<(1<<bits)
    def requested_ports(value):
        return ((value&0x7f7f)|0x8080) | ((((value>>16)&0x27)|0xffd8)<<16) | (
            (((value>>28)&3)|((value>>21)&0x30)|0xffcc)<<32)
    if (observed.get('scope')!='mk2-native-cmos-host-check' or observed.get('through')!='cmos-host' or
        observed.get('input')!='valid-cmos-host-keyboard-xinput-finite' or
        observed.get('gameplay_complete') is not False or observed.get('execution_continues') is not False or
        not isinstance(cpu,list) or len(cpu)!=34 or any(not uint(v,32) for v in cpu) or
        cpu[0]!=0xff80dd30 or observed.get('frontier_pc')!=cpu[0] or
        not bounds['start_cycle']<=cpu[2]<=bounds['maximum_cycle'] or not isinstance(host,dict) or
        any(host.get(k)!=v for k,v in bounds.items())):
        raise ValueError('Native valid-CMOS host origin or cooperative boundary differs')
    admitted=host.get('provider_admitted');stopped=host.get('stop_requested')
    requests=host.get('provider_requests');events=host.get('events')
    if (type(admitted) is not bool or type(stopped) is not bool or not uint(requests,64) or
        not isinstance(events,list) or any(not isinstance(row,list) or len(row)!=4 or
            not uint(row[0],64) or not uint(row[1],48) or not uint(row[2],48) or
            type(row[3]) is not int or row[3] not in (6,7) for row in events) or
        sum(row[3]==6 for row in events)!=requests or
        any(not bounds['start_cycle']*16<=row[0]<=cpu[2]*16 for row in events) or
        any(a[0]>b[0] for a,b in zip(events,events[1:])) or
        observed.get('termination')!=('finite_valid_cmos_host_stop_requested' if stopped else 'finite_valid_cmos_host_boundary') or
        (admitted and (cpu[2]<=bounds['start_cycle'] or not requests)) or
        (not admitted and (not stopped or requests or events or not frontier_matches(cpu,frontier('cmos-draw')))) or
        (not stopped and (not admitted or cpu[2]<bounds['stop_after_cycle']))):
        raise ValueError('Native valid-CMOS host stop, provider or physical events differ')
    last_tick=host.get('last_input_tick');stop_tick=host.get('stop_observed_tick')
    if (not uint(host.get('last_input'),32) or not uint(last_tick,64) or not uint(stop_tick,64) or
        last_tick>cpu[2]*16 or stop_tick>cpu[2]*16 or
        (last_tick and (last_tick<bounds['start_cycle']*16 or
            not any(row[3]==6 and row[0]==last_tick and
                row[1]==requested_ports(host['last_input']) for row in events))) or
        (not last_tick and host['last_input']!=0xffffffff) or
        (admitted and stopped and stop_tick<bounds['start_cycle']*16) or
        (not stopped and not last_tick) or
        (stopped and (not stop_tick or last_tick>stop_tick)) or (not stopped and stop_tick) or
        (not admitted and (last_tick or host['last_input']!=0xffffffff))):
        raise ValueError('Native valid-CMOS held input or stop timestamp differs')
    frames=cpu[2]*16//1827925
    if (observed.get('frames')!=frames or observed.get('width')!=400 or observed.get('height')!=254 or
        len(observed.get('frame_sha256',[]))!=frames):
        raise ValueError('Native valid-CMOS host complete-raster cardinality differs')


def run(rom_directory,window=False,reference_frames=False,settings=None,audio=False,through='first-draw',host_controls=False,profile_directory=None,dsw_word=0xfc7d):
    root=quality.ROOT
    sources=[p for p in foundation.SOURCES if p not in
             ("native/tests/mk2_foundation_tests.cpp",)]
    sources += ["native/mk2/mk2_native_check.cpp","native/mk2/mk2_music.cpp","native/src/mk5_window.cpp","native/src/mk5_audio_output.cpp"]
    files=list(dict.fromkeys(sources+foundation.HEADERS+
        [p for p in foundation.VERIFIERS if Path(p).suffix in ('.py','.json')]+[
        "native/mk2/mk2_live_host.hpp","native/mk2/mk2_live_limits.hpp","native/mk2/mk2_save_state.hpp","native/mk2/mk2_state_io.hpp","native/include/mk5_profile_presentation.hpp","native/mk2/mk2_scanout.hpp","native/include/mk5_window.hpp",
        "native/include/mk5_crt.hpp","native/include/mk5_crt_shader.hpp","native/include/mk5_stage_bicubic.hpp",
        "native/include/mk5_scalefx.hpp","native/include/mk5_scalefx_shader.hpp",
        "native/include/mk5_video.hpp","native/include/mk5_smaa.hpp","native/include/mk5_smaa_shader.hpp",
        "native/include/mk5_smaa_upstream/AreaTex.h","native/include/mk5_smaa_upstream/SearchTex.h","native/include/mk5_smaa_upstream/SMAA.hlsl.h",
        "native/include/mk5_color.hpp","native/include/mk5_bezel.hpp","native/include/mk5_hdr.hpp",
        "native/include/mk5_timing.hpp","native/include/mk5_gamepad.hpp","native/include/mk5_controls.hpp","native/include/mk5_training.hpp","native/include/mk5_hitbox.hpp","native/include/mk5_hid.hpp",
        "native/include/mk5_machine.hpp","native/include/mk5_irq.hpp",
        "native/include/mk5_audio_link.hpp","tools/mk2/native.py","tools/mk2/batch_reference.py",
        "native/mk2/mk2_audio.hpp","native/mk2/mk2_music.hpp","native/runtime/mk2_music_manifest.hpp","native/audio/williams_resampler.hpp","native/audio/sinc_resampler.hpp","native/audio/studio_master.hpp","native/audio/host_gain.hpp","native/audio/custom_audio.hpp","native/audio/tiny_json.hpp","native/include/mk5_audio_output.hpp","native/include/mk5_audio_volume.hpp",
        "tools/mk2/reference.py","tools/quality/run.py","tools/mk2/roms-l31.json",
        "tools/mk2/first_fight_path.json","tools/mk2/profile.py","tools/mk2/valid_cmos_startup_path.json",
        "tools/mk2/valid_cmos_draw_path.json"]+__import__('tools.setup.mk2',fromlist=['units']).units()))
    receipt={"scope":"mk2-native-first-draw-check" if through=='first-draw' else 'mk2-native-translated-continuation-check',"passed":False,"window":window,
             "gameplay_complete":False,"reference_frames":reference_frames,"audio_requested":audio,
             'host_controls_requested':host_controls,'through':through}
    try:
        loaded_cmos=None;pending_cmos=None
        if profile_directory is not None:
            if through not in ('cmos-boot','cmos-startup','cmos-draw','cmos-host') or (host_controls and through!='cmos-host'):
                raise ValueError('Persistent CMOS requires --through cmos-boot, cmos-startup, cmos-draw or cmos-host; host controls require cmos-host')
            receipt['persistence']={'profile':str(Path(profile_directory).absolute()),'committed':False}
            loaded_cmos=cmos_profile.load(profile_directory)
            receipt['persistence']['loaded_sha256']=hashlib.sha256(loaded_cmos).hexdigest()
        elif through in ('cmos-boot','cmos-startup','cmos-draw','cmos-host'):
            raise ValueError(through+' requires an existing explicit --profile')
        if through=='cmos-host' and (not window or not host_controls or reference_frames):
            raise ValueError('Valid CMOS host requires --window --host-controls without reference capture')
        if host_controls and through!='cmos-host' and (not window or reference_frames or through!='first-fight'):
            raise ValueError('Host controls require --window --through first-fight without reference capture')
        if through not in ('first-draw','next-wait','first-fight','cmos-boot','cmos-startup','cmos-draw','cmos-host'):raise ValueError('Unknown translated frontier: '+through)
        if audio and not window:raise ValueError('Audio playback requires --window')
        if settings is not None and not window:raise ValueError('Launcher Settings requires --window')
        if window and reference_frames:raise ValueError('Select window or integral reference verification')
        identities={p:quality.sha(__import__('tools.setup.mk2',fromlist=['source']).source(p) if Path(root).resolve()==quality.ROOT.resolve() else root/p) for p in files}
        receipt['sources']=identities
        selected=read_settings(settings) if settings is not None else None
        if selected is not None:dsw_word=selected['dsw_word']
        free_play=bool(int.from_bytes(loaded_cmos[0x1090:0x1098:2],'big')) if loaded_cmos is not None else False
        if dsw_word!=0xfc7d or free_play:
            frontier('cmos-draw',dsw_word,free_play)
            if loaded_cmos is None:raise ValueError('Operator DIP requires an existing persisted CMOS profile')
            if reference_frames:raise ValueError('Integral raster reference for changed operator DIP is not qualified; use operator_reference.py for original CPU/DSP boundaries')
            if through=='cmos-host':raise ValueError('Finite cmos-host diagnostic remains fixed-DIP; use the public cmos-live runtime for operator DIP')
        if selected is not None:receipt['settings']=selected
        receipt["roms"]=foundation.verify_roms(rom_directory)
        env=environment(selected)
        if audio:
            env.setdefault('MK1_MUSIC_MODE','original')
            env['MK1_AUDIO_LATENCY']=selected['audio_latency']['requested'] if selected is not None else 'balanced'
            if selected is not None:
                selected['audio_latency'].update(applied=True,reason='shared_windows_pcm_output')
        with quality.scratch_tree() as (scratch,cleanup):
            receipt["cleanup"]=cleanup
            exe=scratch/"mk2-native-check.exe"
            flags=[quality.DEFAULT_CC,"-std=c++17","-O2","-municode","-Wall","-Wextra","-Werror",
                   "-Wno-unused-parameter","-Wno-missing-field-initializers","-pthread",
                   "-I"+str(root/"native/include")]
            libs=["-l"+name for name in ("bcrypt","user32","gdi32","winmm","d3d12","dxgi","d3dcompiler","dxguid","windowscodecs","ole32","uuid")]
            build=quality.command(flags+[root/p for p in sources]+libs+["-o",exe],scratch,timeout=240,env=env)
            receipt["build"]=quality.compact(build)
            if not build["passed"]:raise RuntimeError("Native first-draw diagnostic build failed")
            options=['--frames',scratch/'native.frames'] if reference_frames else (["--window"] if window else [])
            if audio:options.append('--audio')
            if host_controls:options.append('--host-controls')
            if loaded_cmos is not None:
                image=scratch/'session-input.cmos'
                image.write_bytes(loaded_cmos)
                options+=['--cmos-image',image]
            if through!='first-draw':options+=['--through',through]
            if dsw_word!=0xfc7d:options+=['--dsw-word',str(dsw_word)]
            result=quality.command([exe,rom_directory]+options,scratch,timeout=120,env=env,
                                   output_limit=256*1024)
            receipt["execution"]=quality.compact(result)
            if not result["passed"]:raise RuntimeError("Native first-draw diagnostic failed")
            if result['discarded_output_bytes']:
                raise RuntimeError('Native diagnostic output truncated')
            observed,events=diagnostic_output(result['stdout'])
            receipt['audio_events']=events
            receipt['native']=observed
            if host_controls:
                if through=='cmos-host':validate_valid_host_boundary(observed)
                else:validate_host_boundary(observed)
                receipt['scope']='mk2-native-cmos-host-check' if through=='cmos-host' else 'mk2-native-host-controls-check'
            else:validate_boundary(observed,through,dsw_word,free_play,cmos_image=loaded_cmos)
            if loaded_cmos is not None:
                encoded=observed.pop('cmos_hex',None)
                if (not isinstance(encoded,str) or len(encoded)!=cmos_profile.SIZE*2 or
                    any(value not in '0123456789abcdef' for value in encoded)):
                    raise ValueError('Native CMOS transport requires exactly 16384 bytes')
                pending_cmos=bytes.fromhex(encoded)
            if through!='first-draw':
                if not host_controls:receipt['scope']=f'mk2-native-{through}-check' if loaded_cmos is not None else 'mk2-native-translated-continuation-check'
                receipt['integral_continuation_qualification']=False
            if observed.get('audio_playback') is not audio:
                raise RuntimeError('Native diagnostic audio mode differs from request')
            if audio:
                pcm=observed['audio']
                blocks=observed['cpu34'][2]*16//2000000
                if (pcm['source_rate']!=31250 or pcm['output_rate']!=48000 or pcm['channels']!=(2 if env.get('MK1_MUSIC_MODE')=='remastered' else 1) or
                    pcm['source_samples']<blocks*625+1 or pcm['output_samples']<blocks*960+1 or
                    pcm['submitted']!=pcm['output_samples'] or pcm['completed']!=pcm['submitted']):
                    raise RuntimeError('Native Windows PCM delivery incomplete')
                if pcm['underruns']:
                    raise RuntimeError('Native Windows PCM underruns: '+str(pcm['underruns']))
            if window:
                presentation=observed["presentation"]
                clean_host_stop=through=='cmos-host' and observed['host_controls']['stop_requested'] is True
                gpu_frames=presentation["gpu_validation_frames"]
                fighter_frames=presentation["bicubic_fighter_validation_frames"]
                if (presentation["renderer"]!="D3D12-DXGI-flip" or
                    gpu_frames>2+fighter_frames or fighter_frames>1 or
                    (not gpu_frames and not clean_host_stop) or presentation["gpu_validation_mismatches"]):
                    raise RuntimeError("Shared DX12 shader validation incomplete or mismatched")
                if selected is not None:
                    video=selected['video']
                    expected={'scalefx_amount':int(env['MK1_SCALEFX']),'crt_preset':env['MK1_CRT_PRESET'],'filter':env['MK1_FILTER'],
                        'scanlines':float(env['MK1_SCANLINES']),'sync_requested':video['sync'],
                        'color_mode':video['color'],'brightness':video['brightness'],
                        'bezel':bool(video['bezel']),'high_refresh':bool(video['highrefresh']),
                        'fullscreen_start':bool(video['fullscreen']),'scale_mode':video['scalemode'],
                        'cas_amount':video['cas'],'bilateral_amount':video['bilateral'],'smaa':bool(video['smaa'])}
                    if any(presentation[key]!=value for key,value in expected.items()):
                        raise RuntimeError('Launcher Settings differs from effective DX12 presentation')
            if reference_frames:
                if loaded_cmos is None:
                    receipt['reference']=capture_screen(scratch,rom_directory,foundation.read_manifest(),through)
                else:
                    receipt['reference']=capture_screen(scratch,rom_directory,foundation.read_manifest(),through,cmos_image=loaded_cmos)
                if receipt['reference']['cpu34']!=observed['cpu34']:
                    raise ValueError('Native and reference terminal CPU34 differ')
                if pending_cmos is not None:
                    original_cmos=cmos_profile._read(scratch/'frame-cmos.bin')
                    if original_cmos!=pending_cmos:
                        raise ValueError('Native and reference boundary CMOS bytes differ')
                    receipt['reference']['CMOS_bytes_compared']=len(original_cmos)
                    receipt['reference']['CMOS_sha256']=hashlib.sha256(original_cmos).hexdigest()
                with (scratch/'native.frames').open('rb') as left,(scratch/'reference.frames').open('rb') as right:
                    receipt['frame_comparison']=compare_frames(left,right,expected_frames=observed['frames'])
                if [row['sha256'] for row in receipt['frame_comparison']['frames']]!=observed['frame_sha256']:
                    raise ValueError('Native frame transport differs from its live scanout')
            if any(quality.sha(__import__('tools.setup.mk2',fromlist=['source']).source(p) if Path(root).resolve()==quality.ROOT.resolve() else root/p)!=want for p,want in identities.items()):
                raise RuntimeError("Source changed while running native diagnostic")
            receipt["native"]=observed
            receipt["passed"]=True
        if receipt['cleanup'].get('exists_after') is not False:
            raise RuntimeError('Native session scratch cleanup incomplete; CMOS save rejected')
        if loaded_cmos is not None:
            receipt['persistence']['saved_sha256']=cmos_profile.save(profile_directory,pending_cmos,
                expected_sha256=receipt['persistence']['loaded_sha256'])
            receipt['persistence']['committed']=True
    except Exception as error:
        receipt['passed']=False
        receipt["error"]=str(error)
    receipt["passed"]=receipt["passed"] and receipt.get("cleanup",{}).get("exists_after") is False
    return receipt


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-directory",type=Path,default=quality.ROOT/"roms/mk2")
    parser.add_argument("--window",action="store_true")
    parser.add_argument("--reference-frames",action="store_true")
    parser.add_argument("--settings",type=Path,help="read the selected video profile from the Launcher/MK2 INI, requires --window; no profile writes")
    parser.add_argument('--audio',action='store_true',help='play the finite original DCS timeline through Windows PCM; requires --window')
    parser.add_argument('--host-controls',action='store_true',
                        help='finite keyboard/XInput; requires --window and first-fight, or cmos-host with an existing --profile; rejects unknown paths')
    parser.add_argument('--profile',type=Path,help='existing isolated CMOS directory; requires a supported CMOS frontier; cmos-host also requires --window --host-controls')
    parser.add_argument('--dsw-word',type=lambda value:int(value,0),default=0xfc7d,
                        help='original blood/violence DIP word for a persisted CMOS frontier; other DIP bits are rejected')
    parser.add_argument('--through',choices=('first-draw','next-wait','first-fight','cmos-boot','cmos-startup','cmos-draw','cmos-host'),default='first-draw',
                        help='finite translated frontier; cmos-host uses live inputs and cooperative stop; no complete-gameplay claim')
    args=parser.parse_args()
    receipt=run(args.rom_directory.resolve(),args.window,args.reference_frames,args.settings,args.audio,args.through,args.host_controls,args.profile,args.dsw_word)
    stage=quality.ROOT/"build/native/mk2-native-20261101"
    stage.mkdir(parents=True,exist_ok=True)
    name="reference-frames" if args.reference_frames else ("window-"+os.environ.get("MK1_CRT_PRESET","off") if args.window else "headless")
    # Preset values enter filenames only after the presenter's own validation succeeds.
    if name not in {"headless","reference-frames",*("window-"+p for p in ("off","bvm-1080","bvm-4k","bvm-soft","energy"))}:
        name="window-invalid"
    if args.settings is not None:name='launcher-settings'
    if args.audio:name+='-audio'
    if args.host_controls:name+='-host-controls'
    if args.through!='first-draw':name+='-'+args.through
    if args.profile is not None:name+='-profile'
    path=stage/(name+".json")
    path.write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"passed":receipt["passed"],"receipt":str(path),"error":receipt.get("error"),
                      "native":receipt.get("native"),"cleanup":receipt.get("cleanup")}))
    return 0 if receipt["passed"] else 1


if __name__=="__main__":raise SystemExit(main())
