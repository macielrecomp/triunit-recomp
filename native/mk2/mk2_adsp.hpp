#pragma once
#include "mk2_dcs.hpp"
#include <array>
#include <functional>
#include <utility>
#include <memory>

namespace mk2 {
struct BootVideo;
struct BoardBootControls;
// Fixed original boot, queued command processing and fifty SPORT half-buffers through the cooperative batch.
// All other PCs/opcode variants fail before executing an instruction.
class AdspBoot {
    friend class DcsBootMachine;
    friend class DcsEffects;
    AdspBoot(const AdspBoot&) = default;
    bool presentation_only_=false;
    std::function<void(const AdspBoot&)> music_instruction_;
    std::function<void(const AdspBoot&,uint16_t)> music_sport_;
    uint64_t main_horizon_ = 3658091;
    bool live_host_domain_=false;
    bool service_irq_domain_=false;
    uint64_t live_instruction_ceiling_=0;
    std::array<uint64_t,6> discarded_observations_{};
    struct Loop { uint16_t start, end; };
    DcsLink& link_;
    const std::vector<uint32_t> boot_;
    const std::shared_ptr<const std::vector<uint8_t>> rom_storage_;
    const std::vector<uint8_t>& rom_;
    bool rebooted_ = false, reset_pending_ = false;
    uint64_t dac_write_sample_ = 0, dac_last_ticks_ = 0, dac_bufin_ = 0, dac_bufout_ = 0;
    uint64_t instruction_ticks_ = 0;
    uint64_t instruction_count_ = 0;
    uint64_t discarded_accesses_ = 0;
    // The live domain without observation (live_host_domain_, no trace) counts its bus accesses without recording them:
    // nothing reads them there, the drain (DiscardLiveObservations) runs at the same counts and AccessCount is the same.
    uint64_t unrecorded_accesses_ = 0;
    // The idle wait (DcsBootMachine::RunDspUntil). WaitState: everything an instruction can change or read without a
    // counted bus access, an event record or a host hook, and the counts of those. music_calls_: host hook calls (a
    // count of this session, not state).
    uint64_t music_calls_ = 0;
    void WaitState(std::vector<uint64_t>& state) const;
    uint64_t InstructionRoom() const;   // instructions the budget of ExecuteInstruction still allows
    void SkipInstructions(uint64_t count, uint64_t last_ticks) { instruction_count_ += count; cycles += count; instruction_ticks_ = last_ticks; }
    void Record(uint16_t address, uint32_t value, char space) {
        if (live_host_domain_ && !observe_trace_) ++unrecorded_accesses_;
        else accesses.push_back({instruction_pc_, address, value, cycles, space});
    }
    uint16_t instruction_pc_ = 0;
    bool observe_trace_ = true;
    bool dac_enabled_ = false;
    std::function<void(uint64_t,const float*,size_t)> dac_observer_;
    std::vector<int16_t> dac_buffer_ = std::vector<int16_t>(32768);
    std::array<uint16_t, 16> pc_stack_{};
    void LoadBootPage();
    void CheckSoftwareBoot() const;
    uint32_t BootWord(uint16_t address) const;
    uint32_t FirmwareWord(uint16_t address) const;
    void PushPc();
    void ReturnCall();
    void Modify(unsigned index, unsigned modifier);
    uint16_t ReadDag(unsigned index, unsigned modifier);
    void WriteDag(unsigned index, unsigned modifier, uint16_t value);
    uint16_t ReadProgramDag(unsigned index, unsigned modifier);
    void WriteProgramDag(unsigned index, unsigned modifier, uint16_t value);
    uint32_t ReadProgram(uint16_t address) const;
    void WriteProgram(uint16_t address, uint32_t value);
    void LogicAr(uint16_t value);
    void AddAr(uint16_t x, uint16_t y);
    uint16_t Logic(uint16_t value);
    uint16_t Absolute(uint16_t value, bool saturate);
    void DivideSign();
    void DivideQuotient();
    uint16_t Saturated(uint16_t value, bool saturate) const;
    uint16_t Add(uint16_t x, uint16_t y, bool saturate);
    uint16_t Sub(uint16_t x, uint16_t y, bool saturate);
    uint16_t Increment(uint16_t value, bool saturate);
    uint16_t Decrement(uint16_t value, bool saturate);
    uint16_t Negate(uint16_t value, bool saturate);
    void ShiftLogical(uint16_t value, int count, bool high, bool accumulate);
    void ShiftArithmetic(uint16_t value, int count);
    void Exponent(uint16_t value);
    void MacMr(uint16_t x, uint16_t y, bool signed_x, bool signed_y, unsigned action, bool rounded);
    void SaturateMr();
    static int16_t SignedModifier(uint16_t value) {
        return int16_t((value & 0x2000) ? (value | 0xc000) : (value & 0x3fff));
    }
    uint16_t dag_base0_ = 0;
    void UpdateDagBase0() {
        uint16_t mask = 0x3fff;
        for (unsigned n = l[0] ? l[0] - 1 : 0; n; n >>= 1)
            mask = uint16_t((mask << 1) & 0x3fff);
        dag_base0_ = i[0] & mask;
    }
    void WriteIndex0(uint16_t value) { i[0] = value & 0x3fff; UpdateDagBase0(); }
    void WriteLength0(uint16_t value) { l[0] = value & 0x3fff; UpdateDagBase0(); }
    uint16_t DagAddress(unsigned index) const;
    void SportTransmit(uint16_t value);
    void SetShift(uint32_t value);
    void ShiftSi();
    void SetIfc(uint16_t value);
    std::array<Loop, 4> loops_{};
    std::array<uint16_t, 4> counters_{};
    struct CoreBank {
        uint16_t ar = 0, si = 0, af = 0, mx0 = 0, mx1 = 0;
        uint16_t ax0 = 0, ax1 = 0, ay0 = 0, ay1 = 0, my0 = 0, my1 = 0;
        uint16_t mr0 = 0, mr1 = 0, mr2 = 0, mf = 0, se = 0, sb = 0, sr0 = 0, sr1 = 0;
    } alternate_;
    struct InterruptState { uint16_t pc; uint8_t mstat, imask, astat; } interrupt_{};
    void EnterIrq2();
    void EnterIrq1();
    void SelectBank(uint8_t value);
    void ReturnIrq();
    void CheckOpcode(uint32_t expected) const;
    void Advance();
    void SetCounter(uint16_t value);
    void StartLoop(uint16_t end);
    void ReadProgramAr();
    void WriteProgramAr();
    void WriteControl(uint16_t address, uint16_t value);
    void WriteData(uint16_t address, uint16_t value);
    uint16_t ReadData(uint16_t address);
    void CopyArToAf();
    void DelayBranch();
    uint16_t ReadInput();
    void ExecuteInstruction();
    void SetInterruptControl(unsigned reg, uint16_t value);
    void SetMstat();
    // Save-state value fields (mk2_save_state.hpp); link, ROMs and observers stay bound to the live machine.
    template<class A> friend void Fields(A& a, AdspBoot& d) {
        a(d.presentation_only_, d.main_horizon_, d.live_host_domain_, d.service_irq_domain_, d.live_instruction_ceiling_,
          d.discarded_observations_, d.rebooted_, d.reset_pending_, d.dac_write_sample_, d.dac_last_ticks_, d.dac_bufin_,
          d.dac_bufout_, d.instruction_ticks_, d.instruction_count_, d.discarded_accesses_, d.instruction_pc_,
          d.observe_trace_, d.dac_enabled_, d.dac_buffer_, d.pc_stack_, d.dag_base0_, d.loops_, d.counters_, d.alternate_,
          d.interrupt_.pc, d.interrupt_.mstat, d.interrupt_.imask, d.interrupt_.astat,
          d.pc, d.si, d.ar, d.af, d.cntr, d.mx0, d.mx1, d.ax0, d.ax1, d.ay0, d.ay1, d.my0, d.my1,
          d.mr0, d.mr1, d.mr2, d.mf, d.se, d.sb, d.sr0, d.sr1, d.rom_bank, d.ifc, d.boot_reloads,
          d.sport_ireg, d.sport_size, d.sport_incs, d.sport_base, d.sport_enabled, d.reboot_events, d.serial_events,
          d.sport_transfers, d.sport_pcm, d.sport_dma_position, d.sport_dma_positions, d.sport_irq1_events,
          d.sport_irq1_asserted, d.sport_irq1_latched, d.sport_timer_cancelled, d.px, d.mstat, d.imask, d.icntl,
          d.astat, d.sstat, d.pc_sp, d.cntr_sp, d.loop_sp, d.stat_sp, d.cycles, d.i, d.l, d.m, d.pm, d.dm,
          d.external_pm, d.control, d.trace, d.accesses, d.unrecorded_accesses_);
    }
public:
    struct Access { uint16_t pc, address; uint32_t value; uint64_t cycle; char space;
        template<class A> friend void Fields(A& a, Access& x) { a(x.pc, x.address, x.value, x.cycle, x.space); } };
    uint16_t pc = 0, si = 0, ar = 0, af = 0, cntr = 0, mx0 = 0, mx1 = 0;
    uint16_t ax0 = 0, ax1 = 0, ay0 = 0, ay1 = 0, my0 = 0, my1 = 0;
    uint16_t mr0 = 0, mr1 = 0, mr2 = 0, mf = 0, se = 0, sb = 0, sr0 = 0, sr1 = 0;
    uint16_t rom_bank = 0, ifc = 0;
    unsigned boot_reloads = 0;
    uint16_t sport_ireg = 0, sport_size = 0, sport_incs = 0, sport_base = 0;
    bool sport_enabled = false;
    std::vector<std::vector<uint64_t>> reboot_events, serial_events;
    std::vector<std::vector<uint64_t>> sport_transfers;
    std::vector<int16_t> sport_pcm;
    uint64_t sport_dma_position = 0;
    std::vector<uint64_t> sport_dma_positions;
    std::vector<std::vector<uint64_t>> sport_irq1_events;
    bool sport_irq1_asserted = false, sport_irq1_latched = false;
    bool sport_timer_cancelled = false;
    uint8_t px = 0, mstat = 0, imask = 0, icntl = 0, astat = 0, sstat = 0x55;
    unsigned pc_sp = 0, cntr_sp = 0, loop_sp = 0, stat_sp = 0;
    uint64_t cycles = 0;
    std::array<uint16_t, 8> i{}, l{};
    std::array<int16_t, 8> m{};
    std::array<uint32_t, 0x400> pm{};
    std::array<uint16_t, 0x200> dm{};
    std::array<uint32_t, 0x800> external_pm{};
    std::array<uint16_t, 32> control{};
    std::vector<std::vector<uint64_t>> trace;
    bool TraceObservationEnabled() const { return observe_trace_; }
    uint64_t SerialEventCount() const { return discarded_observations_[1]+serial_events.size(); }
    uint64_t SportPacketCount() const { return discarded_observations_[3]+sport_transfers.size(); }
    std::array<uint64_t,8> ObservationCounts() const {
        return {InstructionCount(),AccessCount(),discarded_observations_[0]+reboot_events.size(),SerialEventCount(),
            discarded_observations_[2]+sport_pcm.size(),SportPacketCount(),discarded_observations_[4]+sport_dma_positions.size(),
            discarded_observations_[5]+sport_irq1_events.size()};
    }
    void DiscardLiveObservations();
    uint64_t InstructionCount() const { return instruction_count_; }
    // Prefix owners may omit register observations; execution and device events continue.
    void SetTraceObservation(bool enabled) { observe_trace_ = enabled; }
    // Call only after the caller has consumed the complete observation window.
    void DiscardObservedTrace() {
        std::vector<std::vector<uint64_t>>().swap(trace);
    }
    std::vector<Access> accesses;
    uint64_t AccessCount() const { return discarded_accesses_ + accesses.size() + unrecorded_accesses_; }
    // Presentation consumes completed observation windows, preserving absolute SPORT positions.
    void DiscardObservedAccesses() {
        discarded_accesses_ += accesses.size() + unrecorded_accesses_;unrecorded_accesses_ = 0;
        std::vector<Access>().swap(accesses);
    }
    AdspBoot(const std::vector<uint32_t>& boot, DcsLink& link, const std::vector<uint8_t>& rom = {});
    AdspBoot& operator=(const AdspBoot&) = delete;
    void ReloadBoard();
    void ResetCore();
    void Step();
    bool ApplyPendingReset();
    uint16_t ReadControl(uint16_t address) const;
    // Ordered like tools/mk2/adsp_reference.py; values include all data registers,
    // DAG registers and visible control/stack state, never imported from MAME.
    std::vector<uint64_t> Registers() const;
    std::vector<uint64_t> DeviceState() const;
    std::array<uint32_t, 27> ActiveStacks() const;
    // Raw execution state for partition comparisons; memories/control/ring are separate.
    // Excludes observation storage, callbacks and per-grant authorization ceilings.
    std::vector<uint64_t> ExecutionState() const;
    void SportTransfer(uint64_t ticks);
    void ClearSportIrq1(uint64_t ticks);
    void DacUpdate(uint64_t ticks);
    // Canonical 31250 Hz mono DAC timeline, with the cold silent prefix
    // reprojected from 48000 Hz. Index is absolute from machine time zero;
    // float32 /32767, device/route gain 1. Pointer lives only during callback.
    void SetDacObserver(std::function<void(uint64_t,const float*,size_t)> observer) {
        dac_observer_=std::move(observer);
    }
    bool DacEnabled() const { return dac_enabled_; }
    std::vector<uint32_t> DacBuffer(unsigned count) const;
    void SetInstructionTicks(uint64_t ticks) { instruction_ticks_ = ticks; }
};

class DcsBootMachine {
    bool valid_cmos_host_=false;
    unsigned operator_startup_profile_=0; // Guard selection only; guest code reads the unchanged CMOS.
    uint64_t operator_startup_horizon_=0,operator_draw_horizon_=0,operator_dma_start_=0;
    uint64_t live_generation_=0;
    std::function<void(const BoardBootControls&,const BootVideo&)> scanline_observer_;
    uint64_t main_horizon_ = 3658091;
    bool live_host_domain_=false;
    uint64_t dsp_ticks_ = 0, target_ticks_ = 0, main_limit_ = 0, last_main_ = 0;
    uint64_t boost_expiry_ticks_ = 0, sport_deadline_ticks_ = 0, sport_irq1_clear_ticks_ = 0;
    uint64_t next_scanline_ticks_ = 94875;
    uint64_t next_sound_ticks_ = 2000000;
    unsigned scanline_events_ = 0;
    bool sliced_ = false;
    uint64_t dma_deadline_subticks_ = 0;
    unsigned dma_completions_ = 0;
    void AdmitWaitAfterC8(struct BoardBootControls&, uint64_t end_cycle);
    void RunDspUntil(uint64_t ticks);
    std::vector<uint64_t> wait_state_, wait_now_;   // RunDspUntil's two arrivals (scratch, not state)
    uint64_t NextTimerTicks() const {
        uint64_t next = std::min(next_scanline_ticks_, next_sound_ticks_);
        if (sport_deadline_ticks_) next = std::min(next, sport_deadline_ticks_);
        if (dma_deadline_subticks_) next = std::min(next, dma_deadline_subticks_ / 10);
        return sport_irq1_clear_ticks_ ? std::min(next, sport_irq1_clear_ticks_) : next;
    }
    // Save-state value fields; the scanline observer stays with the live session.
    template<class A> friend void Fields(A& a, DcsBootMachine& m) {
        a(m.valid_cmos_host_, m.operator_startup_profile_, m.operator_startup_horizon_, m.operator_draw_horizon_,
          m.operator_dma_start_, m.live_generation_, m.main_horizon_, m.live_host_domain_, m.dsp_ticks_, m.target_ticks_,
          m.main_limit_, m.last_main_, m.boost_expiry_ticks_, m.sport_deadline_ticks_, m.sport_irq1_clear_ticks_,
          m.next_scanline_ticks_, m.next_sound_ticks_, m.scanline_events_, m.sliced_, m.dma_deadline_subticks_,
          m.dma_completions_, m.link, m.dsp);
    }
public:
    void SetScanlineObserver(std::function<void(const BoardBootControls&,const BootVideo&)> observer) {
        scanline_observer_=std::move(observer);
    }
    void AdvanceVideo(BoardBootControls&, const BootVideo&) const;
    void AdmitValidCmosDelay(BoardBootControls&);
    void AdmitValidCmosStartup(BoardBootControls&,unsigned profile=0);
    void AdmitValidCmosDraw(BoardBootControls&);
    bool ValidCmosDrawAdmitted() const { return operator_draw_horizon_ && main_horizon_==operator_draw_horizon_; }
    bool OperatorFreePlayProfile() const { return operator_startup_profile_==1; }
    void AdmitValidCmosHost(BoardBootControls&);
    bool ValidCmosHostAdmitted() const {
        return !live_host_domain_ && valid_cmos_host_ && main_horizon_==150023965 && dsp.main_horizon_==150023965;
    }
    void AdmitValidCmosLiveHost(BoardBootControls&,uint64_t deadline);
    void RenewValidCmosLiveHost(BoardBootControls&,uint64_t generation,uint64_t deadline);
    bool ValidCmosLiveHostAdmitted() const { return live_host_domain_ && dsp.live_host_domain_ && main_horizon_==dsp.main_horizon_; }
    std::array<uint64_t,13> SchedulerClocks() const { return {dsp_ticks_,target_ticks_,main_limit_,last_main_,boost_expiry_ticks_,sport_deadline_ticks_,sport_irq1_clear_ticks_,next_scanline_ticks_,next_sound_ticks_,scanline_events_,uint64_t(sliced_),dma_deadline_subticks_,dma_completions_}; }
    uint64_t LiveHostGeneration() const { return live_generation_; }
    uint64_t LiveHostDeadline() const { if(!ValidCmosLiveHostAdmitted())throw std::runtime_error("MK2 live deadline requires its own domain");return main_horizon_; }
    bool ValidCmosStartupAdmitted() const { return operator_startup_horizon_ && main_horizon_==operator_startup_horizon_; }
    bool HasFullDcsRoms() const { return dsp.rom_.size()==6*0x80000; }
    bool TestSwitchAdmitted() const { return dsp.service_irq_domain_; }
    void RenewTestSwitch(BoardBootControls&);
    DcsLink link;
    AdspBoot dsp;
    explicit DcsBootMachine(const std::vector<uint32_t>& boot, const std::vector<uint8_t>& rom = {})
        : dsp(boot, link, rom) {}
    // 100 MHz half-ticks: TMS internal cycle = 16, ADSP cycle = 10.
    // Finite cold boot through the larger cooperative batch (main cycles 0..3658091). Call after
    // each main instruction; device writes are delivered after catch-up.
    // input_line_event closes the slice for DMA INT1 / TMS HALT clears, even repeated clears.
    void MainInstruction(uint64_t cycles, bool host_write = false, uint16_t value = 0,
                         bool input_line_event = false);
    unsigned ScanlineEvents() const { return scanline_events_; }
    uint64_t LastMainCycles() const { return last_main_; }
    uint64_t SportDeadlineTicks() const { return sport_deadline_ticks_; }
    uint64_t DmaDeadlineSubticks() const { return dma_deadline_subticks_; }
    unsigned DmaCompletions() const { return dma_completions_; }
    bool CoinPairAdmitted() const { return live_host_domain_ || main_horizon_ == 5077311 || main_horizon_ == 6087360 || main_horizon_ == 7005800 || main_horizon_ == 22081952 || main_horizon_ == 23108559 || main_horizon_ == 24004881 || main_horizon_ == 30060282 || main_horizon_ == 100091930 || main_horizon_ == 101005964 || main_horizon_ == 104090574 || main_horizon_ == 110030700 || main_horizon_ == 118028828 || main_horizon_ == 126026668 || main_horizon_ == 134024907 || main_horizon_ == 150023965; }
    bool FirstFightAdmitted() const { return live_host_domain_ || main_horizon_ == 100091930 || main_horizon_ == 101005964 || main_horizon_ == 104090574 || main_horizon_ == 110030700 || main_horizon_ == 118028828 || main_horizon_ == 126026668 || main_horizon_ == 134024907 || main_horizon_ == 150023965; }
    void AdmitCoinPair(struct BoardBootControls& board);
    void AdmitStartWait(struct BoardBootControls& board);
    void AdmitNextWait(struct BoardBootControls& board);
    void AdmitPreStartWait(struct BoardBootControls& board);
    void AdmitStartInput(struct BoardBootControls& board);
    void AdmitStartRelease(struct BoardBootControls& board);
    void AdmitSelection(struct BoardBootControls& board);
    void AdmitFirstFight(struct BoardBootControls& board);
    void ScheduleDma(unsigned pixels, unsigned instruction_cycle_offset = 0) {
        if (TestSwitchAdmitted()) {
            if (instruction_cycle_offset!=22 || pixels>1023u*1023u)
                throw std::runtime_error("MK2 Test Switch DMA outside qualified MMTM geometry");
            dma_deadline_subticks_=(last_main_+instruction_cycle_offset)*160+uint64_t(pixels)*41;
            return;
        }
        if(ValidCmosDrawAdmitted()) {
            if(last_main_!=operator_dma_start_ || pixels!=4864 || dma_deadline_subticks_ || dma_completions_)
                throw std::runtime_error("MK2 valid-CMOS DMA outside original first sprite");
            dma_deadline_subticks_=(last_main_+instruction_cycle_offset)*160+uint64_t(pixels)*41;
            return;
        }

        if (CoinPairAdmitted()) {
            if (pixels > 1023u*1023)
                throw std::runtime_error("MK2 DMA dimensions exceed physical registers");
            // Original timer.adjust replaces a pending timer, including zero-pixel starts.
            dma_deadline_subticks_ = (last_main_+instruction_cycle_offset)*160 + uint64_t(pixels)*41;
            return;
        }
        constexpr uint64_t starts[] = {1259252, 1373046, 1487299, 1601542, 1715788,
                                      1830032, 1944279, 2058520, 2172772,
                                      2287008, 2401264, 2515498, 2629751,
                                      2743990, 2858246, 2972482, 3086732, 3200971, 3314873,
                                      3429040, 3543346, 3657534};
        if (dma_completions_ >= std::size(starts) || last_main_ != starts[dma_completions_] ||
            pixels != 4864 || dma_deadline_subticks_)
            throw std::runtime_error("MK2 DMA timer outside qualified loaded sprite");
        dma_deadline_subticks_ = (last_main_+instruction_cycle_offset) * 160 + uint64_t(pixels) * 41;
    }
    unsigned MainBudget(uint64_t cycles) const {
        const auto deadline = NextTimerTicks() / 16;
        if (cycles != last_main_ || sliced_ || deadline <= cycles)
            throw std::runtime_error("MK2 graphics budget outside synchronized unboosted scope");
        return unsigned(deadline - cycles);
    }
};
}
