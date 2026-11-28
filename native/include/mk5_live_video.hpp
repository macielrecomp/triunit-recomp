#pragma once
#include "mk5_irq.hpp"
#include "mk5_fighter_video.hpp"
#include "mk5_blood_shade.hpp"
#include "mk5_wide.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <tuple>

struct VerifiedMk5GlyphBank {std::vector<uint8_t> bytes,graphics;};
VerifiedMk5GlyphBank LoadMk5GlyphBank(const std::filesystem::path&);
struct Mk5DmaCommand {uint32_t pc;uint64_t start_ns,due_ns;std::array<uint16_t,18> registers;};
// Bounded MK1 non-stream DMA, fixed-point scale, up to 1048576 output samples.
// Draws at command issue, busy/IRQ completes at 41ns/pixel, as pinned driver.
class Mk5LiveVideo {
    Mk5MemoryBuffers& m;Mk5IrqController& irq;BootDiState& di;
    const ResetTrace& cpu;
    std::vector<uint8_t> gfx;std::array<uint16_t,18> regs{};
    uint64_t due=0;bool pending=false;
    bool data_bank=true;
    struct BloodSprite {
        uint32_t frame=0;
        int width=0,height=0;
        std::vector<uint8_t> pixels;
        std::array<uint32_t,64> colors{};
    } blood_sprite;
    // A stain per kept pose, with the floor each splash pixel first covered (mk5_blood_shade.hpp): its pen | kTaken
    // (0 = not drawn yet) and that pen's brightness then.
    struct BloodStain {
        std::vector<uint16_t> pen;std::vector<uint8_t> floor;
        template<class A> friend void Fields(A& a,BloodStain& s){a(s.pen,s.floor);}
    };
    std::map<std::tuple<int16_t,int16_t,bool>,BloodStain> blood_stains;
    std::vector<uint8_t> blood_layer;
    std::vector<uint16_t> blood_pen;    // per video RAM word under a stain: the floor of its splash pixel
    std::vector<uint8_t> blood_floor;
    std::vector<uint16_t> backdrop;
    mk5presentation::FighterSurface fighters;
    mk5presentation::FighterSlots fighter_objects;
    uint32_t held_prop=0;
    // Widescreen (mk5_widescreen.hpp, Mk5TaskBodyRunner shadow mode): the 16:9 frame (mk5_wide.hpp). Each frame starts
    // from its page as VRAM holds it when the builder starts (after the page clear). The shadow walk stages every list's
    // draws with the wide clip; the real walk then merges them by object: before each real draw the staged draws up to the
    // same object are drawn outside their arcade columns, and the real draw itself is mirrored inside them (so the arcade's
    // columns are its own draws). With a window offset (mk5wide::WindowShift) the world's staged draws are drawn whole
    // instead, at the shadow's camera, and the world's real draws are not mirrored. Every other draw of the frame (DMAs outside the walk, the page clear, CPU writes) is
    // mirrored as it happens. The blood is the floor splash's shade, as blood_layer.
    mk5wide::Layer<uint8_t> wide;
    std::array<uint16_t,18> shadow_regs{};
    // The real walk runs (wide_walk); the shadow staged its draws (wide_walk_drawn), else the real walk is mirrored whole.
    bool wide_walk_drawn=false,wide_walk=false;
    // A staged draw: its registers (VRAM x), its object, whether it is screen-fixed (the HUD) and its scenery layer (the
    // walk's list it belongs to, mk5wide::Band; -1: none).
    struct WideEntry{std::array<uint16_t,18> regs;uint32_t object;bool fixed;int layer;
        template<class A> friend void Fields(A& a,WideEntry& e){a(e.regs,e.object,e.fixed,e.layer);}};
    std::vector<WideEntry> wide_staged;size_t wide_cursor=0;
    // The shadow's draw being staged: a screen-fixed object (the drawer's test); the list's scenery draws wait for its end
    // (wide_plane) so the layer's art is measured whole (mk5wide::MeasureBand).
    bool wide_hud=false;int wide_layer=0;int16_t wide_camera=0;int wide_slot=-1;   // the waiting list's camera x and far plane slot (-1: none)
    std::vector<WideEntry> wide_plane;
    // The parts of the far-plane groups the game wraps around a ring (mk5widescreen::kWrapTest): each one's ring length
    // and the frame its group was last tested (every 8 frames; a part not tested for kWrapKeep frames is forgotten).
    struct WideWrapPart{uint32_t object;int length;uint32_t seen;
        template<class A> friend void Fields(A& a,WideWrapPart& w){a(w.object,w.length,w.seen);}};
    static constexpr uint32_t kWrapKeep=24;
    std::vector<WideWrapPart> wide_wrap;uint32_t wide_frames=0;
    void WideRing(const WideEntry& e,std::vector<WideEntry>& to);
    // The palette slots' ROM pointers when the shadow pass starts (mk5widescreen::kPaletteTable); not state (the pass never
    // spans a ContinueMain call). A shadow draw in a slot the pass booked for another palette (the arcade has not built the
    // object, so its colours are not loaded) takes its colours from the ROM (mk5wide::Layer::Book).
    std::array<uint32_t,64> wide_palettes{};const std::vector<uint8_t>* wide_rom=nullptr;   // the program image (Layer::Book)
    void WidePalettes(std::array<uint32_t,64>& to)const;
public:
    unsigned WideDim()const;   // the game's fade right now, of 256 (mk5wide::Layer::Colour)
private:
    void WideStage(const WideEntry& e);
    const WideEntry* WideMerge(uint32_t object);
    void WideFlush(size_t end);
    void WideOutside(const WideEntry& e,bool unmade=false);
    // Save states: gfx and blood_sprite come from the ROMs and stay with the live object.
    template<class A> friend void Fields(A& a,Mk5LiveVideo& v){
        a(v.regs,v.due,v.pending,v.data_bank,v.blood_stains,v.blood_layer,v.blood_pen,v.blood_floor,v.backdrop,v.fighters,v.fighter_objects,v.held_prop,
          v.commands,v.completions,v.pixels_written,v.shift_loads,v.shift_stores);
        v.WideFields(a);
    }
    // Widescreen state (W4), with a presence mark: a session without the layer drops a file's, one with it starts over from
    // VRAM when the file has none. The shadow pass never spans a ContinueMain call: its own per-pass values are not state.
    template<class A> void WideFields(A& a){
        const bool enabled=wide.Enabled();bool present=enabled;a(present);
        if(present){wide.LayerFields(a);wide.EvenFields(a);a(wide_walk_drawn,wide_walk,wide_streaming,wide_staged,wide_cursor,wide_wrap,wide_frames);}
        if constexpr(A::reading){
            if(enabled==present)return;
            const bool blood=!blood_layer.empty();wide.Reset();wide_walk_drawn=wide_walk=wide_streaming=false;
            wide_staged.clear();wide_cursor=0;wide_wrap.clear();wide_frames=0;
            if(enabled)wide.Enable(m.vram,blood);
        }
    }
    void DrawBlood(const ResetTrace&, bool arena_visible);
    bool WidePlane();   // the waiting list's draws: measured (a band's lines) and staged
    bool IsBackdropMode()const;
    bool IsBackdropDma(uint32_t caller)const;
    uint32_t ObjectDrawReturn(uint32_t pc)const;
    uint64_t FighterOwner(uint32_t caller)const;
public:
    static constexpr uint16_t backdrop_owner=0x8000;
    std::vector<Mk5DmaCommand> commands;
    std::vector<uint64_t> completions;
    uint64_t pixels_written=0,shift_loads=0,shift_stores=0;
    Mk5LiveVideo(const VerifiedMk5GlyphBank&,ResetTrace&,Mk5MemoryBuffers&,Mk5IrqController&,BootDiState&,const VerifiedMk5Program* blood_program=nullptr);
    void ObserveBlood(uint32_t pc,const ResetTrace&);
    void BindFighter(uint32_t object);
    void BindHeldProp(uint32_t object);
    void FighterRamWrite(uint32_t pc,uint32_t address,uint32_t value);
    bool BackdropEnabled()const{return !backdrop.empty();}
    const mk5presentation::FighterSurface& FighterVideo()const{return fighters;}
    uint16_t BackdropPixel(size_t index)const{return backdrop[index];}
    uint32_t BloodPixel(size_t index)const {
        return blood_layer.empty()?0:blood_sprite.colors[blood_layer[index]];
    }
    // The floor of the stain word at `index` (meaningful where BloodPixel is not 0): pen | kTaken and its brightness then.
    uint16_t BloodPen(size_t index)const{return blood_pen.empty()?0:blood_pen[index];}
    uint8_t BloodFloor(size_t index)const{return blood_floor.empty()?0:blood_floor[index];}
    void Update();
    uint16_t Read(unsigned offset)const;
    void Write(uint32_t pc,unsigned offset,uint16_t value);
    uint32_t CpuRead(uint32_t bit,unsigned width)const;
    void CpuWrite(uint32_t bit,unsigned width,uint32_t value,bool draw=true);   // draw: it belongs to the picture (the wide layer takes it)
    void Control(uint16_t value){data_bank=(value&32)!=0;}
    uint32_t ShiftLoad(uint32_t bit);
    uint32_t FillXY(ResetTrace&);
    // Widescreen. EnableWide before the run; the runner drives the frame (WideFrame, WideEndUnbuilt) and the shadow pass
    // (WideBegin, WideList, ShadowWrite/ShadowRead for its DMA registers, WideEnd).
    void EnableWide(const std::vector<uint8_t>& program){wide_rom=&program;wide.Enable(m.vram,!blood_layer.empty());}
    bool WideEnabled()const{return wide.Enabled();}
    const mk5wide::Layer<uint8_t>& WideLayer()const{return wide;}
    uint32_t WideBloodPixel(size_t index)const{const auto shade=wide.blood.empty()?0:wide.blood[index];return shade?blood_sprite.colors[shade]:0;}
    void WideFrame(unsigned page_row,bool still){wide.StartFrame(page_row,m.vram,blood_layer,still);wide_walk_drawn=false;wide_staged.clear();wide_cursor=0;
        ++wide_frames;wide_wrap.erase(std::remove_if(wide_wrap.begin(),wide_wrap.end(),[&](const WideWrapPart& w){return wide_frames-w.seen>kWrapKeep;}),wide_wrap.end());}   // still: mk5wide::Layer::StartFrame
    void WideWrap(uint32_t object,int length){
        for(auto& w:wide_wrap)if(w.object==object){w.length=length;w.seen=wide_frames;return;}
        wide_wrap.push_back({object,length,wide_frames});
    }
    void WideEndUnbuilt();
    // The real walk starts / ends: its draws merge the staged ones; at its end the rest of them are drawn.
    void WideRealWalk(bool inside){
        if(!inside && wide_walk && wide_walk_drawn){WideFlush(wide_staged.size());wide_staged.clear();wide_cursor=0;}
        wide_walk=inside;
    }
    // The shadow pass starts.
    void WideBegin(){
        shadow_regs=regs;wide_hud=false;wide_layer=0;wide_plane.clear();wide_staged.clear();wide_cursor=0;
        WidePalettes(wide_palettes);shadow_active=true;
    }
    // The frame's window offset (mk5wide::Layer::Offset) and a scroll slot's ratio, set when the shadow pass starts.
    int WideOffset(bool shifting,int camera,int low,int high,bool keep){return wide.Offset(shifting,camera,low,high,keep);}
    bool shadow_active=false;   // no VRAM, layer or timing effect may happen while it is set
    bool wide_streaming=false;  // the real streamer is running (the shadow does not run it then)
    // A list starts (or the walk ends) in the shadow walk: the waiting list's draws are measured and staged.
    bool WideList(){const bool placed=WidePlane();++wide_layer;return placed;}
    void WideFixedObject(bool fixed){wide_hud=fixed;}
    void WideListCamera(int16_t camera,int slot){wide_camera=camera;wide_slot=slot;}   // the list that starts: its camera x and far slot
    void WideWhole(){wide.Whole();}
    mk5wide::Layer<uint8_t>& WideLayerState(){return wide;}   // the shadow pass's scrolls (mk5wide::Layer::PlaneScroll)
    bool ShadowWrite(unsigned offset,uint16_t value);
    uint16_t ShadowRead(unsigned offset)const{const auto v=shadow_regs.at(offset==0?1:offset);return offset<=1?uint16_t(v&0x7fff):v;}
    void WideEnd(bool complete){wide_plane.clear();wide_walk_drawn=complete;shadow_active=false;
        if(!complete){wide_staged.clear();wide.bands.clear();wide.page_shift[wide.page]=wide.shift=0;wide.page_whole[wide.page]=wide.whole=false;}}   // the real walk, mirrored whole: centred
    uint64_t wide_faults=0,wide_streamer_skips=0;uint32_t wide_fault_pc=0;   // diagnostics, not state
    uint64_t wide_ring=0;   // ring copies staged (WideRing; diagnostics)
};
