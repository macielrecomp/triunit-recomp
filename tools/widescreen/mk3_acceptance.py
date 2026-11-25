"""MK3 part of the widescreen acceptance (tools/widescreen/acceptance.py): the native MK3 (tools/mk3/preview.py, built once
into a scratch tree) runs each legal tape twice in the Remastered edition, without and with the wide layer and its
diagnostics (--wide, TRIUNIT_WIDE_DEBUG). Per presented frame it logs the frame's and the palette's hash, the host PCM
hash, the hash of main RAM, video RAM, CMOS and the CPU registers, the mode, camera and camera limits, and with the
layer the centre against the arcade frame, the margin audit, the parallax and still-camera checks and the measured
scenery layers. The same tape without and with the layer must log the same game; the criteria are checked here."""
import os,sys
from collections import defaultdict
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))

# The mode word's fight values: 2 the fight, 6 and 7 the round's start, 3 the match's end, 5 the round's end, 17 a
# finishing move.
FINISHER=17
FIGHT_MODES={2,3,5,6,7,FINISHER}
SAME=('rgb','prev','palette','cycle','pcm','state','scenery','fighters','blood','rm','game')
SPECK=16   # masked margin pixels a lit fight frame may show (criterion 7)


def replays(all_replays):
    """name -> (rows, frames). Default: the route to the first fight, diagonal jumps from wall to wall in three arenas and a
    finishing move; --all: every arena the coin-in time reaches, a campaign of matches through the arenas after the
    ninth (with the versus and continue screens), a finishing move with a screen flash and one in the tower."""
    from tools.mk3 import versus
    from tools.widescreen import mk3_tapes
    route=[tuple(map(int,l.split())) for l in (ROOT/'tools/mk3/fight_route.tape').read_text('utf-8').splitlines() if l and not l.startswith('#')]
    chosen=os.environ.get('ACCEPT_MK3')   # a comma-separated subset (diagnostics)
    names=['fight-route','jumps-7','jumps-3','jumps-5','finisher-r1fin01']+(
        [f'jumps-{a}' for a in (1,2,4,6,8,9)]+['campaign-9','finisher-r1fin00','finisher-r1fin08'] if all_replays else [])
    out={}
    for name in chosen.split(',') if chosen else names:
        kind,_,arena=name.partition('-')
        out[name]=((route,3000) if name=='fight-route' else mk3_tapes.campaign(int(arena)) if kind=='campaign' else
                   mk3_tapes.finisher(arena) if kind=='finisher' else mk3_tapes.jumps(int(arena)))
    return out


def black():
    h=0xcbf29ce484222325
    for _ in range(400*254):h=(h*0x100000001b3)&0xffffffffffffffff   # the frame hash of a black screen (HashRgb)
    return f'{h:016x}'


def run(all_replays,table):
    from tools.mk3 import preview
    from tools.widescreen import mk3_tapes
    failures=[];unlit=black();suite=defaultdict(int)
    with preview.Preview() as p:
        for name,(rows,frames) in replays(all_replays).items():
            tape=p.temp/f'{name}.tape';tape.write_text(mk3_tapes.text(rows),encoding='utf-8')
            options=('--edition','remastered','--hashes','--state-hash','--frames',frames,'--tape',tape)
            plain=p.run(*options,timeout=14400,output_limit=1<<30)
            wide=p.run(*options,'--wide',timeout=14400,output_limit=1<<30,window_env={'TRIUNIT_WIDE_DEBUG':'1'})
            bad=[];fp,fw=plain['frames'],wide['frames']
            first=next((a['frame'] for a,b in zip(fp,fw) if any(a.get(k)!=b.get(k) for k in SAME)),None)
            if len(fp)!=len(fw) or first is not None:bad.append(f'1: widescreen changed the game (first frame {first}, {len(fp)}/{len(fw)} frames)')
            for k in ('cycles','frontier','last_frame','audio','dcs'):
                if plain['summary'].get(k)!=wide['summary'].get(k):bad.append(f'1: the run ends differently ({k})')
            total=defaultdict(int);arena=defaultdict(int);coverage={};worst=(0,0)
            for x in fw:
                w=x['wide'];mode,camera,low,high=x['game'];fight=mode in FIGHT_MODES
                shown,sentinel,masked,outside,outside_real,generated,stale=w['audit']
                for k,v in (('centre',w['centre']),('filled',w['filled']),('sentinel',sentinel),('masked',masked),('outside',outside),('generated',generated),
                            ('stale',stale),('shifted',w['shift']!=0),('whole',w['whole']),('soft',w['soft'])):
                    total[k]+=v
                    if fight:arena[k]+=v
                arena['frames']+=fight;arena['measured']+=fight and bool(w.get('bands'))
                arena['finisher']+=mode==FINISHER;arena['finisher_soft']+=mode==FINISHER and w['soft']
                if fight and not w['soft'] and masked>SPECK and x['rgb']!=unlit:
                    arena['masked_frames']+=1
                    if masked>worst[0]:worst=(masked,x['frame'])
                if fight:
                    for layer,left,right in w.get('bands',()):
                        c=coverage.setdefault((low,high,layer),[low,high,layer,0,camera,camera,67,67])
                        c[3]+=1;c[4]=min(c[4],camera);c[5]=max(c[5],camera);c[6]=min(c[6],left);c[7]=min(c[7],right)
            last=fw[-1]['wide'] if fw else {};checks=fw[-1].get('checks',[0,0,0,0]) if fw else [0,0,0,0]
            if total['centre']:bad.append(f"2: the centre differs from the arcade frame ({total['centre']} pixels)")
            if arena['sentinel']:bad.append(f"3: a fight frame shows {arena['sentinel']} margin pixels no draw wrote")
            if total['outside'] or total['generated'] or arena['stale']:bad.append('4: a margin pixel does not belong to a draw meant to cover its column')
            if checks[1]:bad.append(f'5: parallax checked {checks[0]}, differ {checks[1]}')
            if checks[3]:bad.append(f'6: a scenery plane moved while the camera stood still ({checks[3]} builds of {checks[2]})')
            if arena['masked_frames']:bad.append(f"7: {arena['masked_frames']} lit fight frames mask their margins (worst: {worst[0]} pixels, frame {worst[1]})")
            if arena['finisher_soft']:bad.append(f"8: {arena['finisher_soft']} frames of a finishing move show soft sides instead of the arena")
            if name=='finisher-r1fin01' and not arena['finisher']:bad.append('8: no finishing move was reached')
            if last.get('faults'):bad.append(f"the shadow pass faulted {last['faults']} times (PC {last.get('fault_pc',0):08x})")
            print(f'MK3 {name}:','FAIL' if bad else 'PASS',{'frames':len(fw),'faults':last.get('faults'),'updater_skips':last.get('skips'),**total,
                   'parallax':checks[:2],'still':checks[2:],'fight_frames':arena['frames'],'fight_measured':arena['measured'],
                   'fight_sentinel':arena['sentinel'],'fight_stale':arena['stale'],'fight_masked':arena['masked'],'masked_frames':arena['masked_frames'],
                   'finisher_frames':arena['finisher'],'finisher_soft':arena['finisher_soft']})
            if coverage:table(f'MK3 {name}: edge coverage',list(coverage.values()))
            failures+=[f'MK3 {name}: {m}' for m in bad]
            suite['parallax']+=checks[0];suite['fight']+=arena['frames']
    if not suite['fight']:failures.append('MK3: 3: no fight frame was reached')
    if not suite['parallax']:failures.append('MK3: 5: no parallax comparison was made')
    return failures
