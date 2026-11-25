#pragma once
#include "../include/mk5_wide.hpp"
#include <cstdint>
// Widescreen shadow pass (mk3_wide.hpp): address-only facts of MK3 rev 2.1 (Wolf Unit), recovered locally from the
// user's ROM. Bit addresses: program FF800000.., RAM 01000000.. The VRAM line is the T-Unit games': 512 columns, the 400
// visible ones from column 56, so the wide layer is the shared one (mk5_wide.hpp).
namespace mk3::wide {
// Display table walk of the frame builder: after the queue reset, to the direct-DMA stage. The builder runs inside the
// display interrupt (kDisplayIrq: a new frame), after the page clear (kPageFill, by shift-register transfers).
constexpr uint32_t kWalkStart = 0xff804f10, kWalkEnd = 0xff8050c0, kQueueReset = 0xff806840, kDisplayIrq = 0xff81a7c0;
constexpr uint32_t kPageFill = 0xff81b580;
// The table's marker entry (B8 = A1): when the pump reaches it, the fighters' floor shadows are drawn by direct DMA,
// before the entries staged after it (the world objects).
constexpr uint32_t kMarker = 0xff920c60, kPump = 0xff804640;
// The pump's marker branch: the return of its floor-shadow call, and its RETI (taken with or without shadows).
constexpr uint32_t kMarkerDone = 0xff804890, kMarkerEnd = 0xff8049a0;
// The list drawer's interrupt kick (DINT, INT1 enable, DMA busy test, TRAP 1, EINT), skipped by the shadow.
constexpr uint32_t kKick = 0xff804440, kKickEnd = 0xff804510;
// Staging MMTMs through A1: the list drawer (A12 A11 A10 A9 A8 A6 as r0/r1..r10/r11; A0 the object, A7 bit 0 an object
// that keeps its screen place) and the floor producer (A9 A8 A7 A6 A3 A2).
constexpr uint32_t kDrawerStage = 0xff804410, kFloorStage = 0xff800470;
constexpr uint32_t kFixedObject = 1;
// The floor producer keeps its own place: each frame its x long (16.16, minus the scroll) loses the velocity long that
// kFloorVelocity points at (a scroll slot's: the plane behind the fighters), and the perspective routine
// kFloorPerspective (the long at kFloorRoutine) takes a 128th of that velocity from the long that each floor line adds
// to the x of the line before it.
constexpr uint32_t kFloorX = 0x0105f390, kFloorSlope = 0x0105f370, kFloorVelocity = 0x0105f440, kFloorRoutine = 0x0105f400;
constexpr uint32_t kFloorPerspective = 0xff9204c0;
constexpr unsigned kFloorSlopeShift = 7;
// The walk's next table entry (MOVE *A3+,A0: the previous list or callback has ended) and each list's camera: at
// kCameraLoad A13 holds the address of the word the walk reads as the list's camera; at kListWalk A0 is the list.
constexpr uint32_t kTableNext = 0xff804fc0, kCameraLoad = 0xff805000, kListWalk = 0xff805060, kWorldList = 0x0105dc10;
// Scenery updater: a process calls it every 8 ticks; it rebuilds every plane's blocks from its window (the clip longs
// and 28h columns, 8 lines around them). kStreamerReturn: the return of that call; kStreamerEnd: its only RETS.
// kPoolReset: the object pool's free list is made again (the planes are gone).
constexpr uint32_t kStreamer = 0xff908670, kStreamerReturn = 0xff908620, kStreamerEnd = 0xff908800, kPoolReset = 0xff8066d0;
// The updater's pass over a plane's objects frees the ones outside its window from kPlaneFree on (the object in A8).
// The game stops calling its updater in a finishing move, and some of them then move the scenery by hand (the arena
// scrolls away under a falling fighter, objects wait below the screen): there the updater's window means nothing.
// kUpdaterStopped: display interrupts without a call after which the game's updater counts as stopped (it is called
// every 8 ticks).
constexpr uint32_t kPlaneFree = 0xff925100;
constexpr uint32_t kUpdaterStopped = 32;
// The arena changes inside a fight (its arena store). Most arenas share one pair of camera limits, so the limits do not
// tell one arena from the next: the planes' measured ratios and ends are forgotten here and at each pool reset.
constexpr uint32_t kArenaSwitch = 0xff90ebe0;
// The palette slots' table (the search at FF925830 compares kPaletteSlots entries from kPaletteTable, kPaletteEntry
// apart): each the long of the palette's ROM pointer in the slot.
constexpr uint32_t kPaletteTable = 0x0105bb30, kPaletteEntry = 0x40;
constexpr unsigned kPaletteSlots = 0x5a;
// The object pool's free list: its head long; each free object's first long links the next.
constexpr uint32_t kFreeList = 0x010608c0;
constexpr size_t kPoolObjects = 4096;
// RAM: the clip longs (top-left, bottom-right as y<<16|x) and the page origin long (its high word: the page's first row).
constexpr uint32_t kClipTopLeft = 0x010608e0, kClipBottomRight = 0x01060900, kPageOrigin = 0x0105d7c0;
constexpr uint32_t kArcadeBottomRight = 0x00fe0190;   // x 400
// The scroll longs (16.16, a camera word is the high word): the far planes' from kScrollFirst every 20h and the
// world's (slot 8, kCameraX its word); each frame each adds its velocity long, kScrollVelocityGap lower. A list whose
// camera is one of these words is scenery or the world; any other camera is screen-fixed (HUD).
constexpr uint32_t kScrollFirst = 0x0105d9d0, kScrollVelocityGap = 0x120;
constexpr unsigned kScrollSlots = 9, kWorldSlot = 8;
constexpr uint32_t kCameraX = 0x0105dae0, kScrollLast = kCameraX;
static_assert(kScrollFirst + kWorldSlot * 0x20 + 16 == kCameraX, "MK3 scroll layout");
// The arena's camera limits (words) and the mode word.
constexpr uint32_t kCameraMin = 0x0105f480, kCameraMax = 0x0105f490, kMode = 0x01060960;
// In the fight's modes (2, and 6 and 7 at a round's start) the wide window follows the camera within the arena's limits
// (mk5wide::WindowShift); afterwards its offset stays while the camera does (mk5wide::Layer::Offset). The attract has
// one mode for its demo fights and its other screens: there the window follows the camera once the layer shows an
// arena (mk5wide::Layer::arena) and while the limits are an arena's: not the ones the game gives a screen without an
// arena (ScreenLimits) nor wider than an arena's (mk5wide::kWorldMax). The round's end (5), the match's end (3) and a
// finishing move (kFinisherMode, from its start to the round's end) go on like the fight they follow, under the same
// limits. A screen's limits end it: the tower after a fight won and the continue screen's doors keep those modes.
constexpr unsigned kFinisherMode = 17;
constexpr unsigned kShiftModes = (1u << 2) | (1u << 6) | (1u << 7), kAfterModes = (1u << 3) | (1u << 5) | (1u << kFinisherMode), kAttractMode = 1;
inline bool Shifting(unsigned mode) { return mode < 32 && (kShiftModes >> mode & 1); }
inline bool FightMode(unsigned mode) { return mode < 32 && ((kShiftModes | kAfterModes) >> mode & 1); }
inline bool ScreenLimits(int low, int high) { return (low == -1280 && high == 1280) || (low == -80 && high == 4095); }
inline bool Demo(unsigned mode, int low, int high, bool arena, bool fight, bool still) {   // still: the camera stands where the fight left it
    return (mode == kAttractMode ? arena || (fight && still) : mode < 32 && (kAfterModes >> mode & 1) && (arena || fight)) &&
           low < high && high - low <= mk5wide::kWorldMax && !ScreenLimits(low, high);
}
inline bool ScrollCamera(uint32_t camera) { return camera >= kScrollFirst + 16 && camera <= kScrollLast && !((camera - 16 - kScrollFirst) % 0x20); }
// A list's far plane slot from its camera word's address; -1: the world's or none.
inline int FarSlot(uint32_t camera) { return ScrollCamera(camera) && (camera - 16 - kScrollFirst) / 0x20 < kWorldSlot ? int((camera - 16 - kScrollFirst) / 0x20) : -1; }
// The shadow's clip (y<<16|x): screen x -67 .. 467 plus kReach on each side. Its 10-bit DMA x stays within -256..767.
constexpr uint32_t WideTopLeft() { return uint32_t(uint16_t(int16_t(-mk5wide::kWideMargin - mk5wide::kReach))); }
constexpr uint32_t WideBottomRight() { return (kArcadeBottomRight & 0xffff0000u) | uint16_t(int16_t(400 + mk5wide::kWideMargin + mk5wide::kReach)); }
}
