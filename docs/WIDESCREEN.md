# Widescreen 16:9 (MK1, MK2 and MK3 Remastered)

The Remastered edition of MK1 rev 5.0, MK2 L3.1 and MK3 rev 2.1 can show a 16:9 picture: the arcade's own 400 columns in the
middle and 67 more on each side. It is purely visual: the game runs exactly as in 4:3. The picture's 400 centre columns
are the arcade frame, bit for bit, with two exceptions: near an arena's ends in a fight, where the wide picture stops at
the end of the arena's art instead of showing black beside it (see *Arena ends* below), and, in MK1 and MK3, where a
scenery block is entering the arcade's screen and the arcade has not drawn it yet (see *Blocks entering the screen*).
MK3 runs on another board (the Wolf Unit) and its arenas are built differently: what it shares with the other two
games and what it does its own way is in *MK3* below.

## Turning it on

In the app, with the Remastered edition selected, **Settings > Display > Screen: Widescreen**. The launcher starts the game with
`MK1_WIDESCREEN=1`, `MK2_WIDESCREEN=1` or `MK3_WIDESCREEN=1`. Without it, or in the Original edition, the game shows the arcade's 4:3 frame,
which is also the safe fallback. Widescreen leaves out the TV frame. The CRT and Phosphor filters work at the new width
and keep whole scanlines.

## How the margins are made

- **Width.** The arcade shows 400×254 pixels on a 4:3 screen. A 16:9 screen holds 400 × (16/9) ÷ (4/3) = 533.3 of those
  pixels, so the picture is 534 columns: 400 + 67 + 67. The pixel's shape does not change.
- **The game's own draws.** Once per frame a *shadow pass* runs the frame's display-list walk again with the game's
  clip widened to the 16:9 window, plus 112 columns on each side. It uses the code the setup generated from your ROM,
  runs on a journal of RAM that is undone afterwards, and spends no cycles. The game's objects (scenery blocks,
  fighters, projectiles, blood) are built and drawn there at the x the game gives them, with each layer's own parallax
  scroll. The game's culling and scenery streamer run with the wider clip, so objects just outside the 4:3 screen are
  drawn too.
- **Colours of objects only the wide picture has (MK1).** An object the arcade has not built has no colours in the
  palette RAM, so it takes them from the ROM palette. While the game fades its palette, those colours take the same
  fade, measured from the palettes the game has loaded (the middle one of their ratios to the ROM).
- **No wraparound.** The margins live in a layer 534 columns wide, separate from the board's 512-column video RAM. A
  draw at x = −40 lands in the left margin, never on the other side. Samplers in the D3D12 presentation clamp; none wraps.
- **The centre is the arcade's.** While the wide camera is the arcade's, the arcade's 400 columns come from the real
  frame (MK1: the real draws; MK2: the shadow's draws, checked against the real frame). The HUD stays in screen space,
  centred on those 400 columns, always.
- **Blocks entering the screen (MK1).** The game builds its scenery blocks for the 4:3 window every 8 frames. While
  the screen scrolls fast (diagonal jumps from wall to wall), a block reaches the arcade's last columns a few frames
  before the game builds it, and the arcade shows the page's background there (a thin streak in the sky's colour over
  a wall). On a 4:3 screen that is the picture's very edge; in 16:9 it would sit inside the picture, next to a margin
  that already shows the block. The shadow pass has built the block for its wider window, so the wide picture draws it
  whole, those columns included. The acceptance tests count these pixels (`filled`).
- **A store that is no draw (MK1).** The game stores a long through a null pointer, which lands on video RAM's first
  four words, 56 columns left of the arcade's screen (`mk5widescreen::kNullWrite`). The wide layer leaves it out, and
  takes its margins from the game's draws alone, never from video RAM's own columns beside the screen.

## Arena ends: the limited wide camera

Each arena's art is finite, and the arcade's camera stops when the 4:3 screen reaches its ends. A 16:9 picture centred
on that camera would then show 67 columns past the art on one side. Three things cannot all hold there: a centre always
equal to the arcade frame, full margins, and nothing invented. Up to v0.9.0 the margins gave way (black). Now the
centre does, and only there:

- **The wide camera stops with the art.** In a fight the wide picture's camera is the arcade's, limited to the arena's
  own camera limits moved in by 67 columns. Away from the ends nothing changes. Within 67 camera columns of an end the
  whole world (scenery, fighters, projectiles, blood) is shown up to 67 columns off the centre, and the picture stays
  full. The offset depends on the camera alone: no ramp, no delay, nothing moves while the camera stands still.
- **The arcade's camera passes its limits a little.** At a wall the game's camera goes up to 3 columns past its own
  limit, and the arcade's last columns then show the page's background past the art's end. The wide camera stays on
  its limit meanwhile (for up to 8 columns), so those columns fall outside the wide picture.
- **Far planes stop at their own ends.** A far plane (sky, walls, crowd) scrolls slower than the world and its art can
  end sooner. The plane's ratio to the world is measured from the game's own scroll velocities, and its art's ends are
  measured from its draws on a canvas wider than the screen. The plane then eases into the camera position where its
  art's end meets the wide screen's edge and stays there.
  A jump scrolls the screen up and shows lines of a plane whose art ends sooner. A stop learned while the plane already
  stands near that end would move it at once, with the camera still, so it is taken only when taking it moves nothing
  (the camera away from that end); until then those lines follow the edge policy below.
- **Such frames come from the shadow's draws.** In a frame whose wide camera is not the arcade's, the world of the whole
  picture comes from the shadow pass, and the HUD from the real draws. The acceptance tests count these frames apart.
- **After the fight.** The round's end, the next round's start and the match's end go on like the fight while the
  arena's camera limits stand: the window follows the camera, back to the middle when the fighters are put back. Once
  the limits are gone (a finisher that frees the camera, the fade) the last offset stays while the camera does.
- **The continue screen (MK1).** When a match is lost the game stops clearing the page and draws its countdown over the
  fight's last picture. If that fight ended near an arena's end, the wide picture keeps that last picture and its
  window while the camera stands (video RAM holds the arcade's window there, not the wide one).
- **Free-camera arenas.** An arena without camera limits from its start stays centred.
- **Attract demos.** The attract has one mode word for its demo fights and its other screens. A demo fight is told by
  two things: its camera limits are an arena's (not the ones the game gives a screen without an arena), and the last
  frame's scenery went on past the 4:3 screen (24 columns or more of a margin, on 32 lines or more). Its window then
  follows the camera like a fight's. A frame that measures no scenery (the screen following an uppercut upwards) is
  still the demo while the camera stands.
- Any other screen stays centred.

## Edge policy (where there is still no art)

Art can still fall short of the edge: a layer narrower than the wide screen, an arena narrower than 534 columns. No
object of the game is mirrored, repeated or stretched there, and no stale video memory is shown.

- A *scenery layer* is one list of the game's display walk (a far plane, the world, the front plane) or, in MK2, one
  callback such as the floor. It is measured on each line where its pixels cover all 400 arcade columns. A side where
  one of the layer's draws reaches past the 534 columns is left as drawn.
- **A line's own pattern goes on.** Past the layer's outermost pixel on a line, the line's last columns are repeated to
  the screen's edge in two cases: most of the layer's short lines on that side (3 in 5) end flat or in a two-column
  dither (a sky's gradient), or they end on one straight edge (9 in 10 at the same column, over 32 lines or more: a
  backdrop picture narrower than the wide screen). All the layer's lines are treated alike: a line that could not be
  measured (its art has a hole inside the arcade's columns) and ends on that same edge goes on too, so a wall that
  stops a few columns short of the arena's end is not left with a dashed strip. A gap that shows at most 24 columns
  past the art (something nearer may cover the rest of the way to the screen's edge) takes the columns next to it (a
  floor's texture a few columns on). Only an arena's lines are continued (a fight, or
  a screen with a far plane's picture): a menu's textured background would come out as streaks.
- **Anything else shows black**, unless something nearer (a fighter, a projectile) is drawn there.
- A margin pixel that no draw wrote since its page came plain from video RAM shows black.
- On a line the frame redrew across all 400 arcade columns, a margin pixel an earlier frame drew and this one did not
  shows black. This stops an older pose of a fighter from staying in the margin.
- **The page's background belongs to a picture that shows it.** Some arenas have no sky of their own: their sky is the
  page's background, the colour the game clears the page with (MK1's Courtyard and Palace Gates, the light behind the
  Throne Room's windows). In a frame whose arcade columns show that background on 32 lines or more, it is part of the
  picture wherever it shows, in the margins too: it is never blackened, whatever covers a line in the arcade's columns
  (the life bars, a fighter in a jump, a roof). In a frame that shows none of it (art over the whole screen, as in
  MK2's arenas), background in a margin is a place without art: it shows black on a line the arcade's columns cover
  whole.

## Screens without an arena

Menus, story and biography screens, the select screen and the like often draw nothing past the 400 columns: their
backgrounds are single pictures the size of the 4:3 screen, or a little wider. When such a screen leaves its margins
unfilled, both margins get **soft sides**: each margin line takes the average colour of the last 4 columns of the
arcade line beside it, blurred over 10 lines up and down and dimmed from 59% beside the picture to 23% at the screen's
edge. Nothing of the picture is copied, so no object can appear twice. In MK1 the soft side starts where the line's
picture ends: when a picture stops a few columns short of the arcade's edge (the select screen, on the right), those
black columns take the side's first colour, so no black line stands between the picture and its side. A screen whose
draws do fill the margins (a title, an arena shown outside a fight) keeps them as drawn.

Some pictures are drawn with no display walk at all, straight to the page (a story picture after an attract demo).
After 3 frames without a walk, with something written meanwhile, the pages drop the window the last walk left on them:
the picture is centred and gets soft sides.

## MK3

MK3's video RAM line is the other two games': 512 columns, the 400 visible ones from column 56. So the wide layer, the
shadow pass (the scenery updater and the display walk run again on journaled RAM, with no cycles, the clip widened and
the scrolls at the wide camera), the limited wide camera, the soft sides of screens without an arena, the colours
taken from the ROM for objects only the wide picture has (with the game's own fade) and the blocks entering the
screen are the same. The differences come from how its arenas and its screens are drawn:

- **The picture ends where the arcade's ends, plane by plane.** Nearly everything in an MK3 arena is a far plane with
  its own parallax (the wall behind the fighters too), and each plane's art is made to measure for what the arcade
  shows of it at the camera's limits: a wall can stop in a ragged edge a few columns short of the arcade's last screen,
  which then shows the page's background past it. So each far plane is held where the wide screen's edge meets the
  arcade's own last column of that plane (its camera at the arena's limits, moved in by 67 columns): at an arena's
  end the wide picture shows of every plane what the arcade shows at its limit, no more. A plane is held this way once
  any of its lines ends within the arcade's last 67 columns at a limit, or short of them; a plane whose art goes on is
  not held. The world's plane (fighters, floor) follows the limited wide camera as in the other games.
- **No edge policy.** Past a layer's end the wide picture shows what the arcade shows there: a farther plane or the
  page's background. Nothing is masked and no line is continued (MK1's and MK2's *Edge policy* is not used).
- **A far backdrop's soft sides.** Several arenas have a far backdrop that hardly scrolls and is a picture the size
  of the arcade's screen (a skyline behind a street, a bank's windows or a rooftop; a sunset behind a bridge). It has
  no art for the sides at any camera position. Where such a plane's lines end on straight edges in a margin (32
  lines or more, each covering three quarters of the arcade's columns or more, 9 in 10 of them at a column where 32
  lines or more end, give or take one: a backdrop made of two pictures, one above the other, has two such edges), the
  margin past the edge is shown soft, in the plane's own place in the draw order (anything nearer is drawn
  over it): each line takes the average colour of its own last 4 columns, blurred over 10 lines up and down and
  dimmed from all of it beside the picture to 45% a margin's width further out. Nothing of the picture is copied, so
  no object can appear twice.
- **The floor.** The floor is drawn line by line with a perspective of its own: it keeps its own place, tied to the
  scroll of the plane behind the fighters, each line a little faster than the one above it. With an offset window the
  shadow pass moves that place by the plane's share of the offset, so the floor stays under the fighters whether or
  not the plane itself is held at its end.
- **Draws outside the walk.** A draw the game makes outside its display walk (the fighters' floor shadows, by direct
  DMA) lands at the wide window's column, so it stays under its fighter near an arena's end; a fill that reaches the
  video RAM line's end column (a wipe, a clear) goes on over the layer's columns past it. The floor stains of the
  Remastered edition are drawn over the wide window too, in the walk's order (under the world's objects). When the
  game clears a page after a build's walk and before it draws that build (the continue screen's doors opening), the
  shadow's draws went with the clear, and the page takes the game's own draws as they come.
- **The centre.** MK3's centre comes from the shadow's draws, checked against the real frame as in MK2; the pixels of
  a block the arcade has not drawn yet are counted apart as in MK1 (`filled`).
- **The scenery updater runs in a fight's build only, and only as the game's own does.** The game's updater also
  builds the blocks of screens that are no arena, and it builds them when it is called, not when the game shows them:
  the continue screen's doors exist in the game's lists about 15 frames before the game draws them. So the shadow
  pass runs the updater only in a build the wide window takes for a fight; on any other screen it runs the display
  walk alone and shows what the game itself has built. In a finishing move the game stops calling its updater (it is
  called every 8 frames in a fight; 32 without a call count as stopped), and some finishing moves then move the
  scenery by hand: the arena scrolls away under a falling fighter. When the shadow's updater would free an object the
  game still keeps while the game's own is stopped, that frame's pass goes without the updater too. The margins then
  hold the blocks the game built for its own window (about 40 columns of each margin) and the planes behind them.
- **Finishing moves.** From a finishing move's start to the round's end the game has a mode of its own. The wide
  picture goes on like the fight there, as at a round's end and a match's end: same window, same held planes. A
  finishing move with a scene of its own, no wider than the arcade's screen (a pit under the arena), leaves the
  margins without art: a fight's build with 32 lines or more of such margins (24 pixels or more on each) gets soft
  sides instead of black ones.
- **A fight that pauses.** Some finishing moves wipe the screen (a white flash) while the game builds no frame for a
  few frames. The fight is still the fight when its builds come back, with its planes where they were held, and a
  page the game filled whole is shown whole (see *Screens* below).
- **The match's end.** When a match ends the game freezes the fight's last picture and closes the continue screen's
  doors over it. From the frame the game stops building the arena the wide picture is the arcade's own, centred, with
  soft sides: a fight that ended within 67 columns of an arena's end moves back to the centre at that instant.
- **Screens.** A screen is told from an arena by the game's own camera limits (the two pairs it gives every screen
  without an arena): under them nothing is taken for a fight, whatever the mode word says (the tower between two
  fights keeps the round's-end mode) and wherever the camera stands. A far plane's picture no wider than the arcade's
  screen is no arena either (the versus screen: its portraits reach past the screen, its background does not), and an
  arena's scenery is measured over all its planes together, since none of them need cover the screen by itself. Such
  screens get soft sides. A page with soft sides whose margins already hold the colour each arcade line ends in (a
  page filled whole: a flash, a wipe, black) is shown as it is. The power-on advisory screen is drawn with no display
  walk on a page the game filled black: its blue bars end at the arcade's screen, with black beside them.

## Debug mode

Set `TRIUNIT_WIDE_DEBUG=1` before the game starts. Each margin pixel then records which draw wrote it, the kind of
write and the columns the draw meant to cover (from the game's own x, before the board's 10-bit wrap). A margin pixel
no draw wrote shows magenta instead of black. Each shown frame gets a margin audit (`mk5wide::Audit`): sentinel,
masked, outside (a pixel its draw never meant to cover), generated (written by no draw of the game) and stale pixels.
The layer also keeps each build's scenery draws (`notes_done`) and measured layers (`bands_done`) for the parallax check
and the coverage table.

## Acceptance tests

```powershell
python tools/widescreen/acceptance.py
```

The command needs the ROMs and the setup's generated code for the three games. `--game mk1`, `--game mk2` or
`--game mk3` runs one game. `--all` adds the long replays (MK1 `campaign-prefix`, MK2's full campaign, which visits
every arena, MK3's jumps in every arena the coin-in time reaches, a campaign of matches through the others with the
versus and continue screens between them, a finishing move with a screen flash and one in the tower). Each
legal replay runs without and with widescreen, and the command checks:

1. **Same game in every frame.** MK1 compares CPU, cycles, RAM, VRAM and palette per frame. MK2 compares VRAM and
   palette per frame, and RAM, CPU and cycles at every live chunk (about 17 frames). MK3 compares the frame, the
   palette, the cycles, the host PCM and a hash of RAM, VRAM, CMOS and the CPU registers per frame. Shown frames, PCM
   and CMOS must also match.
2. **Centre.** The 400 centre columns equal the arcade frame bit for bit in every frame whose wide camera is the
   arcade's. MK1 compares the presented frames (its wide layer takes the floor stains at presentation, so the raw layer
   alone differs where blood lies). Frames with an offset window or a held far plane are counted apart. So are, in
   MK1, the pixels of a block the arcade has not drawn yet (`filled`: each must come from a draw of that frame's
   shadow pass meant to cover its column) and, on a screen with soft sides, a line's black end within 12 columns of the
   arcade's edge, where the soft side starts (`joined`).
3. **Sentinel.** No fight frame (the game's mode word: the fight, a round's start or end, a match's end, in MK3 a
   finishing move) shows a margin pixel no draw wrote. Masked pixels are counted, and MK2's replays must reach a scenery layer's edge in a fight
   (the mask at work).
4. **No duplication.** Every margin pixel belongs to a draw meant to cover that column, and no fight frame shows a
   margin pixel older than the frame on a line the frame redrew.
5. **Parallax.** For each scenery layer and each frame where the layer's list camera moved and its centre draws moved by
   minus that amount, the most frequent move of its draws in the margins is the same. A comparison needs at least 3
   draws sharing the move on each side, so objects moving on their own (fighters, flying objects) are not taken for
   scenery. In MK3 a margin where a draw moved like the centre's is no difference: blood drops fly in the world's own
   list, and three of a spray can pair by chance with one move from a build to the next.
6. **Still camera.** In MK1 and MK3, with the camera still, every scenery list's camera is still: a plane's place
   depends on the camera alone.
7. **No black over the picture.** In MK1 and MK3 no fight frame with a lit picture masks more than a speck (16 pixels) of its
   margins (a frame with soft sides shows none of them and is left out). A black bar or block over a sky fails; what is left is a pixel of a farther layer at the screen's last
   column, masked for a few frames of an animation.
8. **Finishing moves keep the arena.** In MK3 the suite's finishing moves, whose scenes are the arena's, show no
   frame with soft sides.

Continued lines and soft sides (a menu's, a far backdrop's in MK3) are derived from the picture's own edge colours, not
drawn by the game; criterion 4 covers the game's draws.

The command also prints the edge coverage table: for each arena (its camera limits) and scenery layer, the fewest
margin columns its art covered and the camera range seen. Replays: MK1 `versus-walls` (both walls of the Courtyard),
`versus-corner-win` (a fight won near the Courtyard's right end) and `versus-jumps` (diagonal jumps from wall to
wall, both fighters); MK2 `finisher_replays/case00`, `case05` and
`natural-f01-stage-a`; MK3 `fight-route` (the attract, the select screen, the tower and a first fight), `jumps-N`
(a versus match on arena N of the game's order: both fighters walk to a wall, jump diagonally from wall to wall and
back, then P1 punches until the round ends; inputs only, `tools/widescreen/mk3_tapes.py`) and `finisher-NAME` (a
legal finishing-move route of `tools/mk3/r1_finishes.json` from reset).
