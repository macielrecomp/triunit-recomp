"""Inputs-only MK3 tapes for the widescreen acceptance (tools/widescreen/mk3_acceptance.py): legal versus matches whose
fighters walk to an arena's wall, jump diagonally from wall to wall and back, walk to the other wall, and then P1
punches until the round ends. The first match's arena comes from the coin-in time (tools/mk3/versus.py, the shifts of
tools/mk3/r1_routes.json); `campaign` goes on for several matches (the loser buys back in), so the arenas follow one
another in the game's own order; `finisher` is a legal finishing-move route of tools/mk3/r1_finishes.json. Rows are
(cycle, port, value) as tools/mk3/replay.py reads them."""
import json
from pathlib import Path
from tools.mk3 import versus

ROOT=Path(__file__).resolve().parents[2]
F=versus.FRAME
LEFT,RIGHT,UP,DOWN,HIGH_PUNCH=4,8,1,2,0x10
CYCLE=240+420+420+240+30+28*20   # frames of one pattern


def both(stick):return stick|stick<<8


def pattern(start,cycles,pulse=False):
    """`cycles` patterns from `start`. pulse: each pattern also pulses a coin, both starts and P2's High Punch while P1
    punches, so that a player who lost the match buys back in and picks a fighter."""
    rows=[];c=start
    def hold(in0,frames,in1=0,in2=0):
        nonlocal c
        rows.extend(((c,0,0xffff^in0),(c,1,0xffff^in1),(c,3,0xffff^in2)));c+=frames*F
    for _ in range(cycles):
        hold(both(LEFT),240);hold(both(UP|RIGHT),420);hold(both(UP|LEFT),420);hold(both(RIGHT),240);hold(0,30)
        for k in range(28):   # P1 walks in and punches
            buy=pulse and k%7==0
            hold(RIGHT,10,0,versus.COIN1 if buy else 0)
            hold(HIGH_PUNCH|(HIGH_PUNCH<<8 if pulse and k%7==3 else 0),4,0,versus.START1|versus.START2 if buy else 0);hold(0,6)
    hold(0,1)
    return rows,c


def spec(arena):
    routes=json.loads((ROOT/'tools/mk3/r1_routes.json').read_text('utf-8'))['routes']
    return next(r for r in routes if r.get('arena')==arena and 'joins' not in r)


def jumps(arena,cycles=1,pulse=False):
    """(rows, frames): a versus match on `arena` with `cycles` patterns from 1.5 s after its first round starts."""
    s=spec(arena)
    rows,end=pattern(s['fight']+90*F,cycles,pulse)
    return sorted(list(versus.versus_tape(s['p1'],s['p2'],s['shift'])['tape'])+rows),end//F+60


def campaign(arena=9,cycles=40):
    """Several matches in a row from `arena`: the arenas after it in the game's order."""
    return jumps(arena,cycles,True)


def versus_with(arena,rows,end):
    s=spec(arena)
    return sorted(list(versus.versus_tape(s['p1'],s['p2'],s['shift'])['tape'])+rows),end//F+300


def uppercut(arena=2):
    """(rows, frames): a versus match on `arena` where the fighters walk into each other and P1 uppercuts three times. On
    arena 2 the first one sends P2 up through the ceiling into the next arena (the passage between two arenas)."""
    rows=[];c=spec(arena)['fight']+90*F
    for stick,frames in ((RIGHT|LEFT<<8,60),*((DOWN,10),(DOWN|HIGH_PUNCH,6),(0,60))*3):
        rows.append((c,0,0xffff^stick));c+=frames*F
    return versus_with(arena,rows,c)


def corner(arena=7,frames=1700):
    """(rows, frames): a versus match on `arena` where both fighters hold LEFT and P1 presses High Punch for 4 frames in
    every 20: the match ends at the arena's left end (the wide window offset when the game freezes its last picture)."""
    rows=[];c=spec(arena)['fight']+90*F;end=c+frames*F
    while c<end:rows+=[(c,0,0xffff^(both(LEFT)|HIGH_PUNCH)),(c+4*F,0,0xffff^both(LEFT))];c+=20*F
    return versus_with(arena,rows+[(end,0,0xffff)],end)


def finisher(name):
    """(rows, frames): the route `name` of tools/mk3/r1_finishes.json from reset (tools/mk3/finishes.py) and 10 s more."""
    from tools.mk3 import finishes
    s=next(r for r in json.loads(finishes.SPECS.read_text('utf-8'))['routes'] if r['name']==name)
    return finishes.tape_of(s),s['horizon']//F+600


def text(rows):return ''.join(f'{c} {p} {v}\n' for c,p,v in rows)
