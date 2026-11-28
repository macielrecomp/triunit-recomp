"""Inputs-only MK1 tapes for the widescreen acceptance (tools/widescreen/acceptance.py). `bonus`: a legal versus
session in which P1 wins match after match while P2, who loses, buys back in and picks a fighter; it reaches the bonus
stage that comes between matches. `demos`: a one-player match that P1 loses and does not continue; the attract mode's
demo fights that follow visit other arenas, the bridge over the pit among them (its ring of clouds).
Events are [frame, frames, input mask] as tools/mk1/routes.py renders them."""
import json
from pathlib import Path

HERE=Path(__file__).resolve().parent
UP,DOWN,LEFT,RIGHT,HIGH_PUNCH=1,2,4,8,16      # P1's bits; P2's are these << 8
COIN2,START2=1<<17,1<<21


def bonus(cycles):
    """The versus start of tools/mk1/tapes/versus-jumps.json (to its first fight), then `cycles` times: P1 walks in
    and uppercuts while P2 stands. Every fifth time P2 pays and starts, and two later picks a fighter. A versus match
    is always fought in the first arena; the bonus stage comes after every fifth win."""
    events=json.loads((HERE.parent/'mk1/tapes/versus-jumps.json').read_text('utf-8'))['events']
    events=[e for e in events if e[0]<2561];frame=2561
    def hold(mask,frames):
        nonlocal frame
        events.append([frame,frames,mask]);frame+=frames
    for k in range(cycles):
        buy=k%5==0
        hold(RIGHT|(COIN2 if buy else 0),30);hold(DOWN|(START2 if buy else 0),8)
        hold(DOWN|HIGH_PUNCH|(HIGH_PUNCH<<8 if k%5==2 else 0),6);hold(0,40)
    hold(0,60)
    return events


def demos(frames=15700):
    """The one-player start of tools/mk1/tapes/campaign-prefix.json (to its first fight), then P1 walks to a wall and
    jumps from wall to wall, punching in the air, for as long as the tape lasts: the machine wins the match, nobody
    continues, and the attract mode goes on to its demo fights (the one on the bridge from frame 14875)."""
    events=json.loads((HERE.parent/'mk1/tapes/campaign-prefix.json').read_text('utf-8'))['events']
    events=[e for e in events if e[0]<836];frame=836
    def hold(mask,count):
        nonlocal frame
        events.append([frame,count,mask]);frame+=count
    def jump(stick,count):
        for _ in range(0,count,20):hold(stick,16);hold(stick|HIGH_PUNCH,4)
    while frame<frames:
        hold(LEFT,200);jump(UP|RIGHT,340);jump(UP|LEFT,440);hold(RIGHT,260);hold(0,20)
    return [e for e in events if e[0]<frames]


def write(path,events):
    Path(path).write_text(json.dumps({'columns':['frame','requested_frames','input_mask'],'events':events}),'utf-8')
