"""From-reset chronological PCM against the authenticated MAME Lua sound hooks.

Only compact receipts survive the exclusive scratch guard. Hooks observe actual
DMADAC/speaker float samples; no guest memory, clock or PC is imported or changed.
"""
import argparse,hashlib,json,os,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.quality import run as quality
from tools.mk2 import native,run,batch_reference,reference

LUA=r'''local machine=manager.machine
local main=machine.devices[":maincpu"]
local port=machine.ioport.ports[":IN0"]
local button=port.fields["P1 High Punch"]
assert(button.mask==0x10)
local events,requested,previous={},0,port:read()
local function tick() return math.floor(machine.time:as_double()*100000000+0.5) end
local tap=main.spaces["program"]:install_read_tap(0x01a80000,0x01a8000f,"mk2-audio-input",function()
    local pc,t=main.state["PC"].value,tick()
    if pc==0xffa5aef0 and t==8096368 then
        assert(requested==0 and #events==0);requested=1
    elseif pc==0xffa5b130 and t==9140064 then
        assert(requested==1 and #events==2);requested=0
    else return end
    button:set_value(requested)
    events[#events+1]={t,requested,port:read(),0}
end)
local frame=emu.add_machine_frame_notifier(function()
    local value=port:read()
    if value~=previous then
        events[#events+1]={tick(),requested,value,1};previous=value
    end
end)
local dac=assert(machine.devices[":dcs:dac"])
local enabled
for name,index in pairs(dac.items) do
    if name:sub(-10)=="/m_enabled" then assert(not enabled);enabled=emu.item(index) end
end
assert(enabled and enabled.count==1)
local source=assert(io.open("reference-dac.bin","wb"))
local speaker=assert(io.open("reference-speaker.bin","wb"))
local blocks,speaker_blocks={},{}
local transition=false
local source_end
machine.sounds[":dcs:dac"].hook=true
machine.sounds[":speaker"].hook=true
emu.register_sound_update(function(data)
    local t=tick()
    assert(data[":dcs:dac"] and data[":speaker"])
    for tag,channels in pairs(data) do
        assert(#channels==1)
        local samples=channels[1]
        if tag==":dcs:dac" then
            local rate=enabled:read(0)==0 and not transition and 48000 or 31250
            if rate==48000 then
                for _,value in ipairs(samples) do assert(value==0,"cold 48000Hz prefix is not silent") end
            else
                transition=true
                local ending=math.floor(t/3200)+1
                local first=ending-#samples
                if not source_end then
                    assert(first==1875 and #blocks==3,"cold silent rate-change prefix differs")
                    for n=1,first do assert(source:write(string.pack("<f",0))) end
                else
                    assert(first==source_end,"source block gap/overlap")
                end
                for _,value in ipairs(samples) do assert(source:write(string.pack("<f",value))) end
                source_end=ending
            end
            blocks[#blocks+1]={t,rate,#samples}
        elseif tag==":speaker" then
            for _,value in ipairs(samples) do assert(speaker:write(string.pack("<f",value))) end
            speaker_blocks[#speaker_blocks+1]={t,#samples}
        else error("unexpected hooked audio device") end
    end
end)
mk2_audio_stop_subscription=emu.add_machine_stop_notifier(function()
    tap:remove();assert(frame)
    assert(transition);assert(source:close());assert(speaker:close())
    assert(source_end==6251)
    local f=assert(io.open("audio-meta.json","w"));local e,b,s={},{},{}
    for _,row in ipairs(events) do e[#e+1]="["..table.concat(row,",").."]" end
    for _,row in ipairs(blocks) do b[#b+1]="["..table.concat(row,",").."]" end
    for _,row in ipairs(speaker_blocks) do s[#s+1]="["..table.concat(row,",").."]" end
    assert(f:write('{"inputs":['..table.concat(e,",")..'],"dac_blocks":['..table.concat(b,",")..
        '],"speaker_blocks":['..table.concat(s,",")..']}'))
    assert(f:close())
end)
'''


def compare_float32(actual,expected,count,label):
    if len(actual)!=count*4 or len(expected)!=count*4:
        raise ValueError(f'{label}: incomplete/extra samples ({len(actual)//4}/{len(expected)//4}, expected {count})')
    if actual!=expected:
        first=next(i for i in range(count) if actual[i*4:i*4+4]!=expected[i*4:i*4+4])
        a,b=(struct.unpack_from('<f',data,first*4)[0] for data in (actual,expected))
        raise ValueError(f'{label}: first unequal float32 at {first}: {a}/{b}')
    return {'samples_compared':count,'unequal_float32_bits':0,'sha256':hashlib.sha256(actual).hexdigest(),
            'nonzero_samples':sum(value!=0 for value, in struct.iter_unpack('<f',actual))}


def validate_blocks(meta):
    expected_dac=[[n*2000000,48000 if n<4 else 31250,
                   961 if n==1 else 960 if n<4 else 626 if n==4 else 625] for n in range(1,11)]
    expected_speaker=[[n*2000000,961 if n==1 else 960] for n in range(1,11)]
    if meta.get('dac_blocks')!=expected_dac or meta.get('speaker_blocks')!=expected_speaker:
        raise ValueError('Missing, extra or mistimed DAC/speaker block, or rate-change phase differs')


def compare_pcm16(actual,speaker):
    if len(speaker)%4:raise ValueError('Truncated speaker float32')
    expected=b''.join(struct.pack('<h',max(-32768,min(32767,int(value*32768))))
                      for value, in struct.iter_unpack('<f',speaker))
    if actual!=expected:raise ValueError('PCM16 conversion differs from every-sample MAME speaker comparison')
    return {'samples_compared':len(speaker)//4,'unequal_samples':0,'sha256':hashlib.sha256(actual).hexdigest()}


def finalize_result(result):
    result['passed']=bool(result.get('passed') and not result.get('error') and
                          result.get('cleanup',{}).get('exists_after') is False)
    return result['passed']


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,default=Path('build/native/mk2-dcs-audio-20261101/reference.json'))
    args=parser.parse_args();result={'passed':False,'scope':'cold reset through first draw; actual DAC31250 and mono speaker48000, first200ms inclusive'}
    try:
        run.verify_roms(run.ROOT/'roms/mk2')
        source_manifest=json.loads((run.ROOT/'docs/investigation/decoder-research/adsp-sources.json').read_text())
        result['upstream_sources']={}
        for name in ('dmadac.cpp','dmadac.h','disound.cpp','sound.cpp'):
            entry=next(row for row in source_manifest['files'] if row['file']==name)
            actual=quality.sha(run.ROOT/'docs/investigation/decoder-research/upstream'/name)
            if actual!=entry['sha256']:raise ValueError('Pinned audio source identity differs: '+name)
            result['upstream_sources'][name]=actual
        if quality.sha(batch_reference.MAME)!=batch_reference.MAME_SHA:raise ValueError('MAME executable identity differs')
        with quality.scratch_tree() as (scratch,cleanup):
            result['cleanup']=cleanup
            inputs=[*run.SOURCES,*run.HEADERS,'native/tests/mk2_dac_boot_tests.cpp','native/audio/williams_resampler.hpp','native/audio/sinc_resampler.hpp','native/audio/studio_master.hpp','native/mk2/mk2_audio.hpp',
                    'tools/mk2/dac_reference.py','tools/mk2/native.py']
            before={name:quality.sha(__import__('tools.setup.mk2',fromlist=['source']).source(name)) for name in inputs}
            native.prepare_reference_roms(run.ROOT/'roms/mk2',scratch/'mk2',run.read_manifest())
            (scratch/'audio.lua').write_text(LUA,encoding='ascii')
            registers=[f'a{n}' for n in range(15)]+['sp']+[f'b{n}' for n in range(15)]
            fmt='MK2_AUDIO_CPU '+' '.join(['%X']*34)+'\\n'
            script='focus :maincpu\n'
            for pc,cycle in ((0xffa5aef0,506023),(0xffa5b130,571254)):
                script+=f'bpset {pc:x},totalcycles=={cycle:x},{{printf "MK2_AUDIO_INPUT %X\\n",w@1a80000;g}}\n'
            script+=f'bpset ff80dd30,totalcycles=={1261260:x},{{printf "{fmt}",pc,st,totalcycles,'+','.join(registers)+';quit}\ngo\n'
            (scratch/'audio.cmd').write_text(script,encoding='ascii')
            cc=quality.DEFAULT_CC;env={'PATH':str(cc.parent)+os.pathsep+os.environ.get('PATH','')}
            sources=[name for name in run.SOURCES if '/tests/' not in name and not name.endswith('mk2_task_batch.cpp')]
            build=quality.command([cc,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(run.ROOT/'native/include'),
                run.ROOT/'native/tests/mk2_dac_boot_tests.cpp',*[run.ROOT/name for name in sources],'-lbcrypt','-o',scratch/'native.exe'],scratch,env=env,timeout=180)
            result['build']=quality.compact(build)
            if not build['passed']:raise RuntimeError('Native PCM test compilation failed: '+build['stdout'][-3000:])
            execution=quality.command([scratch/'native.exe',run.ROOT/'roms/mk2',scratch/'native-pcm.bin'],scratch,env=env,timeout=120)
            result['native_execution']=quality.compact(execution)
            if not execution['passed']:raise RuntimeError('Native PCM observation failed: '+execution['stdout'][-3000:])
            result['native']=json.loads(execution['stdout'])
            capture=quality.command([batch_reference.MAME,'mk2','-noreadconfig','-nowriteconfig','-rompath',scratch,
                '-video','none','-sound','none','-nothrottle','-skip_gameinfo','-noplugins','-frameskip','0',
                '-debug','-debugger','windows','-debuglog','-debugscript','audio.cmd','-autoboot_delay','0',
                '-autoboot_script','audio.lua','-nvram_directory',scratch/'nvram','-cfg_directory',scratch/'cfg',
                '-snapshot_directory',scratch/'snap','-homepath',scratch/'home','-seconds_to_run','1'],scratch,cwd=scratch,timeout=120)
            result['reference_execution']=quality.compact(capture)
            result['reference_debug_tail']=(scratch/'debug.log').read_text()[-2000:] if (scratch/'debug.log').exists() else ''
            if not capture['passed'] or capture['discarded_output_bytes']:raise RuntimeError('MAME PCM capture failed: '+capture['stdout'][-3000:])
            for line in capture['stdout'].splitlines():reference.reject_mame_diagnostic(line)
            lines=(scratch/'debug.log').read_text().splitlines()
            for line in lines:reference.reject_mame_diagnostic(line)
            cpu=[[int(value,16) for value in line.split()[1:]] for line in lines if line.startswith('MK2_AUDIO_CPU ')]
            meta=json.loads((scratch/'audio-meta.json').read_text())
            if len(cpu)!=1 or len(cpu[0])!=34 or cpu[0][0]!=0xff80dd30 or cpu[0][2]!=1261260:raise ValueError('MAME terminal CPU boundary differs')
            if meta['inputs']!=[[8096368,1,65535,0],[9139625,1,65519,1],[9140064,0,65519,0],[10967550,0,65535,1]]:raise ValueError('Legal input timeline differs')
            validate_blocks(meta)
            if cpu[0]!=result['native']['cpu34']:raise ValueError('Complete terminal CPU34 differs from MAME')
            raw=(scratch/'native-pcm.bin').read_bytes()
            result['speaker48000']=compare_float32(raw[:9601*4],(scratch/'reference-speaker.bin').read_bytes(),9601,'speaker48000')
            result['dac31250']=compare_float32(raw[9601*4:(9601+6251)*4],(scratch/'reference-dac.bin').read_bytes(),6251,'DAC31250')
            result['pcm16']=compare_pcm16(raw[(9601+6251)*4:],(scratch/'reference-speaker.bin').read_bytes())
            if not result['speaker48000']['nonzero_samples'] or not result['dac31250']['nonzero_samples']:raise ValueError('Silent-only comparison is insufficient')
            after={name:quality.sha(__import__('tools.setup.mk2',fromlist=['source']).source(name)) for name in inputs}
            if before!=after:raise ValueError('Native source inputs changed during comparison')
            if quality.sha(batch_reference.MAME)!=batch_reference.MAME_SHA:raise ValueError('MAME executable changed during comparison')
            result.update(passed=True,inputs=before,inputs_equal=True,reference_executable_sha256=batch_reference.MAME_SHA,
                          lua_sha256=quality.sha(scratch/'audio.lua'),debug_script_sha256=quality.sha(scratch/'audio.cmd'),
                          cpu34=cpu[0],reference_meta=meta)
    except Exception as error:
        result.update(passed=False,error=str(error))
    finalize_result(result)
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:value for key,value in result.items() if key not in ('inputs','build','reference_execution','native_execution','reference_debug_tail')}))
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
