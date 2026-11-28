#pragma once
#include "../include/mk5_wide.hpp"
#include <cstdint>
// Widescreen shadow pass (mk2_task_batch.cpp, BootOps::shadow): address-only facts of MK2 L3.1, recovered locally from
// the user's ROM. Bit addresses: program FF800000.., RAM 01000000..
namespace mk2::wide {
// Display table walk of the frame builder: after the queue reset, to the direct-DMA stage.
constexpr uint32_t kWalkStart=0xff804e30,kWalkEnd=0xff805050,kQueueReset=0xff808a40;
// The display interrupt's entry: each frame's build (when the game builds one) follows it.
constexpr uint32_t kDisplayIrq=0xff816370;
// The table's marker entry (B8 = A1): when the drain reaches it, the fighters' floor shadows are drawn (FF801000 by direct
// DMA, FF801590/FF801600), before the entries staged after it (the world objects).
constexpr uint32_t kMarker=0xff804f60,kDrain=0xff804630;
// The list drawer's interrupt kick (DINT, INT1 enable, DMA busy test, TRAP 1, EINT), skipped by the shadow.
constexpr uint32_t kKick=0xff804440,kKickEnd=0xff804510;
// Staging MMTMs: the list drawer (A12 A11 A10 A9 A8 A6 as r0/r1..r10/r11) and the floor producer (A9 A8 A7 A6 A3 A2).
constexpr uint32_t kDrawerStage=0xff804410,kFloorStage=0xff800490;
// The floor producer (FF800000) keeps its own place: each frame its x long (16.16, minus the scroll) loses the velocity long
// that kFloorVelocity points at (a scroll slot's), and the arena's perspective routine (the long at kFloorRoutine, one of
// kFloorPerspective) takes that velocity shifted right by its own count (its SRA) from the long that each floor line
// adds to the x of the line before it.
constexpr uint32_t kFloorX=0x0105ec00,kFloorSlope=0x0105ebe0,kFloorVelocity=0x0105ecb0,kFloorRoutine=0x0105ec70;
struct FloorRoutine{uint32_t pc;unsigned shift;};
constexpr FloorRoutine kFloorPerspective[]={{0xff9fe770,7},{0xff9ffa50,7},{0xffa026f0,7},{0xffa047a0,7},{0xffa04e50,7},{0xffa07850,8},{0xffa08f10,7}};
// Scenery streamer: entry, the return of its only call, and the object pool resets that end its planes.
constexpr uint32_t kStreamer=0xff9f8030,kStreamerReturn=0xff9f7fe0,kPoolReset[2]={0xff808990,0xff83f160};
struct Range {uint32_t from,to;};
constexpr Range kStreamerCode[]={{0xff9f8030,0xff9f8260},{0xff80af00,0xff80c600},{0xff8090b0,0xff809280},
                                 {0xff8093a0,0xff809580},{0xff809ea0,0xff809fa0},{0xff80c6b0,0xff80c7e0},{0xff80d180,0xff80d790}};
inline bool StreamerCode(uint32_t pc){for(const auto& r:kStreamerCode)if(pc>=r.from&&pc<=r.to)return true;return false;}
// The palette slots' table (the search at FF80CBC0 compares kPaletteSlots entries from kPaletteTable, kPaletteEntry apart):
// each the long of the palette's ROM pointer in the slot. A palette the shadow's streamer allocates is only booked in that
// table, which the journal undoes: its colours are not in the palette RAM (BootVideo::wide_palettes).
constexpr uint32_t kPaletteTable=0x0105bb30,kPaletteEntry=0x40;constexpr unsigned kPaletteSlots=0x3c;
// The object pool's free list: its head long; each free object's first long links the next (0 ends it; FF8090D0).
constexpr uint32_t kFreeList=0x01060130;constexpr size_t kPoolObjects=4096;
// An object's x velocity and x position longs (16.16), their offsets from the object (bits): each frame's move adds the
// velocity to the position.
constexpr uint32_t kObjectXVelocity=0x40,kObjectX=0x80;
// RAM: the clip longs (top-left, bottom-right as y<<16|x) and the page origin long (its high word: the page's first row).
constexpr uint32_t kClipTopLeft=0x01060150,kClipBottomRight=0x01060170,kPageOrigin=0x0105d050;
constexpr uint32_t kArcadeBottomRight=0x00fe0190;   // x 400
constexpr int kMargin=mk5wide::kMargin;   // VRAM columns beyond each side of the 400 visible ones
// Each list's camera: at kCameraLoad A13 holds the address of the variable the walk reads as the list's camera. Lists whose
// camera is a world or parallax scroll (kScrollFirst..kScrollLast) are scenery; any other camera is screen-fixed (HUD).
constexpr uint32_t kCameraLoad=0xff804fc0,kScrollFirst=0x0105d260,kScrollLast=0x0105d370;
// The walk's next table entry (MOVE *A3+,A0): the previous list or callback has ended.
constexpr uint32_t kTableNext=0xff804f10;
// The camera x word (WORLDTLX), the arena's camera limits (words) and the mode word (2: the fight): read by the
// widescreen acceptance harness only; the layer never reads the camera.
constexpr uint32_t kCameraX=0x0105d350,kCameraMin=0x0105ecf0,kCameraMax=0x0105ed00,kMode=0x010601d0;
// The scroll longs (16.16, a camera word is the high word): the far planes' from kScrollFirst every 20h, the world's
// (kCameraX's long, slot 7) and the front plane's (slot 8); each frame each adds its velocity long, kScrollVelocityGap
// lower. In the fight's modes (2, 6, 7) the wide window follows the camera within the arena's limits
// (mk5wide::WindowShift); afterwards its offset stays while the camera does (mk5wide::Layer::Offset). The attract (1)
// has one mode for its demo fights and its other screens: there the window follows the camera once the layer shows an
// arena (mk5wide::Layer::arena) and while the limits are an arena's: not the ones the game gives a screen without an
// arena (kScreenMin, kScreenMax) nor wider than an arena's (mk5wide::kWorldMax). A build that measures no scenery (the
// screen following an uppercut upwards) does not end it while the camera stands (when the demo is left the camera goes
// back to 0 with the demo's limits still there). The round's end and the next one's start (5) and the match's
// end (3) go on like the fight they follow, under the same limits (the camera goes back to the middle between rounds:
// an offset kept only while it stays would leave the arena there).
constexpr unsigned kScrollSlots=9,kWorldSlot=7;constexpr uint32_t kScrollVelocityGap=0x120;
static_assert(kScrollFirst+kWorldSlot*0x20+16==kCameraX && kScrollFirst+(kScrollSlots-1)*0x20+16==kScrollLast,"MK2 scroll layout");
constexpr unsigned kShiftModes=(1u<<2)|(1u<<6)|(1u<<7),kAttractMode=1;constexpr int kScreenMin=-1280,kScreenMax=1280;
inline bool Shifting(unsigned mode){return mode<16&&(kShiftModes>>mode&1);}
constexpr unsigned kAfterModes=(1u<<3)|(1u<<5);
inline bool Demo(unsigned mode,int low,int high,bool arena,bool fight,bool still){   // still: the camera stands where the fight left it
    return (mode==kAttractMode?arena||(fight&&still):mode<16&&(kAfterModes>>mode&1)&&(arena||fight))&&
           low<high&&high-low<=mk5wide::kWorldMax&&!(low==kScreenMin&&high==kScreenMax);
}
// A list's far plane slot from its camera word's address (A13 at kCameraLoad); -1: the world's, the front's or none.
inline int FarSlot(uint32_t camera){
    const uint32_t at=camera-16-kScrollFirst;
    return camera>=kScrollFirst+16&&!(at%0x20)&&at/0x20<kWorldSlot?int(at/0x20):-1;
}
// The shadow's clip (y<<16|x): screen x -67 .. 467 plus kReach on each side, so that the streamer builds, and the walk
// stages, the objects partly inside the layer. Its 10-bit DMA x stays within -256..767.
constexpr int kReach=mk5wide::kReach;
constexpr uint32_t WideTopLeft(){return uint32_t(uint16_t(int16_t(-mk5wide::kWideMargin-kReach)));}
constexpr uint32_t WideBottomRight(){return (kArcadeBottomRight&0xffff0000u)|uint16_t(int16_t(400+mk5wide::kWideMargin+kReach));}
}
