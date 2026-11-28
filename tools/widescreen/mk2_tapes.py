"""Generated MK2 replays for the widescreen acceptance (tools/widescreen/mk2_acceptance.py): inputs only, built from a
replay of tools/mk2 up to a fight's start. Input rows are [time (frame * FRAME), active-low host word]: player 1's
right is bit 3 and left bit 2, player 2's the same bits eight higher."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
FRAME=1827925
P1_RIGHT,P1_LEFT,P2=8,4,8
IDENTITY=('format','timebase_hz','initial_shared_active_low','program_sha256','factory_cmos_sha256','factory_cmos_bytes')


def walk(source='finisher_replays/natural-f01-stage-a',start=3660,legs=(300,700,450,700,450,600),after=120):
    """The source replay up to frame `start` (its second match, in the red-sky arena, is about to begin), then both
    players walk together, right and left in turns, for `legs` frames each: the camera goes from wall to wall while the
    far objects of the arena fly across, several times."""
    data=json.loads((ROOT/'tools/mk2'/f'{source}.json').read_text())
    rows=[row for row in data['request_rows'] if row[0]<start*FRAME]
    at=start
    for k,frames in enumerate(legs):
        held=(P1_LEFT if k%2 else P1_RIGHT)
        rows.append([at*FRAME,0xffffffff&~(held|held<<P2)]);at+=frames
    rows.append([at*FRAME,0xffffffff])
    return {**{key:data[key] for key in IDENTITY},'request_rows':rows,'end_frame':at+after}


def lost(source='finisher_replays/natural-f01-stage-a',start=660,end=7120):
    """The source replay's coins, player 1's start and choice (player 2 never joins), then player 1 only walks left:
    the machine's fighter wins the match near the arena's left end; the continue and game over screens follow it, and
    then the attract with its shots of several arenas."""
    data=json.loads((ROOT/'tools/mk2'/f'{source}.json').read_text())
    absent=0xff<<P2|1<<21   # player 2's stick, buttons and start
    rows=[]
    for t,v in [*([t,v|absent] for t,v in data['request_rows'] if t<start*FRAME),[start*FRAME,0xffffffff&~P1_LEFT]]:
        if v!=(rows[-1][1] if rows else data['initial_shared_active_low']):rows.append([t,v])
    return {**{key:data[key] for key in IDENTITY},'request_rows':rows,'end_frame':end}


MADE={'walk-red-sky':walk,'lost-left':lost}
