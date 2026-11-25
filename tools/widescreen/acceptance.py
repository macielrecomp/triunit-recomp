"""Widescreen 16:9 acceptance, MK1, MK2 and MK3, in one command: python tools/widescreen/acceptance.py [--game mk1|mk2|mk3] [--all]

Each legal replay runs twice in the Remastered edition, without and with the widescreen shadow pass (its diagnostics on,
TRIUNIT_WIDE_DEBUG), and checks:
  1. the game state is identical in every frame (MK1: CPU, cycles, RAM, VRAM, palette per frame; MK2: VRAM and palette per
     frame, CPU and RAM at the replay's end; MK3: CPU, cycles, RAM, VRAM, CMOS, palette and PCM per frame) and so are the
     shown frames, PCM and CMOS;
  2. the wide frame's 400 centre columns are the arcade frame bit for bit in every frame whose wide camera is the
     arcade's, before any CRT filter (a frame with an offset window or a held far plane is counted apart); in MK1 two
     kinds of pixel are counted apart too: a scenery block the arcade has not drawn yet at its edge columns (filled; MK3
     counts it too) and, on a screen with soft sides, a line's black end where the soft side starts (joined);
  3. no fight frame shows a margin pixel no draw wrote (the sentinel); masked pixels (edge policy) are counted;
  4. every margin pixel belongs to a draw of the game meant to cover that column (no wrap, copy, mirror or invented pixel);
  5. each scenery layer moves by the same columns per frame in the margins as in the centre;
  6. MK1 and MK3: with the camera still, every scenery list's camera is still;
  7. MK1 and MK3: no fight frame with a lit picture masks more than a speck of its margins (a black bar or block over a sky);
all of it for MK1, MK2 and MK3 (--all adds the long replays that visit every arena).
It prints the edge coverage table (per arena and scenery layer: the fewest margin columns its art covered) and exits 1 on
any failure. Needs the authenticated ROMs and the setup's generated code; nothing is written outside a temporary tree."""
import argparse,json,os,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.quality import run as q

MK1_TAPES=['versus-walls','versus-corner-win','versus-jumps']   # both walls of an arena; a fight won near the right end; diagonal jumps wall to wall
MK1_ALL=['campaign-prefix']


def table(title,rows):
    print(title)
    print('  arena (camera limits)   layer  frames  camera seen    left cols  right cols')
    for low,high,layer,frames,seen_low,seen_high,left,right in sorted(rows):
        print(f'  {low:>5}..{high:<5}          {layer:>5}  {frames:>6}  {seen_low:>5}..{seen_high:<5}  {left:>9}  {right:>10}')


def mk1(all_tapes):
    from tools.mk1 import routes
    from tools.setup import mk1
    mk1.inc_dir();seed=mk1.seed_path().resolve()
    objects=json.loads((ROOT/'tools/mk1/runtime-inputs.json').read_text('utf-8'))['objects']
    failures=[]
    with q.scratch_tree() as (scratch,cleanup):
        srcs=[ROOT/'native/tests/mk5_widescreen_tests.cpp']+[ROOT/p for p in objects if Path(p).stem not in routes.EXCLUDE]
        exe,_=routes.build(scratch,srcs)
        env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ['PATH'],'TEMP':str(scratch),'TMP':str(scratch),'MK1_GAME_VOLUME':'0'}
        chosen=os.environ.get('ACCEPT_TAPES')   # a comma-separated subset (diagnostics)
        for name in chosen.split(',') if chosen else MK1_TAPES+(MK1_ALL if all_tapes else []):
            tape=scratch/f'{name}.tsv';tape.write_bytes(routes.render_tape(routes.HERE/'tapes'/f'{name}.json'))
            r=q.command([exe,ROOT,tape,seed],scratch,cwd=ROOT,env=env,timeout=7200,output_limit=1<<20)
            lines=[l for l in r['stdout'].splitlines() if l.startswith('{')]
            result=json.loads(lines[-1]) if lines else {}
            print(f'MK1 {name}:',r['passed'] and 'PASS' or 'FAIL',{k:v for k,v in result.items() if k!='coverage'})
            for line in ((r.get('stderr') or '')+r['stdout']).splitlines():
                if line.startswith('parallax'):print('  ',line)
            if not r['passed']:failures.append(f'MK1 {name}: '+(r.get('stderr') or r['stdout'])[-800:].strip())
            if result.get('coverage'):table(f'MK1 {name}: edge coverage',result['coverage'])
    assert not cleanup['exists_after']
    return failures


def main():
    parser=argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game',choices=('mk1','mk2','mk3'));parser.add_argument('--all',action='store_true')
    args=parser.parse_args()
    failures=[]
    if args.game in (None,'mk1'):failures+=mk1(args.all)
    if args.game in (None,'mk2'):
        from tools.widescreen import mk2_acceptance
        failures+=mk2_acceptance.run(args.all,table)
    if args.game in (None,'mk3'):
        from tools.widescreen import mk3_acceptance
        failures+=mk3_acceptance.run(args.all,table)
    for f in failures:print('FAIL',f)
    print('widescreen acceptance:','FAIL' if failures else 'PASS')
    return 1 if failures else 0


if __name__=='__main__':sys.exit(main())
