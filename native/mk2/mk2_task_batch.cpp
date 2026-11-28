#include "mk2_task_batch.hpp"
#include "mk2_live_host.hpp"
#include "mk2_valid_cmos_draw.hpp"
#include <chrono>
#include "mk5_program.hpp"
#include "mk5_hitbox.hpp"
#include "mk2_widescreen.hpp"
#include "mk2_valid_cmos_continuation.hpp"
#include "mk2_oracle_digest.hpp"
#include <iostream>
#include <memory>

namespace mk2 {
static bool ExecuteStartWaitExtras(BootOps& o) {
    auto& s=o.s;
    switch (s.next_pc) {
#include "mk2_start_wait.inc"
    default: return false;
    }
    return true;
}

static bool ExecuteNextWaitExtras(BootOps& o) {
    auto& s=o.s;
    switch (s.next_pc) {
#include "mk2_next_wait.inc"
    default: return ExecuteStartWaitExtras(o);
    }
    return true;
}

struct PreStartWaitStage { uint64_t end, instructions; const char* cpu; }; // cpu: CPU34 SHA-256 (mk2_oracle_digest.hpp)
static constexpr std::array<PreStartWaitStage,kPreStartWindowCount> prestart_stages{{
#include "mk2_prestart_wait_bounds.inc"
}};
static bool ExecutePreStartWaitExtras(BootOps& o) {
    auto& s=o.s;
    switch(s.next_pc) {
#include "mk2_prestart_wait.inc"
    default:return ExecuteNextWaitExtras(o);
    }
    return true;
}

static bool ExecuteStartReleaseExtras(BootOps& o) {
    auto& s=o.s;
    switch(s.next_pc) {
#include "mk2_start_release.inc"
    default:return ExecutePreStartWaitExtras(o);
    }
    return true;
}

static constexpr std::array<PreStartWaitStage,kSelectionWindowCount> selection_stages{{
#include "mk2_selection_bounds.inc"
}};
static bool ExecuteSelectionExtras(BootOps& o) {
    auto& s=o.s;
    switch(s.next_pc) {
#include "mk2_selection.inc"
    default:return ExecuteStartReleaseExtras(o);
    }
    return true;
}

static constexpr std::array<PreStartWaitStage,kFirstFightWindowCount> first_fight_stages{{
#include "mk2_first_fight_bounds.inc"
}};
static constexpr std::array<PreStartWaitStage,1> first_fight_high_punch_stage{{
#include "mk2_first_fight_high_punch_bounds.inc"
}};
#include "mk2_first_fight_main.inc"   // pages: generated units (tools/setup/paths.generated_sources)
bool ExecuteGeneratedInstruction(BootOps& o) { return ExecuteFirstFightStatic(o); }
static bool ExecuteFirstFightExtras(BootOps& o) {
    return ExecuteFirstFightStatic(o) || ExecuteSelectionExtras(o);
}

ResetTrace RunTestSwitch(const Program& program,ValidCmosContinuation prefix,DcsBootMachine& machine,
                         std::function<uint32_t(uint64_t)> input,const std::function<bool()>& running) {
    if (!machine.TestSwitchAdmitted() || !TestSwitchCpuBoundary(prefix.ready,4) ||
        !TestSwitchDspBoundary(machine.dsp.Registers(),4) || machine.LastMainCycles()!=prefix.ready.cycles ||
        prefix.main_ram.size()!=262144 || Sha256(program.bytes)!=kProgramSha256 ||
        Sha256(program.graphics)!=kGraphicsSha256 || !input || !running)
        throw std::runtime_error("MK2 Test Switch requires the synchronized original service branch");
    BootContinuation service=std::move(prefix);
    service.observe_history=false;
    BootOps o(program,machine,service);
    auto& s=o.s;
    s.board.SetHostInputProvider([input=std::move(input)](uint64_t tick) {
        const auto value=input(tick);
        if((~value&~0x12u)!=0)throw std::runtime_error("MK2 Test Switch accepts P1 Down and High Punch");
        return value;
    },s.cycles*16);
    uint64_t renew_at=s.cycles;
    auto watchdog=std::chrono::steady_clock::now();
    while(running()) {
        if(s.cycles>=renew_at && o.CanSuspend()) {
            machine.RenewTestSwitch(s.board);
            renew_at=s.cycles+live_detail::kRequestCycles;
            watchdog=std::chrono::steady_clock::now();
        }
        if((s.instructions&1023)==0 && std::chrono::steady_clock::now()-watchdog>
           std::chrono::milliseconds(live_detail::kChunkWallTimeoutMs))
            throw std::runtime_error("MK2 Test Switch scheduler watchdog expired");
        try {
            if((s.status&0x00200000) && (s.board.io[17]&s.board.io[18]))o.PendingInterrupt();
            if(!running())break;
            if(!ExecuteFirstFightStatic(o))throw std::runtime_error("MK2 Test Switch PC outside authenticated static translation");
        } catch(const std::exception& error) {
            throw std::runtime_error(std::string(error.what())+"; TMS PC "+std::to_string(s.next_pc)+
                "; TMS cycle "+std::to_string(s.cycles)+"; DSP PC "+std::to_string(machine.dsp.pc)+
                "; DSP cycle "+std::to_string(machine.dsp.cycles));
        }
    }
    s.board.ReleaseHostInputProvider();
    return std::move(service.ready);
}

static TaskBatchInit ExecuteTaskBatch(const Program& program, const BootContinuation& prefix,
    DcsBootMachine& machine, uint64_t end_cycle, uint64_t instruction_count,
    unsigned dma_completions, uint64_t dma_start, bool coin_queue = false, bool coin_accounting = false,
    bool coin_pair = false, bool start_wait = false, bool next_wait = false, bool prestart_wait = false, bool start_input = false, bool start_release = false, bool selection = false, bool first_fight = false, bool first_fight_high_punch = false, bool host_controls = false, bool live_host = false,
    BootContinuation* consume = nullptr) {
    // consume: the caller's own prefix, used no more (the live chunks): its state moves into the result instead of a copy.
    TaskBatchInit result;
    if(consume && consume==&prefix){
        result.ready = std::move(consume->ready); result.main_ram = std::move(consume->main_ram); result.video = std::move(consume->video);
        result.observe_history = consume->observe_history;
        if(live_host)result.execution=std::move(consume->execution);
    } else {
        result.ready = prefix.ready; result.main_ram = prefix.main_ram; result.video = prefix.video;
        result.observe_history = prefix.observe_history;
        if(live_host)result.execution=prefix.execution;
    }
    BootOps o(program, machine, result);
    auto& s = o.s;
    if(host_controls && (!first_fight || !s.board.HostInputProviderActive() || first_fight_high_punch))
        throw std::runtime_error("MK2 host execution requires its separate first-fight provider");
    if(first_fight_high_punch && (!first_fight || s.cycles!=128083937 || s.next_pc!=0xff80dd30 ||
       s.board.input_events.size()!=20 || s.board.requested_p1_high_punch || s.board.input0!=0xffff))
        throw std::runtime_error("MK2 high-punch branch requires its actual released-input prefix");
    if (coin_accounting && (s.next_pc != 0xff8119b0 || s.cycles != 3315882 ||
        !s.board.requested_coin1 || s.board.input_events.size() != 6))
        throw std::runtime_error("MK2 coin accounting requires the original queued-coin prefix");
    if(live_host && (!host_controls || result.observe_history || !machine.ValidCmosLiveHostAdmitted()))
        throw std::runtime_error("MK2 live execution requires its separate bounded-observation domain");
    const auto watchdog_start=live_host ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto first_instruction = s.instructions;
    const uint32_t end_pc = coin_queue ? 0xff8119b0 : 0xff80dd30;
    // Widescreen W1 (BootOps::shadow), once per frame at the display table walk (FF804E30, after the queue reset): with the
    // clip longs widened in the journal to the 16:9 window (-67..467 plus kReach), the scenery streamer FF9F8030 (when the game
    // runs it; it rebuilds every plane from its window, so one call fills the wider window) and then the whole walk run on
    // the journaled RAM, the drawer's interrupt kick skipped (FF804440..FF804500); the entries staged at FF804410 and
    // FF800490 draw the margins. Then the journal is undone and the real walk runs. The shadow never reaches the loop's
    // guards; a PC outside its routines, a write outside RAM or an exception ends it as a fault.
    std::tuple<decltype(s.a),decltype(s.b),decltype(s.status)> shadow_saved{};
    bool shadow_skip=false,shadow_walk=false;uint64_t shadow_steps=0;uint32_t shadow_fault_pc=0;
    constexpr uint64_t kShadowSteps=4000000;
    const auto shadow_end=[&]{
        if(o.shadow_fault)o.out.video.wide.bands.clear();   // an incomplete pass measured no whole layer
        for(auto it=o.shadow_journal.rbegin();it!=o.shadow_journal.rend();++it)o.out.main_ram[it->first]=it->second;
        o.shadow_journal.clear();std::tie(s.a,s.b,s.status)=shadow_saved;s.next_pc=mk2::wide::kWalkStart;
        if(o.shadow_fault)o.out.video.margin_fault_pc=shadow_fault_pc;
        if(o.shadow_fault){auto& w=o.out.video.wide;w.page_shift[w.page]=w.shift=0;w.page_whole[w.page]=w.whole=false;}   // the real walk, mirrored: centred
        o.shadow=false;shadow_skip=true;o.out.video.margin_faults+=o.shadow_fault;
        o.out.video.wide_walk_drawn=!o.shadow_fault;   // else the real walk's DMAs are mirrored too
    };
    // The shadow's clip and camera, written through the journal: with a window offset (mk5wide::WindowShift) the world's
    // scroll moves by it and each far plane's by its ratio times it, held where its art covers the wide screen
    // (mk5wide::Layer::PlaneScroll), so the streamer and the walk build the frame of the wide camera.
    const auto shadow_view=[&]{
        namespace w=mk2::wide;auto& layer=o.out.video.wide;
        o.Write32(w::kClipTopLeft,w::WideTopLeft());o.Write32(w::kClipBottomRight,w::WideBottomRight());
        const auto scroll_at=[&](uint32_t bit){const size_t i=(bit-0x01000000)/16;return int32_t(o.out.main_ram[i]|uint32_t(o.out.main_ram[i+1])<<16);};
        const int camera=int16_t(o.out.main_ram[(w::kCameraX-0x01000000)/16]);
        const int64_t offset=mk5wide::ScrollOffset(scroll_at(w::kScrollFirst+w::kWorldSlot*0x20),layer.shift,layer.AtLimit(camera));
        for(unsigned slot=0;slot<w::kScrollSlots;++slot){
            const uint32_t scroll=w::kScrollFirst+slot*0x20;const auto arcade=scroll_at(scroll);
            const auto wide=slot==w::kWorldSlot?int32_t(arcade+offset):layer.PlaneScroll(slot,arcade,camera,offset);
            o.out.video.wide_delta[slot]=int16_t((wide>>16)-(arcade>>16));
            if(wide!=arcade){o.Write32(scroll,uint32_t(wide));layer.Whole();}
            // The floor follows this slot's velocity from its own longs: they move by the slot's share of the window's
            // offset, whether or not the slot's plane is held at its art's end, so the floor stays under the fighters.
            if(offset&&uint32_t(scroll_at(w::kFloorVelocity))==scroll-w::kScrollVelocityGap){
                const auto& r=layer.ratio[slot];const int32_t moved=int32_t(r.world?offset*r.plane/r.world:offset);
                o.Write32(w::kFloorX,uint32_t(scroll_at(w::kFloorX)-moved));
                for(const auto& routine:w::kFloorPerspective)if(routine.pc==uint32_t(scroll_at(w::kFloorRoutine)))
                    o.Write32(w::kFloorSlope,uint32_t(scroll_at(w::kFloorSlope)-(moved>>routine.shift)));
            }
        }
    };
    while (live_host ? s.cycles<end_cycle :
           s.next_pc != end_pc || (host_controls ? s.cycles<end_cycle : s.cycles!=end_cycle)) {
        if(o.shadow){
            if(s.next_pc==mk2::wide::kKick)s.next_pc=mk2::wide::kKickEnd;
            if(s.next_pc==mk2::wide::kMarker&&shadow_walk)o.out.video.wide_shadow_post=true;
            if(s.next_pc==mk2::wide::kCameraLoad&&shadow_walk){   // a list with a screen-fixed camera (HUD) keeps its place
                o.out.video.wide_hud=s.a[13]<mk2::wide::kScrollFirst||s.a[13]>mk2::wide::kScrollLast;
                if(!o.out.video.WideList(o.program.graphics))o.shadow_fault=true;
                const size_t camera=(s.a[13]-0x01000000u)/16;   // a scenery list's camera word (debug notes)
                o.out.video.wide_camera=!o.out.video.wide_hud&&camera<o.out.main_ram.size()?int16_t(o.out.main_ram[camera]):int16_t(0);
                o.out.video.wide_slot=o.out.video.wide_hud?-1:mk2::wide::FarSlot(s.a[13]);
            }
            // Each table entry (a list or a callback) ends the previous one: the waiting scenery layer is measured and placed.
            if(s.next_pc==mk2::wide::kTableNext&&shadow_walk){
                if(!o.out.video.WideList(o.program.graphics))o.shadow_fault=true;
                o.out.video.wide_slot=-1;   // a callback's layer has no far plane's scroll
            }
            const uint32_t pc=s.next_pc;
            if(!shadow_walk&&pc==mk2::wide::kStreamerReturn){   // the streamer returned: the walk from its start, with the real registers
                std::tie(s.a,s.b,s.status)=shadow_saved;s.next_pc=mk2::wide::kWalkStart;shadow_walk=true;continue;
            }
            // The walk also runs the tables' callbacks (floor producers in FFA0xxxx entering FF800160): any program PC.
            const bool inside=shadow_walk?pc>=0xff800000:mk2::wide::StreamerCode(pc);
            if((shadow_walk&&pc==mk2::wide::kWalkEnd)||!inside||++shadow_steps>kShadowSteps){
                if(!(shadow_walk&&pc==mk2::wide::kWalkEnd)){o.shadow_fault=true;shadow_fault_pc=pc|(shadow_walk?1:0);}
                shadow_end();continue;
            }
        }else if(s.next_pc==mk2::wide::kWalkStart && o.out.video.WideEnabled()){
            const auto ram_long=[&](uint32_t bit){const size_t i=(bit-0x01000000)/16;return o.out.main_ram[i]|uint32_t(o.out.main_ram[i+1])<<16;};
            if(shadow_skip)shadow_skip=false;
            else if(ram_long(mk2::wide::kClipTopLeft)==0&&ram_long(mk2::wide::kClipBottomRight)==mk2::wide::kArcadeBottomRight){
                o.out.video.wide_hud=false;
                auto& free=o.out.video.wide_free;free.clear();   // the pool's free objects before the shadow's streamer run
                uint32_t object=ram_long(mk2::wide::kFreeList),link=mk2::wide::kFreeList;
                for(;object>=0x01000000&&object<0x013fffe0&&free.size()<mk2::wide::kPoolObjects;link=object,object=ram_long(object))free.push_back(object);
                std::sort(free.begin(),free.end());
                for(unsigned slot=0;slot<mk2::wide::kPaletteSlots;++slot)
                    o.out.video.wide_palettes[slot]=ram_long(mk2::wide::kPaletteTable+slot*mk2::wide::kPaletteEntry);
                shadow_saved={s.a,s.b,s.status};shadow_steps=0;o.shadow=true;o.shadow_fault=false;
                {   // the frame's window offset; a moving world gives each plane's ratio (its velocity over the world's)
                    namespace w=mk2::wide;auto& layer=o.out.video.wide;
                    const auto word=[&](uint32_t bit){return int16_t(o.out.main_ram[(bit-0x01000000)/16]);};
                    const unsigned mode=uint16_t(word(w::kMode));
                    layer.Offset(w::Shifting(mode)||w::Demo(mode,word(w::kCameraMin),word(w::kCameraMax),layer.arena,layer.fight,word(w::kCameraX)==layer.held[0]),word(w::kCameraX),word(w::kCameraMin),word(w::kCameraMax),mode!=w::kAttractMode&&!layer.Paused());
                    for(unsigned slot=0;slot<w::kScrollSlots;++slot)
                        layer.Velocity(slot,int32_t(ram_long(w::kScrollFirst+slot*0x20-w::kScrollVelocityGap)),int32_t(ram_long(w::kScrollFirst+w::kWorldSlot*0x20-w::kScrollVelocityGap)));
                }
                shadow_view();
                // The free list does not end in 0 (its last link holds another value): the game never takes its last object, but
                // the shadow's wider window can. In the journal the last link is 0, so the allocator (FF8090D0) takes its own
                // empty-pool path (FF809250, no object) instead of reading that value as an object.
                if(object)o.Write32(link,0);
                // Not while the interrupted main process is inside the real streamer: its lists are half updated.
                shadow_walk=!o.out.video.margin_streamer||o.out.video.margin_streaming;
                if(!shadow_walk){o.Push(mk2::wide::kStreamerReturn);s.next_pc=mk2::wide::kStreamer;}
            }
        }
        if(!o.shadow){
            if(live_host && ((s.instructions-first_instruction)&1023)==0 &&
               std::chrono::steady_clock::now()-watchdog_start>std::chrono::milliseconds(live_detail::kChunkWallTimeoutMs))
                throw std::runtime_error("MK2 live chunk wall-clock watchdog expired");
            if (s.cycles >= (host_controls ? (live_host ? machine.LiveHostDeadline() : first_fight_stages.back().end) : end_cycle) ||
                s.instructions - first_instruction >= instruction_count)
                throw std::runtime_error("MK2 batch exceeded measured cooperative horizon");
            if ((s.status & 0x00200000) && (s.board.io[17] & s.board.io[18])) {
                try { if (coin_pair || start_wait || next_wait || prestart_wait || start_input || start_release || selection || first_fight) o.PendingInterrupt(); else o.DisplayInterrupt(); }
                catch(const std::exception& error) {
                    throw std::runtime_error(std::string(error.what())+"; PC "+std::to_string(s.next_pc)+
                        "; cycle "+std::to_string(s.cycles)+"; status "+std::to_string(s.status)+
                        "; pending "+std::to_string(s.board.io[18]));
                }
            }
            if (result.observe_history) {
                std::array<uint64_t, 34> row{s.next_pc, s.status, s.cycles};
                for (unsigned n = 0; n < 16; ++n) row[3+n] = s.a[n];
                for (unsigned n = 0; n < 15; ++n) row[19+n] = s.b[n];
                result.cpu_trace.push_back(row);
            }
        }
        const auto instruction_pc = s.next_pc;
        if(!o.shadow){
        if(instruction_pc==mk2::wide::kMarker)o.out.video.wide_real_post=true;   // the real walk's marker entry
        if(instruction_pc==mk2::wide::kDisplayIrq)o.out.video.EndWideFrame();   // a new frame
        if(instruction_pc==mk2::wide::kStreamer)o.out.video.margin_streamer=o.out.video.margin_streaming=true;   // it streams
        else if(instruction_pc==mk2::wide::kStreamerReturn)o.out.video.margin_streaming=false;   // the streamer returned
        else if(instruction_pc==mk2::wide::kPoolReset[0]||instruction_pc==mk2::wide::kPoolReset[1]){o.out.video.margin_streamer=false;o.out.video.wide_moving.clear();}   // every object is freed: no flight goes on
        if(instruction_pc==mk2::wide::kQueueReset && o.out.video.WideEnabled())   // the frame's queue reset: its page as VRAM holds it
            o.out.video.StartWideFrame(o.out.main_ram[(mk2::wide::kPageOrigin-0x01000000)/16+1]&0x1ff);
        if(instruction_pc==mk5hitbox::kMk2.strike_pc)mk5hitbox::Strike(mk5hitbox::kMk2,s.a.data(),o.out.main_ram);   // training mode, read-only
        else if(instruction_pc==0xff805020 && s.a[0]==0x0105d480)mk5hitbox::Capture(mk5hitbox::kMk2,o.out.main_ram);   // the world objects' DMA
        if(instruction_pc==0xff818d00 || instruction_pc==0xff805020 || instruction_pc==0xff805040 || instruction_pc==0xff9b0e60)
            o.ObservePresentation();
        }
        try { if (!(first_fight ? ExecuteFirstFightExtras(o) : selection ? ExecuteSelectionExtras(o) : start_release ? ExecuteStartReleaseExtras(o) : (prestart_wait || start_input) ? ExecutePreStartWaitExtras(o) : next_wait ? ExecuteNextWaitExtras(o) : start_wait && ExecuteStartWaitExtras(o))) {
        if (coin_pair || start_wait || next_wait || prestart_wait || start_input || start_release || selection || first_fight) { switch (s.next_pc) {
#include "mk2_coin_pair.inc"
        default: throw std::runtime_error("MK2 coin pair PC outside authenticated fixed translation");
        } } else if (coin_accounting) { switch (s.next_pc) {
#include "mk2_coin_accounting.inc"
        default: throw std::runtime_error("MK2 coin accounting PC outside authenticated fixed translation");
        } } else if (coin_queue) { switch (s.next_pc) {
#include "mk2_coin_queue.inc"
        default: throw std::runtime_error("MK2 coin queue PC outside authenticated fixed translation");
        } } else { switch (s.next_pc) {
#include "mk2_task_batch.inc"
        default: throw std::runtime_error("MK2 batch PC outside authenticated fixed translation");
        } } } } catch (const std::exception& error) {
            if(o.shadow&&!shadow_walk){   // the streamer met a list it cannot walk (boot frames): this frame's walk without it
                for(auto it=o.shadow_journal.rbegin();it!=o.shadow_journal.rend();++it)o.out.main_ram[it->first]=it->second;
                o.shadow_journal.clear();std::tie(s.a,s.b,s.status)=shadow_saved;o.shadow_fault=false;
                shadow_view();
                s.next_pc=mk2::wide::kWalkStart;shadow_walk=true;++o.out.video.margin_streamer_skips;continue;
            }
            if(o.shadow){o.shadow_fault=true;shadow_fault_pc=instruction_pc|2;shadow_end();continue;}
            std::string cpu="; CPU34 "+std::to_string(s.next_pc)+","+std::to_string(s.status)+","+std::to_string(s.cycles);
            for(auto value:s.a)cpu+=","+std::to_string(value);
            for(unsigned n=0;n<15;++n)cpu+=","+std::to_string(s.b[n]);
            throw std::runtime_error(std::string(error.what()) + "; TMS PC " + std::to_string(instruction_pc) +
                "; TMS cycle " + std::to_string(s.cycles) + "; status " + std::to_string(s.status) +
                "; DSP PC " + std::to_string(machine.dsp.pc) + "; DSP cycle " + std::to_string(machine.dsp.cycles) +
                "; DSP mstat/imask/icntl " + std::to_string(machine.dsp.mstat) + "/" +
                std::to_string(machine.dsp.imask) + "/" + std::to_string(machine.dsp.icntl) +
                "; DSP pc/loop/stat stacks " + std::to_string(machine.dsp.pc_sp) + "/" +
                std::to_string(machine.dsp.loop_sp) + "/" + std::to_string(machine.dsp.stat_sp)+cpu);
        }
        // The legal field request follows frame_update; physical sampling is on the next frame.
        if (coin_queue && !s.board.requested_coin1 && s.cycles*16 >= 51181900)
            s.board.SetCoin1(true,51181900);
        // Only this suffix owns release; it never replays the prefix's press.
        if (coin_accounting && s.board.input_events.size() == 6 && s.cycles*16 >= 54837750)
            s.board.SetCoin1(false,54837750);
        if (coin_pair && s.board.input_events.size() == 8 && s.cycles*16 >= 62149450)
            s.board.SetCoin1(true,62149450);
        if (coin_pair && s.board.input_events.size() == 10 && s.cycles*16 >= 65805300)
            s.board.SetCoin1(false,65805300);
        if (start_input && s.board.input_events.size()==12 && s.cycles*16>=365585000)
            s.board.SetStart1(true,365585000);
        if (start_input && s.board.input_events.size()==14 && s.cycles*16>=369240850)
            s.board.SetStart1(false,369240850);
        if(selection && s.board.input_events.size()==16 && s.cycles*16>=438702000)
            s.board.SetP1HighPunch(true,438702000);
        if(selection && s.board.input_events.size()==18 && s.cycles*16>=442357850)
            s.board.SetP1HighPunch(false,442357850);
        if(first_fight_high_punch && s.board.input_events.size()==20 && s.cycles*16>=2050931850)
            s.board.SetP1HighPunch(true,2050931850);
        if(first_fight_high_punch && s.board.input_events.size()==22 && s.cycles*16>=2058243550)
            s.board.SetP1HighPunch(false,2058243550);
        if(host_controls && !result.observe_history && s.next_pc==end_pc)
            machine.dsp.DiscardObservedAccesses();
    }
    if(live_host && (!o.CanSuspend() || !IsFirstFightStaticPC(s.next_pc)))
        throw std::runtime_error("MK2 live suspension requires an intact execution step and an executable static PC");
    result.executed_instructions = s.instructions - first_instruction;
    if (!result.observe_history) machine.dsp.DiscardObservedAccesses();
    if(host_controls) return result;
    if(selection || first_fight) {
        if(s.instructions-first_instruction!=instruction_count ||
           s.board.input_events.size()!=(first_fight_high_punch ? 24u : end_cycle>=28004306 ? 20u : 16u) ||
           s.board.requested_p1_high_punch || s.board.requested_start1 || s.board.requested_coin1 ||
           s.board.input0!=0xffff || s.board.input1!=0xffff || s.board.input2!=0xffff ||
           s.board.dip_switches!=0xfc7d) throw std::runtime_error("MK2 selection instruction/input horizon differs");
        return result;
    }
    if (start_release) {
        static constexpr char expected[]="9b9ac7ce9b16b5e5d7b435cddf15a00525efc235b6b7e45e39df3ad355bd402e"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
        std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
        for(unsigned n=0;n<16;++n) cpu[3+n]=s.a[n];
        for(unsigned n=0;n<15;++n) cpu[19+n]=s.b[n];
        if(!Cpu34Is(cpu,expected) || s.instructions-first_instruction!=instruction_count ||
           s.board.input_events.size()!=16 || s.board.requested_start1 || s.board.requested_coin1 ||
           s.board.requested_p1_high_punch || s.board.input0!=0xffff || s.board.input1!=0xffff ||
           s.board.input2!=0xffff || s.board.dip_switches!=0xfc7d)
            throw std::runtime_error("MK2 Start release CPU/instruction/input horizon differs");
        o.Checkpoint("start_release_ready");
        return result;
    }
    if (start_input) {
        static constexpr char expected[]="6298871e010dac2c37f170a29060bbf5c3d7264ea203f2f94add9bfa5f70f79e"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
        std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
        for(unsigned n=0;n<16;++n) cpu[3+n]=s.a[n];
        for(unsigned n=0;n<15;++n) cpu[19+n]=s.b[n];
        if(!Cpu34Is(cpu,expected) || s.instructions-first_instruction!=instruction_count ||
           s.board.input_events.size()!=15 || s.board.requested_start1 || s.board.requested_coin1 ||
           s.board.requested_p1_high_punch || s.board.input0!=0xffff || s.board.input1!=0xfffb ||
           s.board.input2!=0xffff || s.board.dip_switches!=0xfc7d)
            throw std::runtime_error("MK2 Start input CPU/instruction/input horizon differs");
        o.Checkpoint("start_input_ready");
        return result;
    }
    if(s.board.requested_start1) throw std::runtime_error("MK2 unexpected Start request outside Start input mode");
    if (prestart_wait) {
        if(s.instructions-first_instruction!=instruction_count || s.board.input_events.size()!=12 ||
           s.board.requested_coin1 || s.board.requested_p1_high_punch || s.board.input0!=0xffff ||
           s.board.input1!=0xffff || s.board.input2!=0xffff || s.board.dip_switches!=0xfc7d)
            throw std::runtime_error("MK2 pre-Start instruction/input horizon differs");
        o.Checkpoint("prestart_wait_ready");
        return result;
    }
    if (next_wait) {
        static constexpr char expected[]="462a678e1e3037dc8310234e3627e3127f890e139ee1fcac48c68b20bba9b0a4"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
        std::array<uint64_t,34> actual{s.next_pc,s.status,s.cycles};
        for (unsigned n=0;n<16;++n) actual[3+n]=s.a[n];
        for (unsigned n=0;n<15;++n) actual[19+n]=s.b[n];
        if (!Cpu34Is(actual,expected) || s.instructions-first_instruction != instruction_count ||
            s.board.input_events.size()!=12 || s.board.requested_coin1 || s.board.requested_p1_high_punch ||
            s.board.input0!=0xffff || s.board.input1!=0xffff)
            throw std::runtime_error("MK2 next wait differs from the measured CPU/input horizon");
        o.Checkpoint("next_wait_ready");
        return result;
    }
    if (start_wait) {
        // ST, SP and A13 of the Original at this horizon: SHA-256 only (mk2_oracle_digest.hpp).
        if (s.instructions - first_instruction != instruction_count ||
            WordsDigest({s.status,s.a[15],s.a[13]}) != "64cfe62d418fbd07958343677b6f817d12cec60407089a4764c10b4f7002b78e" ||
            s.board.input_events.size() != 12 ||
            s.board.requested_coin1 || s.board.requested_p1_high_punch ||
            s.board.input0 != 0xffff || s.board.input1 != 0xffff)
            throw std::runtime_error("MK2 Start wait differs from measured instruction/input horizon");
        o.Checkpoint("start_wait_ready");
        return result;
    }
    if (coin_pair) {
        if (s.instructions - first_instruction != instruction_count ||
            WordsDigest({s.status,s.a[15]}) != "71863ff00ef70e6edf71e043505d5618c5e0f1a6405096fe7436ea2461f6bdd8" ||
            s.board.input_events.size() != 12 || s.board.requested_coin1 ||
            s.board.input1 != 0xffff)
            throw std::runtime_error("MK2 coin pair differs from measured instruction/input horizon");
        o.Checkpoint("coin_pair_ready");
        return result;
    }
    if (s.instructions - first_instruction != instruction_count ||
        (coin_queue ? WordsDigest({s.status,s.a[15],s.a[13]}) != "408d73a3a97843a4ddd3941d81c0eb04ea9eb6afc9a393a95d2e3919d02b6d7e"
                    : WordsDigest({s.status,s.a[15],s.a[13],s.a[1]}) != "c1275655fc039f648426e6c1e22a7ccefbdf3c32090043c2755c0e14ac12af21") ||
        machine.DmaCompletions() != dma_completions || s.board.dma_command != 0xc382 ||
        machine.DmaDeadlineSubticks() != dma_start*160 + uint64_t(4864)*41)
        throw std::runtime_error("MK2 batch instruction count differs from measured horizon");
    if (coin_queue && (s.board.input0 != 0xffff || s.board.input1 != 0xfffe ||
        !s.board.requested_coin1 || s.board.input_events.size() != 6))
        throw std::runtime_error("MK2 original coin request/physical timeline differs");
    if (coin_accounting && (s.board.input0 != 0xffff || s.board.input1 != 0xffff ||
        s.board.requested_coin1 || s.board.input_events.size() != 8))
        throw std::runtime_error("MK2 original coin release/physical timeline differs");
    o.Checkpoint(coin_accounting ? "coin_accounting_ready" : coin_queue ? "coin_queue_ready" : "task_batch_ready");
    return result;
}

TaskBatchInit RunCoinPairInit(const Program& program, const TaskBatchInit& prefix, DcsBootMachine& machine) {
    // The caller reaches this prefix from the unchanged reset and all previous fixed translations.
    if (program.bytes.size() != 0x100000 || Sha256(program.bytes) != kProgramSha256 ||
        program.graphics.size() != 0xc00000 || Sha256(program.graphics) != kGraphicsSha256 ||
        prefix.ready.next_pc != 0xff80dd30 || prefix.ready.cycles != 3658091 ||
        prefix.main_ram.size() != 262144 || prefix.ready.board.scanline_events != 9239 ||
        prefix.ready.board.input_events.size() != 8 || prefix.ready.board.requested_coin1)
        throw std::runtime_error("MK2 coin pair requires the actual released-coin native prefix");
    static constexpr char expected[]="80941a4304513ff90741b163408dbd4e4dc7375d47d99c3c84e23d631c1b42c6"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
    std::array<uint64_t,34> actual{prefix.ready.next_pc,prefix.ready.status,prefix.ready.cycles};
    for(unsigned n=0;n<16;++n) actual[3+n]=prefix.ready.a[n];
    for(unsigned n=0;n<15;++n) actual[19+n]=prefix.ready.b[n];
    if(!Cpu34Is(actual,expected)) throw std::runtime_error("MK2 coin pair native prefix CPU differs");
    auto admitted = prefix;
    machine.AdmitCoinPair(admitted.ready.board);
    machine.dsp.DiscardObservedTrace();
    return ExecuteTaskBatch(program,admitted,machine,5077311,485629,0,0,false,false,true);
}

TaskBatchInit RunTaskBatchInit(const Program& program, const BootContinuation& prefix,
                              DcsBootMachine& machine, bool extended, bool next_batch) {
    if (program.bytes.size() != 0x100000 || program.sha256 != kProgramSha256 ||
        Sha256(program.bytes) != kProgramSha256 || program.graphics.size() != 0xc00000 ||
        Sha256(program.graphics) != kGraphicsSha256)
        throw std::runtime_error("MK2 batch requires verified program and graphics ROMs");
    if (prefix.ready.next_pc != 0xff80dd30 || prefix.ready.cycles != 1261260 ||
        WordsDigest({prefix.ready.status,prefix.ready.a[15],prefix.ready.a[13],prefix.ready.board.io[17],prefix.ready.board.io[18]}) !=
            "1cb98ac1d664dc687af727090a72966c14d0a693b9e2c6b9bed52d318ae27015" || prefix.main_ram.size() != 262144 ||
        machine.LastMainCycles() != prefix.ready.cycles || machine.dsp.InstructionCount() != 1998125 ||
        machine.dsp.pc != 0x131 || machine.dsp.cycles != 744145 ||
        machine.DmaDeadlineSubticks() || machine.DmaCompletions() != 1)
        throw std::runtime_error("MK2 batch requires synchronized first loaded drawing boundary");
    const auto& inputs = prefix.ready.board;
    if (inputs.input0 != 0xffff || inputs.input1 != 0xffff || inputs.input2 != 0xffff ||
        inputs.dip_switches != 0xfc7d || inputs.requested_p1_high_punch || inputs.input_events.size() != 4)
        throw std::runtime_error("MK2 batch requires the qualified released-button timeline");
    return ExecuteTaskBatch(program,prefix,machine,next_batch ? 2173200 : extended ? 1716210 : 1487718,
        next_batch ? 202367 : extended ? 100962 : 50260,next_batch ? 8u : extended ? 4u : 2u,
        next_batch ? 2172772 : extended ? 1715788 : 1487299);
}

TaskBatchInit RunStartWaitInit(const Program& program, const TaskBatchInit& prefix, DcsBootMachine& machine) {
    if (program.bytes.size() != 0x100000 || Sha256(program.bytes) != kProgramSha256 ||
        program.graphics.size() != 0xc00000 || Sha256(program.graphics) != kGraphicsSha256 ||
        prefix.main_ram.size() != 262144 || prefix.executed_instructions != 485629 ||
        (prefix.observe_history && prefix.cpu_trace.size() != 485629))
        throw std::runtime_error("MK2 Start wait requires the actual authenticated C7 prefix");
    static constexpr char expected[]="866339cdf17ba806b96e4ebf9fbfc325fc40e6910d9202a0ce477e9f28a03ce4"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
    std::array<uint64_t,34> actual{prefix.ready.next_pc,prefix.ready.status,prefix.ready.cycles};
    for (unsigned n=0;n<16;++n) actual[3+n]=prefix.ready.a[n];
    for (unsigned n=0;n<15;++n) actual[19+n]=prefix.ready.b[n];
    if (!Cpu34Is(actual,expected)) throw std::runtime_error("MK2 Start wait native C7 CPU differs");
    BootContinuation admitted=prefix;
    machine.AdmitStartWait(admitted.ready.board);
    machine.dsp.DiscardObservedTrace();
    return ExecuteTaskBatch(program,admitted,machine,6087360,425616,0,0,false,false,false,true);
}

TaskBatchInit RunNextWaitInit(const Program& program, const TaskBatchInit& prefix, DcsBootMachine& machine) {
    if (program.bytes.size()!=0x100000 || Sha256(program.bytes)!=kProgramSha256 ||
        program.graphics.size()!=0xc00000 || Sha256(program.graphics)!=kGraphicsSha256 ||
        prefix.main_ram.size()!=262144 || prefix.executed_instructions!=425616 ||
        (prefix.observe_history && prefix.cpu_trace.size()!=425616))
        throw std::runtime_error("MK2 next wait requires the actual authenticated C8 prefix");
    static constexpr char expected[]="7fc1846f25370ddc0f96b34dee3576b0bf931de8e0534734c1053e32961d896f"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
    std::array<uint64_t,34> actual{prefix.ready.next_pc,prefix.ready.status,prefix.ready.cycles};
    for (unsigned n=0;n<16;++n) actual[3+n]=prefix.ready.a[n];
    for (unsigned n=0;n<15;++n) actual[19+n]=prefix.ready.b[n];
    if (!Cpu34Is(actual,expected)) throw std::runtime_error("MK2 next wait native C8 CPU differs");
    BootContinuation admitted=prefix;
    machine.AdmitNextWait(admitted.ready.board);
    machine.dsp.DiscardObservedTrace();
    return ExecuteTaskBatch(program,admitted,machine,7005800,392474,0,0,false,false,false,false,true);
}

static TaskBatchInit ExecutePreStartStage(const Program& program,const TaskBatchInit& prefix,
                                         DcsBootMachine& machine,unsigned index) {
    const auto& stage=prestart_stages.at(index);
    auto result=ExecuteTaskBatch(program,prefix,machine,stage.end,stage.instructions,0,0,
                                 false,false,false,false,false,true);
    const auto& s=result.ready;
    std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
    for(unsigned n=0;n<16;++n) cpu[3+n]=s.a[n];
    for(unsigned n=0;n<15;++n) cpu[19+n]=s.b[n];
    if(!Cpu34Is(cpu,stage.cpu)) throw std::runtime_error("MK2 pre-Start CPU34 boundary differs");
    return result;
}

static TaskBatchInit ExecuteStartInputStage(const Program& program,const TaskBatchInit& prefix,DcsBootMachine& machine) {
    return ExecuteTaskBatch(program,prefix,machine,23108559,439431,0,0,false,false,false,false,false,false,true);
}

static TaskBatchInit ExecuteSelectionStage(const Program& program,const TaskBatchInit& prefix,DcsBootMachine& machine,unsigned index) {
    const auto& stage=selection_stages.at(index);
    auto result=ExecuteTaskBatch(program,prefix,machine,stage.end,stage.instructions,0,0,
                                 false,false,false,false,false,false,false,false,true);
    const auto& s=result.ready;
    std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
    for(unsigned n=0;n<16;++n) cpu[3+n]=s.a[n];
    for(unsigned n=0;n<15;++n) cpu[19+n]=s.b[n];
    if(!Cpu34Is(cpu,stage.cpu)) throw std::runtime_error("MK2 selection CPU34 boundary differs");
    return result;
}

static TaskBatchInit ExecuteFirstFightStage(const Program& program,const TaskBatchInit& prefix,DcsBootMachine& machine,unsigned index) {
    const auto& stage=first_fight_stages.at(index);
    auto result=ExecuteTaskBatch(program,prefix,machine,stage.end,stage.instructions,0,0,
                                 false,false,false,false,false,false,false,false,false,true);
    const auto& s=result.ready;
    std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
    for(unsigned n=0;n<16;++n) cpu[3+n]=s.a[n];
    for(unsigned n=0;n<15;++n) cpu[19+n]=s.b[n];
    if(!Cpu34Is(cpu,stage.cpu)) throw std::runtime_error("MK2 first-fight CPU34 boundary differs");
    return result;
}

static TaskBatchInit ExecuteFirstFightHighPunch(const Program& program,const TaskBatchInit& prefix,DcsBootMachine& machine) {
    const auto& stage=first_fight_high_punch_stage[0];
    auto result=ExecuteTaskBatch(program,prefix,machine,stage.end,stage.instructions,0,0,
                                 false,false,false,false,false,false,false,false,false,true,true);
    const auto& s=result.ready;
    std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
    for(unsigned n=0;n<16;++n) cpu[3+n]=s.a[n];
    for(unsigned n=0;n<15;++n) cpu[19+n]=s.b[n];
    if(!Cpu34Is(cpu,stage.cpu)) throw std::runtime_error("MK2 high-punch CPU34 boundary differs");
    // Request and physical latch events 20..23 (SHA-256 of the flattened rows, mk2_oracle_digest.hpp).
    if(s.board.input_events.size()<24 ||
       RowsDigest(std::vector<std::array<uint64_t,4>>(s.board.input_events.begin()+20,s.board.input_events.begin()+24))!=
       "5de63653c8383f3b8abe1f5907d6d5a66b635f530800ee1a8fab833f41d31085") throw std::runtime_error("MK2 high-punch request/physical latch differs");
    return result;
}

TaskBatchInit RunTranslatedContinuation(const Program& program, const BootContinuation& prefix,
    DcsBootMachine& machine, int first_fight_window, bool observe_history) {
    if (first_fight_window < -1 || first_fight_window >= int(kFirstFightWindowCount))
        throw std::runtime_error("MK2 continuation endpoint outside fixed translation");
    auto admitted = prefix;
    admitted.observe_history = observe_history;
    auto result = RunTaskBatchInit(program,admitted,machine,false,true);
    result = ExecuteTaskBatch(program,result,machine,3087189,202812,16,3086732);
    result = ExecuteTaskBatch(program,result,machine,3315882,50320,18,3314873,true);
    result = ExecuteTaskBatch(program,result,machine,3658091,75020,21,3657534,false,true);
    result = RunCoinPairInit(program,result,machine);
    result = RunStartWaitInit(program,result,machine);
    if (first_fight_window == -1) return RunNextWaitInit(program,result,machine);
    machine.AdmitFirstFight(result.ready.board);
    for (unsigned n=0;n<kPreStartWindowCount;++n)
        result = ExecutePreStartStage(program,result,machine,n);
    result = ExecuteStartInputStage(program,result,machine);
    result = ExecuteTaskBatch(program,result,machine,24004881,389582,0,0,
                             false,false,false,false,false,false,false,true);
    for (unsigned n=0;n<kSelectionWindowCount;++n)
        result = ExecuteSelectionStage(program,result,machine,n);
    for (int n=0;n<=first_fight_window;++n)
        result = ExecuteFirstFightStage(program,result,machine,unsigned(n));
    return result;
}

HostControlBounds HostControlRange(unsigned start_window,int stop_window) {
    if(start_window>=kFirstFightWindowCount || stop_window< -1 ||
       stop_window>=int(kFirstFightWindowCount)-1)
        throw std::runtime_error("MK2 host range outside translated endpoints");
    const uint64_t stop=stop_window<0 ? 130000000 : first_fight_stages[unsigned(stop_window)].end;
    const auto& start=first_fight_stages[start_window];
    if(stop<=start.end || stop>first_fight_stages[kFirstFightWindowCount-2].end)
        throw std::runtime_error("MK2 host range requires increasing endpoints and a final guard margin");
    return {start_window,start.end,stop,first_fight_stages.back().end};
}

TaskBatchInit RunHostContinuation(const Program& program,const TaskBatchInit& prefix,
    DcsBootMachine& machine,std::function<uint32_t(uint64_t)> input_provider,
    unsigned start_window,int stop_window) {
    const auto range=HostControlRange(start_window,stop_window);
    const auto& stage=first_fight_stages[start_window];
    const auto& s=prefix.ready;
    std::array<uint64_t,34> cpu{s.next_pc,s.status,s.cycles};
    for(unsigned n=0;n<16;++n)cpu[3+n]=s.a[n];
    for(unsigned n=0;n<15;++n)cpu[19+n]=s.b[n];
    if(machine.ValidCmosHostAdmitted() || !input_provider || !Cpu34Is(cpu,stage.cpu) || prefix.main_ram.size()!=262144 ||
       s.cmos.size()!=8192 || prefix.video.vram.size()!=524288 || prefix.video.palette_ram.size()!=32768 ||
       machine.ScanlineEvents()!=s.board.scanline_events ||
       machine.LastMainCycles()!=s.cycles || !machine.FirstFightAdmitted() ||
       !s.board.CoinPairAdmitted() || s.board.input_events.size()!=20 ||
       s.board.dip_switches!=0xfc7d || program.bytes.size()!=0x100000 ||
       Sha256(program.bytes)!=kProgramSha256 || program.graphics.size()!=0xc00000 ||
       Sha256(program.graphics)!=kGraphicsSha256)
        throw std::runtime_error("MK2 host execution requires the actual authenticated first-fight prefix");
    auto admitted=prefix;
    admitted.ready.board.SetHostInputProvider(std::move(input_provider),s.cycles*16);
    auto result=ExecuteTaskBatch(program,admitted,machine,range.minimum_stop_cycle,
        range.maximum_cycle-s.cycles,0,0,
        false,false,false,false,false,false,false,false,false,true,false,true);
    result.host_origin=HostOrigin::cold_first_fight;return result;
}

TaskBatchInit ResumeHostContinuation(const Program& program,const TaskBatchInit& prefix,
    DcsBootMachine& machine,uint64_t minimum_stop_cycle) {
    const auto& s=prefix.ready;
    if(prefix.host_origin!=HostOrigin::cold_first_fight || machine.ValidCmosHostAdmitted() || !s.board.HostInputProviderActive() || s.next_pc!=0xff80dd30 ||
       minimum_stop_cycle<=s.cycles || minimum_stop_cycle>first_fight_stages[kFirstFightWindowCount-2].end ||
       prefix.main_ram.size()!=262144 || s.cmos.size()!=8192 ||
       prefix.video.vram.size()!=524288 || prefix.video.palette_ram.size()!=32768 ||
       machine.ScanlineEvents()!=s.board.scanline_events || machine.LastMainCycles()!=s.cycles ||
       !machine.FirstFightAdmitted() || !s.board.CoinPairAdmitted() ||
       s.board.input_events.size()<=20 || s.board.input_events.back()[0]>s.cycles*16 ||
       s.board.dip_switches!=0xfc7d || program.bytes.size()!=0x100000 ||
       Sha256(program.bytes)!=kProgramSha256 || program.graphics.size()!=0xc00000 ||
       Sha256(program.graphics)!=kGraphicsSha256)
        throw std::runtime_error("MK2 host resume requires the synchronized active host session and increasing translated stop");
    auto result=ExecuteTaskBatch(program,prefix,machine,minimum_stop_cycle,
        first_fight_stages.back().end-s.cycles,0,0,
        false,false,false,false,false,false,false,false,false,true,false,true);
    result.host_origin=HostOrigin::cold_first_fight;return result;
}


ValidHostControlBounds ValidHostControlRange() { return {7316296,148081944,150023965}; }

static void RequireValidHostMemory(const Program& program,const BootContinuation& prefix,
    const DcsBootMachine& machine,uint64_t minimum_stop_cycle) {
    const auto range=ValidHostControlRange();const auto& s=prefix.ready;
    if(minimum_stop_cycle<=s.cycles || minimum_stop_cycle>range.maximum_stop_cycle ||
       s.next_pc!=0xff80dd30 || prefix.main_ram.size()!=262144 || s.cmos.size()!=8192 ||
       prefix.video.vram.size()!=524288 || prefix.video.palette_ram.size()!=32768 ||
       machine.ScanlineEvents()!=s.board.scanline_events || machine.LastMainCycles()!=s.cycles ||
       s.board.dip_switches!=0xfc7d || program.bytes.size()!=0x100000 ||
       Sha256(program.bytes)!=kProgramSha256 || program.graphics.size()!=0xc00000 ||
       Sha256(program.graphics)!=kGraphicsSha256)
        throw std::runtime_error("MK2 valid host requires synchronized cooperative state and increasing finite stop");
}

TaskBatchInit RunValidHostContinuation(const Program& program,const ValidCmosContinuation& prefix,
    DcsBootMachine& machine,std::function<uint32_t(uint64_t)> provider,uint64_t minimum_stop_cycle) {
    RequireValidHostMemory(program,prefix,machine,minimum_stop_cycle);
    static constexpr char expected[]="d73e16802db7d0d363fac475f467ef5018a9f8a7ce6692c4c206f8f4113e255c"; // CPU34 SHA-256 (mk2_oracle_digest.hpp)
    const auto& s=prefix.ready;std::array<uint64_t,34> actual{s.next_pc,s.status,s.cycles};
    for(unsigned n=0;n<16;++n)actual[3+n]=s.a[n];
    for(unsigned n=0;n<15;++n)actual[19+n]=s.b[n];
    if(!provider || !Cpu34Is(actual,expected) || !machine.ValidCmosDrawAdmitted() ||
       s.board.HostInputProviderActive() || !s.board.input_events.empty() ||
       s.board.requested_p1_high_punch || s.board.requested_coin1 || s.board.requested_start1 ||
       s.board.input0!=0xffff || s.board.input1!=0xffff || s.board.input2!=0xffff)
        throw std::runtime_error("MK2 valid host requires the original input-free valid-CMOS first draw");
    BootContinuation admitted=prefix;
    admitted.ready.board.SetHostInputProvider(std::move(provider),s.cycles*16);
    machine.AdmitValidCmosHost(admitted.ready.board);
    auto result=ExecuteTaskBatch(program,admitted,machine,minimum_stop_cycle,
        ValidHostControlRange().maximum_cycle-s.cycles,0,0,
        false,false,false,false,false,false,false,false,false,true,false,true);
    result.host_origin=HostOrigin::valid_cmos_draw;return result;
}

TaskBatchInit ResumeValidHostContinuation(const Program& program,const TaskBatchInit& prefix,
    DcsBootMachine& machine,uint64_t minimum_stop_cycle) {
    RequireValidHostMemory(program,prefix,machine,minimum_stop_cycle);
    const auto& s=prefix.ready;
    if(prefix.host_origin!=HostOrigin::valid_cmos_draw || !machine.ValidCmosHostAdmitted() ||
       s.board.requested_p1_high_punch || s.board.requested_coin1 || s.board.requested_start1 ||
       !s.board.HostInputProviderActive() || (!s.board.input_events.empty() && s.board.input_events.back()[0]>s.cycles*16))
        throw std::runtime_error("MK2 valid host resume requires its own active provider origin");
    for(const auto& event:s.board.input_events)if(event[3]!=6 && event[3]!=7)
        throw std::runtime_error("MK2 valid host resume cannot contain a cold scripted input");
    auto result=ExecuteTaskBatch(program,prefix,machine,minimum_stop_cycle,
        ValidHostControlRange().maximum_cycle-s.cycles,0,0,
        false,false,false,false,false,false,false,false,false,true,false,true);
    result.host_origin=HostOrigin::valid_cmos_draw;return result;
}

void WriteTaskBatchObservations(const Program& program, const BootContinuation& prefix, DcsBootMachine& machine, bool extended, bool next_batch, bool larger_batch, bool coin_queue, bool coin_accounting, bool coin_pair, bool start_wait, bool next_wait, int prestart_window, bool start_input, bool start_release, int selection_window, int first_fight_window, bool first_fight_high_punch) {
    std::unique_ptr<TaskBatchInit> previous;
    const bool selection_wait=selection_window>=0,first_fight=first_fight_window>=0;
    const bool after_start=selection_wait||first_fight;
    if(first_fight_high_punch && first_fight_window!=86)
        throw std::runtime_error("MK2 high-punch branch has one explicit endpoint");
    if(first_fight && (unsigned(first_fight_window)>=kFirstFightWindowCount || extended || next_batch || larger_batch || coin_queue || coin_accounting || coin_pair || start_wait || next_wait || prestart_window>=0 || start_input || start_release || selection_wait))
        throw std::runtime_error("MK2 select one measured first-fight endpoint");
    if(selection_wait && (first_fight || unsigned(selection_window)>=kSelectionWindowCount || extended || next_batch || larger_batch || coin_queue || coin_accounting || coin_pair || start_wait || next_wait || prestart_window>=0 || start_input || start_release))
        throw std::runtime_error("MK2 select one measured selection endpoint");
    const bool prestart_wait=prestart_window>=0;
    if(start_release && (extended || next_batch || larger_batch || coin_queue || coin_accounting || coin_pair || start_wait || next_wait || prestart_wait || start_input))
        throw std::runtime_error("MK2 select one Start release endpoint");
    if(start_input && (extended || next_batch || larger_batch || coin_queue || coin_accounting || coin_pair || start_wait || next_wait || prestart_wait))
        throw std::runtime_error("MK2 select one Start input endpoint");
    if(prestart_wait && unsigned(prestart_window)>=kPreStartWindowCount) throw std::runtime_error("MK2 invalid pre-Start window");
    const bool pair_prefix=coin_pair || start_wait || next_wait || prestart_wait || start_input || start_release || after_start;
    // This mode emits only the new interval; historical integral observations have their own unchanged gate.
    if (pair_prefix) machine.dsp.DiscardObservedTrace();
    if (first_fight) machine.dsp.SetTraceObservation(false);
    if (larger_batch || coin_queue || coin_accounting || pair_prefix) previous.reset(new TaskBatchInit(RunTaskBatchInit(program,prefix,machine,false,true)));
    if (pair_prefix) machine.dsp.DiscardObservedTrace();
    if (coin_queue || coin_accounting || pair_prefix) previous.reset(new TaskBatchInit(ExecuteTaskBatch(program,*previous,machine,3087189,202812,16,3086732)));
    if (pair_prefix) machine.dsp.DiscardObservedTrace();
    if (coin_accounting || pair_prefix) previous.reset(new TaskBatchInit(ExecuteTaskBatch(program,*previous,machine,3315882,50320,18,3314873,true)));
    if (pair_prefix) {
        machine.dsp.DiscardObservedTrace();
        previous.reset(new TaskBatchInit(ExecuteTaskBatch(program,*previous,machine,3658091,75020,21,3657534,false,true)));
        machine.dsp.DiscardObservedTrace();
    }
    if (start_wait || next_wait || prestart_wait || start_input || start_release || after_start) {
        previous.reset(new TaskBatchInit(RunCoinPairInit(program,*previous,machine)));
        machine.dsp.DiscardObservedTrace();
    }
    if (next_wait || prestart_wait || start_input || start_release || after_start) {
        previous.reset(new TaskBatchInit(RunStartWaitInit(program,*previous,machine)));
        machine.dsp.DiscardObservedTrace();
    }
    if(prestart_wait || start_input || start_release || after_start) {
        if(first_fight) machine.AdmitFirstFight(previous->ready.board);
        else if(selection_wait) machine.AdmitSelection(previous->ready.board);
        else if(start_release) machine.AdmitStartRelease(previous->ready.board);
        else if(start_input) machine.AdmitStartInput(previous->ready.board);
        else machine.AdmitPreStartWait(previous->ready.board);
        const int preceding=(start_input || start_release || after_start) ? int(kPreStartWindowCount) : prestart_window;
        for(int n=0;n<preceding;++n) {
            previous.reset(new TaskBatchInit(ExecutePreStartStage(program,*previous,machine,unsigned(n))));
            machine.dsp.DiscardObservedTrace();
        }
    }
    if(start_release || after_start) {
        previous.reset(new TaskBatchInit(ExecuteStartInputStage(program,*previous,machine)));
        machine.dsp.DiscardObservedTrace();
    }
    if(after_start) {
        previous.reset(new TaskBatchInit(ExecuteTaskBatch(program,*previous,machine,24004881,389582,0,0,false,false,false,false,false,false,false,true)));
        machine.dsp.DiscardObservedTrace();
        for(int n=0;n<(first_fight?int(kSelectionWindowCount):selection_window);++n) {
            previous.reset(new TaskBatchInit(ExecuteSelectionStage(program,*previous,machine,unsigned(n))));
            machine.dsp.DiscardObservedTrace();
        }
    }
    if(first_fight) for(int n=0;n<first_fight_window;++n) {
        previous.reset(new TaskBatchInit(ExecuteFirstFightStage(program,*previous,machine,unsigned(n))));
        machine.dsp.DiscardObservedTrace();
    }
    if (first_fight) machine.dsp.SetTraceObservation(true);
    const auto trace_begin=machine.dsp.trace.size(), access_begin=machine.dsp.accesses.size();
    const auto pcm_begin=machine.dsp.sport_pcm.size(), transfer_begin=machine.dsp.sport_transfers.size();
    const auto irq_begin=machine.dsp.sport_irq1_events.size();
    const std::unique_ptr<TaskBatchInit> owner(new TaskBatchInit(first_fight_high_punch ?
        ExecuteFirstFightHighPunch(program,*previous,machine) : first_fight ?
        ExecuteFirstFightStage(program,*previous,machine,unsigned(first_fight_window)) : selection_wait ?
        ExecuteSelectionStage(program,*previous,machine,unsigned(selection_window)) : start_release ?
        ExecuteTaskBatch(program,*previous,machine,24004881,389582,0,0,false,false,false,false,false,false,false,true) : start_input ?
        ExecuteStartInputStage(program,*previous,machine) : prestart_wait ?
        ExecutePreStartStage(program,*previous,machine,unsigned(prestart_window)) : next_wait ?
        RunNextWaitInit(program,*previous,machine) : start_wait ?
        RunStartWaitInit(program,*previous,machine) : coin_pair ?
        RunCoinPairInit(program,*previous,machine) : coin_accounting ?
        ExecuteTaskBatch(program,*previous,machine,3658091,75020,21,3657534,false,true) : coin_queue ?
        ExecuteTaskBatch(program,*previous,machine,3315882,50320,18,3314873,true) : larger_batch ?
        ExecuteTaskBatch(program,*previous,machine,3087189,202812,16,3086732) :
        RunTaskBatchInit(program,prefix,machine,extended,next_batch)));
    previous.reset();
    const auto& result=*owner;
    auto numbers=[](const auto& values) {
        std::cout << '['; bool first=true;
        for(auto value:values) { if(!first) std::cout << ','; first=false; std::cout << value; }
        std::cout << ']';
    };
    auto rows=[&](const auto& values,size_t begin) {
        std::cout << '[';
        for(size_t n=begin;n<values.size();++n) { if(n!=begin) std::cout << ','; numbers(values[n]); }
        std::cout << ']';
    };
    std::cout << ",\"batch_cpu\":"; rows(result.cpu_trace,0);
    std::cout << ",\"batch_dsp_trace\":"; rows(machine.dsp.trace,trace_begin);
    std::cout << ",\"batch_dsp_accesses\":[";
    for(size_t n=access_begin;n<machine.dsp.accesses.size();++n) {
        const auto& a=machine.dsp.accesses[n]; if(n!=access_begin) std::cout << ',';
        numbers(std::array<uint64_t,5>{uint64_t(a.space),a.pc,a.address,a.value,a.cycle});
    }
    std::cout << ']';
    std::cout << ",\"batch_transfers\":"; rows(machine.dsp.sport_transfers,transfer_begin);
    std::cout << ",\"batch_irq_events\":"; rows(machine.dsp.sport_irq1_events,irq_begin);
    std::vector<uint64_t> dma_positions;
    for(size_t n=transfer_begin;n<machine.dsp.sport_dma_positions.size();++n)
        dma_positions.push_back(machine.dsp.sport_dma_positions[n]-access_begin);
    std::cout << ",\"batch_dma_positions\":"; numbers(dma_positions);
    std::cout << ",\"batch_bus\":[";
    for(size_t n=0;n<result.bus.size();++n) {
        const auto& a=result.bus[n]; if(n) std::cout << ',';
        numbers(std::array<uint64_t,6>{uint64_t(a.kind),a.pc,a.address_bit,a.value,a.cycle,16});
    }
    std::cout << ']';
    std::cout << ",\"batch_main_ram\":"; numbers(result.main_ram);
    std::cout << ",\"batch_cmos\":"; numbers(result.ready.cmos);
    std::cout << ",\"batch_palette_ram\":"; numbers(result.video.palette_ram);
    std::cout << ",\"batch_vram\":"; numbers(result.video.vram);
    std::cout << ",\"batch_shiftreg\":"; numbers(result.video.shift_register);
    std::cout << ",\"batch_io\":"; numbers(result.ready.board.io);
    std::cout << ",\"batch_dma\":"; numbers(result.video.DmaRegisters(result.ready.board));
    std::cout << ",\"batch_display\":"; numbers((std::array<unsigned,2>{result.ready.board.vcount,result.ready.board.display_address}));
    std::cout << ",\"batch_pcm\":"; numbers((std::vector<int16_t>(machine.dsp.sport_pcm.begin()+pcm_begin,machine.dsp.sport_pcm.end())));
    std::cout << ",\"batch_dac\":"; numbers(machine.dsp.DacBuffer(32768));
    std::cout << ",\"batch_dsp_device\":"; numbers(machine.dsp.DeviceState());
    std::cout << ",\"batch_dsp_pm\":"; numbers(machine.dsp.pm);
    std::cout << ",\"batch_dsp_dm\":"; numbers(machine.dsp.dm);
    std::cout << ",\"batch_dsp_external_pm\":"; numbers(machine.dsp.external_pm);
    std::cout << ",\"batch_dsp_final\":"; numbers(machine.dsp.Registers());
    std::cout << ",\"batch_irq_state\":";
    numbers(std::array<unsigned,2>{unsigned(machine.dsp.sport_irq1_asserted),unsigned(machine.dsp.sport_irq1_latched)});
    std::cout << ",\"batch_inputs\":"; rows(result.ready.board.input_events,0);
    std::cout << ",\"batch_ports\":";
    if(start_input || start_release || after_start) numbers(std::array<unsigned,7>{result.ready.board.input0,result.ready.board.input1,result.ready.board.input2,
        result.ready.board.dip_switches,unsigned(result.ready.board.requested_p1_high_punch),unsigned(result.ready.board.requested_coin1),unsigned(result.ready.board.requested_start1)});
    else if (coin_queue || coin_accounting || pair_prefix) numbers(std::array<unsigned,6>{result.ready.board.input0,result.ready.board.input1,result.ready.board.input2,
        result.ready.board.dip_switches,unsigned(result.ready.board.requested_p1_high_punch),unsigned(result.ready.board.requested_coin1)});
    else numbers(std::array<unsigned,5>{result.ready.board.input0,result.ready.board.input1,result.ready.board.input2,
                                       result.ready.board.dip_switches,unsigned(result.ready.board.requested_p1_high_punch)});
    if(start_release || after_start) {
        std::cout << ",\"batch_protection\":";
        numbers(std::array<unsigned,1>{result.ready.board.protection.Read()});
    }
    std::vector<uint16_t> external_dm;
    for(auto word:machine.dsp.external_pm) external_dm.push_back(uint16_t(word>>8));
    std::cout << ",\"batch_dsp_external_dm\":"; numbers(external_dm);
    std::array<uint16_t,32> control{};
    for(unsigned n=0;n<control.size();++n) control[n]=machine.dsp.ReadControl(uint16_t(0x3fe0+n));
    std::cout << ",\"batch_dsp_control\":"; numbers(control);
    std::array<uint64_t,34> final{result.ready.next_pc,result.ready.status,result.ready.cycles};
    for(unsigned n=0;n<16;++n) final[3+n]=result.ready.a[n];
    for(unsigned n=0;n<15;++n) final[19+n]=result.ready.b[n];
    std::cout << ",\"batch_final_cpu\":"; numbers(final);
}
}


namespace mk2 {
// shortcut: the ROM identity is hashed once per Program object and buffers (run-ahead resumes the chain every frame);
// a Program whose bytes changed in place would not be hashed again, and no live path writes them.
static bool AuthenticLiveProgram(const Program& program) {
    static thread_local const Program* checked=nullptr;static thread_local const uint8_t* bytes=nullptr;static thread_local const uint8_t* graphics=nullptr;
    if(checked==&program && bytes==program.bytes.data() && graphics==program.graphics.data())return true;
    if(program.bytes.size()!=0x100000 || Sha256(program.bytes)!=kProgramSha256 ||
       program.graphics.size()!=0xc00000 || Sha256(program.graphics)!=kGraphicsSha256)return false;
    checked=&program;bytes=program.bytes.data();graphics=program.graphics.data();return true;
}
static void RequireLiveMemoryState(const Program& program,const BootContinuation& prefix,const DcsBootMachine& machine) {
    const auto& s=prefix.ready;
    if(!IsFirstFightStaticPC(s.next_pc) || s.cycles!=machine.LastMainCycles() || s.board.scanline_events!=machine.ScanlineEvents() ||
       prefix.observe_history || !prefix.bus.empty() || !prefix.points.empty() || machine.dsp.TraceObservationEnabled() || !machine.dsp.trace.empty() ||
       prefix.main_ram.size()!=262144 || s.cmos.size()!=8192 || prefix.video.vram.size()!=524288 || prefix.video.palette_ram.size()!=32768 ||
       !OperatorDswSupported(s.board.dip_switches) || !AuthenticLiveProgram(program))
        throw std::runtime_error("MK2 live requires authentic ROMs and synchronized cooperative memory without full histories");
}
static uint64_t RequireLiveMemory(const Program& program,const BootContinuation& prefix,const DcsBootMachine& machine,uint64_t requested) {
    const auto deadline=live_detail::ChunkDeadline(prefix.ready.cycles,requested);
    RequireLiveMemoryState(program,prefix,machine);
    return deadline;
}
void LiveHostContinuation::ValidateBoundary(const Program& program,const DcsBootMachine& machine) const {
    if(owner_!=&machine || !generation_ || generation_!=machine.LiveHostGeneration())
        throw std::runtime_error("MK2 live boundary requires its unconsumed chain and original machine");
    RequireLiveMemoryState(program,state_,machine);
    const auto& s=state_.ready;
    if(state_.host_origin!=HostOrigin::valid_cmos_live || !machine.ValidCmosLiveHostAdmitted() ||
       !s.board.HostInputProviderActive() || s.board.requested_p1_high_punch || s.board.requested_coin1 || s.board.requested_start1 ||
       s.board.LiveLastInputTick()>s.cycles*16)
        throw std::runtime_error("MK2 live boundary cannot reattach or replay scripted input");
    for(const auto& event:s.board.input_events)if(event[3]!=6 && event[3]!=7)
        throw std::runtime_error("MK2 live history contains a non-host event");
}
LiveHostContinuation RunValidLiveHost(const Program& program,const ValidCmosContinuation& prefix,DcsBootMachine& machine,
    std::function<uint32_t(uint64_t)> provider,uint64_t requested) {
    const auto deadline=RequireLiveMemory(program,prefix,machine,requested);const auto& s=prefix.ready;
    if(!provider || !ValidCmosDrawBoundary(s) || !machine.ValidCmosDrawAdmitted() || !prefix.cpu_trace.empty() ||
       s.board.HostInputProviderActive() || !s.board.input_events.empty() || s.board.requested_p1_high_punch || s.board.requested_coin1 || s.board.requested_start1 ||
       s.board.input0!=0xffff || s.board.input1!=0xffff || s.board.input2!=0xffff)
        throw std::runtime_error("MK2 live starts only at the genuine input-free first draw");
    BootContinuation admitted=prefix;admitted.ready.board.SetHostInputProvider(std::move(provider),s.cycles*16);
    machine.AdmitValidCmosLiveHost(admitted.ready.board,deadline);
    auto result=ExecuteTaskBatch(program,admitted,machine,requested,deadline-s.cycles,0,0,
        false,false,false,false,false,false,false,false,false,true,false,true,true,&admitted);
    result.host_origin=HostOrigin::valid_cmos_live;
    return LiveHostContinuation(std::move(result),machine,machine.LiveHostGeneration());
}
LiveHostContinuation ResumeValidLiveHost(const Program& program,LiveHostContinuation&& prefix,DcsBootMachine& machine,uint64_t requested) {
    if(prefix.owner_!=&machine || !prefix.generation_ || prefix.generation_!=machine.LiveHostGeneration())
        throw std::runtime_error("MK2 live resume requires its unconsumed chain and original machine");
    const auto deadline=RequireLiveMemory(program,prefix.state_,machine,requested);const auto& s=prefix.state_.ready;
    if(prefix.state_.host_origin!=HostOrigin::valid_cmos_live || !machine.ValidCmosLiveHostAdmitted() ||
       !s.board.HostInputProviderActive() || s.board.requested_p1_high_punch || s.board.requested_coin1 || s.board.requested_start1 ||
       s.board.LiveLastInputTick()>s.cycles*16)
        throw std::runtime_error("MK2 live resume cannot reattach or replay scripted input");
    for(const auto& event:s.board.input_events)if(event[3]!=6 && event[3]!=7)
        throw std::runtime_error("MK2 live history contains a non-host event");
    const auto begin=s.cycles;const auto generation=prefix.generation_;
    auto admitted=std::move(prefix.state_);prefix.owner_=nullptr;prefix.generation_=0;
    machine.RenewValidCmosLiveHost(admitted.ready.board,generation,deadline);
    auto result=ExecuteTaskBatch(program,admitted,machine,requested,deadline-begin,0,0,
        false,false,false,false,false,false,false,false,false,true,false,true,true,&admitted);
    result.host_origin=HostOrigin::valid_cmos_live;
    return LiveHostContinuation(std::move(result),machine,machine.LiveHostGeneration());
}
}
