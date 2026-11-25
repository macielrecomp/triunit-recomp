"""M2 DMA lot: isolated Wolf-Unit DMA blitter model (native/mk3/mk3_dma.hpp, done timer in mk3_video.hpp) vs the
pinned Original.

Declared stimulus (from the Original, reset to SECONDS emulated seconds, tools/mk3/reference_dma.py): every DMA
register write in order (DMA_COMMAND writes with instruction-start cycle and in-instruction offset) and, before
each DMA trigger, the video RAM words that changed since the last observation (CPU writes; none bypass the taps
in this scope, or they would fail the DMA delta). The graphics ROMs come from the authenticated set through the
project manifest (roms.verify) and are read by the native side from the ROM files.
Compared against the Original, per trigger: the 18 DMA registers before the write; the complete video RAM after
the DMA, as the exact set of changed words and as a hash of all 524,288 words. Per instruction boundary: every
change of (DMA_COMMAND & 0x8000) | (INTPEND & INT1) - a DMA_COMMAND write is seen at the boundary after its
instruction, a done timer event at cycle e at the first boundary b >= e (previous boundary < e). Negative
controls must each be detected. Also a Lua-only census of all triggers to CENSUS_SECONDS.
"""
import array,json,os,subprocess,sys,time,unittest
from collections import Counter
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.quality import run as q
from tools.mk3 import reference_dma

ROOT=Path(__file__).resolve().parents[2]
RECEIPT=ROOT/'build/native/mk3-m2/dma.json'
SECONDS,CENSUS_SECONDS=4,25
P=(1<<61)-1
VERIFIED={0xc002,0xc082,0x800c}  # mk3::Dma::Verified

def events(ref):
    trigger_at={t['write']:t for t in ref['triggers']}
    lines=[]
    for i,w in enumerate(ref['writes']):
        if i in trigger_at:
            pre=trigger_at[i]['pre'];lines.append(f'P {len(pre)} '+' '.join(f'{k:X}:{v:X}' for k,v in pre.items()))
        off=(w['offset']>>4)&15
        lines.append(f"C {w['cycle']} {w['elapsed']} {off:X} {w['data']:X} {w['mask']:X}" if off==1 else f"R {off:X} {w['data']:X} {w['mask']:X}")
    lines.append(f"E {ref['states'][-1]['boundary']}")
    return lines

def native(build,runs):
    """runs: [(event lines, bias)] -> output lines per run."""
    cc=q.DEFAULT_CC;env={'PATH':str(cc.parent)+os.pathsep+os.environ['PATH']};out=[]
    with q.scratch_tree() as (temp,cleanup):
        exe=temp/'mk3-dma.exe'
        made=q.command([cc,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-I'+str(ROOT/'native/mk3'),'-I'+str(ROOT/'native/include'),
                        ROOT/'native/tests/mk3_dma_tests.cpp','-o',exe],temp,cwd=ROOT,env=env,timeout=180)
        assert made['passed'],q.compact(made);build['seconds']=made['wall_seconds']
        (temp/'gfx.txt').write_text(''.join(f'{o} {s} {p}\n' for p,o,s in reference_dma.graphics_rows()),encoding='utf-8')
        start=time.perf_counter()
        for i,(lines,bias) in enumerate(runs):
            (temp/f'e{i}.txt').write_text('\n'.join(lines)+'\n',encoding='ascii')
            with (temp/f'o{i}.txt').open('wb') as sink:  # stdout exceeds the shared capture limit
                code=subprocess.run([str(exe),str(temp/'gfx.txt'),str(temp/f'e{i}.txt'),str(bias)],stdout=sink,env=dict(os.environ,**env),timeout=600).returncode
            assert code==0,f'native replay exit {code}'
            out.append((temp/f'o{i}.txt').read_text('ascii').splitlines())
        build['replay_seconds']=time.perf_counter()-start
    assert not cleanup['exists_after']
    return out

def compare(ref,out,graphics_hash,registers=True):
    """-> (first difference or None, statistics). registers=False skips the register file (stimulus mutants)."""
    stats=dict(triggers=0,vram_words_changed=0,vram_hashes=0,commands=Counter(),forced_rows=0,change_rows=0,write_events=0,done_events=0,
               unverified_tail_events=0,max_done_slack=0)
    if not out or out[0]!=f'G {graphics_hash}':return f'graphics image hash {out[:1]} differs from {graphics_hash}',stats
    vram=array.array('H',bytes(2*524288));triggers=iter(ref['triggers']);changes=[]
    rows=iter(out[1:])
    for line in rows:
        if line.startswith('REJECT'):return line,stats
        kind,_,rest=line.partition(' ')
        if kind=='S':
            how,cycle,state=rest.split();changes.append((int(cycle)+(how=='W'),int(state),how));continue
        if kind!='T':return 'unexpected native line '+line[:60],stats
        t=next(triggers,None)
        if t is None:return 'native trigger beyond the Original',stats
        k=stats['triggers'];f=rest.split();pixels,done=int(f[0]),int(f[1]);regs=[int(x,16) for x in f[2:]]
        command=ref['writes'][t['write']]['data']
        if registers and regs!=t['registers']:return f'trigger {k} ({command:#x}): native registers {[hex(x) for x in regs]} Original {[hex(x) for x in t["registers"]]}',stats
        for i,v in t['pre'].items():vram[i]=v
        v_line,h_line=next(rows,''),next(rows,'')
        if not v_line.startswith('V ') or not h_line.startswith('H '):return f'trigger {k}: truncated native output',stats
        delta=reference_dma.words(v_line[2:])
        if delta!=t['post']:
            diff=sorted(set(delta.items())^set(t['post'].items()))[:3]
            return f"trigger {k} ({command:#x} at cycle {ref['writes'][t['write']]['cycle']}): video RAM delta of {len(delta)} words differs from the Original's {len(t['post'])}; first {[(hex(i),hex(v)) for i,v in diff]}",stats
        for i,v in t['post'].items():vram[i]=v
        if int(h_line[2:])!=int.from_bytes(vram.tobytes(),'little')%P:return f'trigger {k}: native video RAM hash differs from the Original',stats
        stats['triggers']+=1;stats['vram_words_changed']+=len(delta);stats['vram_hashes']+=1;stats['commands'][hex(command)]+=1
    if next(triggers,None) is not None:return f"native stopped after {stats['triggers']} of {len(ref['triggers'])} triggers",stats
    # Boundary changes: native thresholds v (a write at c is visible from boundary c + 1, a done at e from e).
    native_changes=changes;j=0;state=0
    def advance(limit,expected,where):
        nonlocal j,state
        while j<len(native_changes) and native_changes[j][0]<=limit:
            state=native_changes[j][1];stats['write_events' if native_changes[j][2]=='W' else 'done_events']+=1;j+=1
            if expected is not None and state!=expected:return f'native state {state:#x} at threshold {native_changes[j-1][0]} {where}, Original {expected:#x}'
        return None
    old=0
    for i,s in enumerate(ref['states']):
        if (e:=advance(s['previous'],old,f'before the Original change at boundary {s["boundary"]}')):return e,stats
        if state!=old:return f'native state {state:#x} at boundary {s["previous"]}, Original {old:#x}',stats
        first=j
        if (e:=advance(s['boundary'],None,'')):return e,stats
        if state!=s['state']:return f"Original change to {s['state']:#x} at boundary {s['boundary']} (previous {s['previous']}, pc {s['pc']:x}): native {state:#x}",stats
        for v,_,how in native_changes[first:j]:
            if how=='T':stats['max_done_slack']=max(stats['max_done_slack'],s['boundary']-v)
        stats['forced_rows' if s['forced'] else 'change_rows']+=1
        old=s['state']
    stats['unverified_tail_events']=len(native_changes)-j
    return None,stats

class DmaBlitter(unittest.TestCase):
    def test_native_dma_equals_original(self):
        timings={}
        start=time.perf_counter();ref=reference_dma.capture(SECONDS);timings['reference']=time.perf_counter()-start
        start=time.perf_counter();census=reference_dma.census(CENSUS_SECONDS);timings['census']=time.perf_counter()-start
        image=reference_dma.graphics_image();graphics_hash=int.from_bytes(image,'little')%P
        lines=events(ref)
        # Negative controls. Reference side: one DMA-written pixel. Stimulus side: one palette write, one clip write,
        # the done time +-1 cycle.
        first_skip=next(t for t in ref['triggers'] if ref['writes'][t['write']]['data']==0xc082 and t['post'])
        mutant=dict(ref,triggers=[dict(t,post=dict(t['post'])) for t in ref['triggers']])
        index=ref['triggers'].index(first_skip);word=next(iter(first_skip['post']))
        mutant['triggers'][index]['post'][word]^=1
        def stimulus(offset,choose,value):
            """Change the data of the last write to `offset` before the first trigger that `choose` accepts."""
            for t in ref['triggers']:
                if choose(t):break
            i=max(i for i,w in enumerate(ref['writes'][:t['write']]) if (w['offset']>>4)&15==offset)
            new=[dict(w) for w in ref['writes']];new[i]['data']=value(t,new[i]['data'])
            return events(dict(ref,writes=new)),i
        palette,palette_write=stimulus(8,lambda t:t['post'] and ref['writes'][t['write']]['data']==0xc002,lambda t,d:d^0x0100)
        # Bottom clip (offset 13 with DMA_CONFIG bit 5 set, as every trigger in scope) at the row after the start.
        clip,clip_write=stimulus(13,lambda t:t['post'] and t['registers'][7]>=2 and t['registers'][15]&0x20,lambda t,d:t['registers'][5]&0x1ff)
        build={}
        out,bad_palette,bad_clip,late,early=native(build,[(lines,0),(palette,0),(clip,0),(lines,1),(lines,-1)])
        difference,stats=compare(ref,out,graphics_hash)
        self.assertIsNone(difference)
        self.assertEqual(stats['triggers'],len(ref['triggers']))
        negatives=dict(pixel=compare(mutant,out,graphics_hash)[0],palette_register=compare(ref,bad_palette,graphics_hash,registers=False)[0],
                       clip_register=compare(ref,bad_clip,graphics_hash,registers=False)[0],done_plus_1_cycle=compare(ref,late,graphics_hash)[0],
                       done_minus_1_cycle=compare(ref,early,graphics_hash)[0])
        self.assertTrue(all(negatives.values()),negatives)
        self.assertTrue(all('video RAM' in negatives[k] for k in ('pixel','palette_register','clip_register')),negatives)
        census_commands=Counter()
        for key,count in census['keys'].items():
            command,sx,sy,config=(int(x,16) for x in key.split());census_commands[(command,sx,sy)]+=count
        in_scope=all(c in VERIFIED and sx in (0,0x100) and sy in (0,0x100) for c,sx,sy in census_commands)
        first=ref['triggers'][0];fw=ref['writes'][first['write']]
        RECEIPT.parent.mkdir(parents=True,exist_ok=True)
        RECEIPT.write_text(json.dumps(dict(passed=True,
            scope='isolated Wolf-Unit DMA blitter (dma_w/dma_r/dma_done, unscaled dma_draw) + VideoTiming done timer; stimulus = Original DMA register writes and pre-DMA video RAM changes; no CPU translation',
            program_sha256=ref['program_sha256'],core_sha256=ref['core_sha256'],seed_sha256=ref['seed_sha256'],
            graphics=dict(loaded_bytes=ref['graphics_loaded_bytes'],sha256=ref['graphics_sha256'],region_bytes=0x2000000),
            emulated_seconds=SECONDS,
            first_trigger=dict(command=hex(fw['data']),pc=hex(fw['pc']),cycle=fw['cycle'],elapsed=fw['elapsed'],
                               registers=[hex(x) for x in first['registers']],video_ram_words_changed=len(first['post'])),
            compared=dict(register_writes=len(ref['writes']),triggers=stats['triggers'],commands=dict(stats['commands']),
                trigger_pcs={hex(k):v for k,v in Counter(ref['writes'][t['write']]['pc'] for t in ref['triggers']).items()},
                register_files=stats['triggers'],video_ram_words_changed=stats['vram_words_changed'],full_video_ram_hashes=stats['vram_hashes'],
                prestimulus_words=sum(len(t['pre']) for t in ref['triggers']),closed_by=dict(Counter(t['closed_by'] for t in ref['triggers'])),
                boundary_rows_after_writes=stats['forced_rows'],other_boundary_changes=stats['change_rows'],native_write_events=stats['write_events'],native_done_events=stats['done_events'],
                native_events_after_last_original_change=stats['unverified_tail_events'],max_done_to_boundary_cycles=stats['max_done_slack'],
                last_boundary=ref['states'][-1]['boundary']),
            census=dict(emulated_seconds=CENSUS_SECONDS,triggers=census['triggers'],register_writes=census['register_writes'],
                commands={f'{c:#06x} scale {sx:#x},{sy:#x}':n for (c,sx,sy),n in sorted(census_commands.items())},
                all_in_verified_scope=in_scope),
            negative_controls=negatives,negative_control_writes=dict(palette=palette_write,clip=clip_write),
            timings_seconds=dict(reference_capture=round(timings['reference'],1),census=round(timings['census'],1),native_build=build['seconds'],
                                 native_replays=round(build['replay_seconds'],1),mame_detail=ref['wall_seconds'],mame_census=census['wall_seconds']),
            cleanup=dict(reference=ref['cleanup']['exists_after'],census=census['cleanup']['exists_after'])),indent=2)+'\n',encoding='utf-8')
        self.assertTrue(in_scope,census_commands)

if __name__=='__main__':unittest.main()
