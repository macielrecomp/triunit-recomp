#pragma once
#include "mk3_profile.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mk3 {
// DCS Audio 8K (REV_DCS1, Wolf-Unit) ADSP-2105 at 10 MHz executing the U2 bank-0 boot page and the measured bank-1
// program by fixed translation (tools/mk3/generate_adsp.py). Contracts: pinned MAME adsp2100.cpp/2100ops.hxx (core),
// dcs.cpp (8K maps, latches, reset/boot, SPORT1 autobuffer -> DAC), diexec.cpp + schedule.cpp (input-line
// synchronization, timers). PCs or encodings outside the generated blocks, unmapped addresses and unmodeled devices
// reject before executing.
struct AdspAccess {
    unsigned boot;       // core reset index (0 = power-on), the debugger's totalcycles restarts at each
    uint64_t cycle;      // totalcycles of the accessing instruction
    uint16_t pc;
    char kind;           // 'r'/'w' data space, 'q'/'p' program-space data read/write (fetches excluded),
                         // 'd' data-space read by the autobuffer transfer (dcs_irq): pc/cycle are those of the last
                         // instruction executed before it (what the debugger shows inside a timer callback)
    uint16_t address;
    uint32_t value;
};
// Host (main CPU) actions on the DCS. write_ns: TMS34010 time of the bus access (cycle * 160 ns); end_ns: where the
// timeslice of that access ends for the ADSP: the main-CPU local time after the writing instruction (its
// synchronize() aborts the slice), or the scheduler target when that comes first (schedule.cpp timeslice(): under
// the 500 ns quantum that dcs_delayed_data_w adds for 5 us, a writing instruction can straddle the target). The
// slice started in [slice_min_ns, slice_max_ns]; an ADSP self-synchronization inside that range is ambiguous and
// rejects.
// DataRead (dcs.cpp data_r, auto-ack): no synchronization; the output latch becomes empty at write_ns and the value
// returned is recorded in host_reads. The ADSP runs to slice_min_ns first; an ADSP output-latch write in
// (slice_min_ns, slice_max_ns] rejects. end_ns and value are unused. The 4444 eaten main-CPU cycles on an empty
// latch belong to the caller.
struct HostEvent {
    enum Kind { ResetAssert, ResetRelease, DataWrite, DataRead } kind;
    uint64_t write_ns, end_ns;
    uint16_t value = 0;
    uint64_t slice_min_ns = 0, slice_max_ns = 0;
};
// One dcs_irq transfer to the DMADAC (dmadac_transfer, 1 channel): `count` signed 16-bit samples at pcm[first...],
// read from data memory at address, address + step, ... The sample rate is 1e9 / sample_ns (dmadac_set_frequency);
// MAME's resampling to the host rate is outside this model.
struct DacBlock {
    uint64_t ns;         // timer expiry = ADSP local time
    unsigned boot;
    uint64_t cycle;      // instructions executed in this boot before the transfer
    uint16_t address;
    int32_t step;
    uint32_t first, count, sample_ns;
    uint32_t arm;        // m_reg_timer settings (sound_tx_callback -> recompute_sample_rate) made before this transfer
};

class Dcs8k {
public:
    explicit Dcs8k(const std::vector<uint8_t>& u2);   // authenticated U2 (1 MiB); power-on + machine_reset at t = 0
    // Sound-data ROMs U3-U5 (authenticated, 1 MiB each) for the data banks 0x100-0x3ff (ROM_LOAD16_BYTE at 0x200000,
    // 0x400000, 0x600000 of the 8 MiB dcs region); without them those banks reject as outside U2.
    void AttachSoundData(const std::filesystem::path& rom_directory);
    // Applies the host events in order and runs to horizon_ns; stop(*this) is checked before every instruction.
    void Run(const std::vector<HostEvent>& events, uint64_t horizon_ns, const std::function<bool(const Dcs8k&)>& stop);
    static const std::vector<std::string>& FieldNames();   // debugger symbol names, in Fields() order
    std::vector<uint64_t> Fields() const;
    // m_latch_control after every ADSP/host event before ns, in plain time order. The main CPU runs first in a
    // timeslice, so a status read really sees only the ADSP changes of earlier slices (tools/mk3/test_adsp_bank1.py).
    uint16_t LatchAt(uint64_t ns) const;
    uint16_t DebugData(uint16_t address) const;              // side-effect-free data-space view (mapped ranges only)
    uint32_t DebugProgram(uint16_t address) const;

    std::function<bool(const Dcs8k&)> observe;               // records Fields() before an instruction when true
    std::function<bool(const Dcs8k&, const AdspAccess&)> observe_access;   // null records every access
    std::vector<std::vector<uint64_t>> rows;                 // {boot, Fields()...}
    std::vector<AdspAccess> accesses;
    std::vector<std::pair<uint64_t, uint16_t>> latch_changes{{0, 0x0c00}};
    std::vector<std::array<uint64_t, 3>> core_resets;        // {local_ns, boot, instructions executed before} per core reset
    std::vector<DacBlock> dac_blocks;
    std::vector<int16_t> pcm;                                // samples of every DacBlock, in delivery order
    std::vector<std::pair<uint64_t, uint16_t>> host_reads;   // {write_ns, m_output_data returned} per DataRead
    uint64_t local_ns = 0, totalcycles = 0, instructions = 0;
    // Earliest scheduler timer of the DCS (dcs.cpp m_reg_timer autobuffer transfer and m_internal_timer, diexec.cpp
    // IRQ1 pulse end), ns; ~0 when none is armed. In MAME each one ends the running timeslice of every CPU.
    uint64_t NextTimerNs() const { return std::min({dac_next_ns_, irq1_clear_ns_, int_next_ns_}); }
    // Self-synchronizing instructions executed so far (input/output latch access at DM 0x3400-0x3403, SYSCONTROL
    // reboot, an internal-timer arming that becomes the scheduler's first timer): each one ends the ADSP timeslice
    // (machine().scheduler().synchronize / pulse_input_line RESET / emu_timer::adjust -> abort_timeslice).
    uint64_t synchronizations = 0;
    // The host scheduler's earliest timer other than the DCS ones, ns rounded up (mk3::Board sets it before each ADSP
    // run). An internal-timer arming earlier than every timer becomes the list head and aborts the ADSP timeslice
    // (schedule.cpp emu_timer::adjust). 0 = unknown: such an arming rejects.
    uint64_t host_next_ns = 0;
    // When true, Run returns right after a self-synchronizing instruction (its timeslice end), before the next
    // slice's check_irqs; a caller that owns the scheduler (mk3::Board) then resumes it with another Run.
    bool return_at_sync = false;
    // Execution profile (mk3_profile.hpp): every swept PM word is a block; the measured (pc, opcode) pairs are the
    // verified ones. Strict: any other block rejects as the measured-only build did. Game: it runs, and `first` (when
    // set) records AdspBlock {id = page << 16 | pc, from = opcode} and AdspEdge {id = pc, from = previous pc} on entry
    // from a verified block; cycle = ADSP cycles since power-on. Untranslated forms and PCs reject in both.
    Profile profile = Profile::Strict;
    const RouteVerified* routes = nullptr;   // Strict: route-verified AdspBlock/AdspEdge keys also run (mk3_profile.hpp)
    FirstExecution* first = nullptr;
    unsigned boot = 0;
    bool suspended = false;
    uint16_t pc = 0, latch_control = 0x0c00, input_data = 0, sounddata_bank = 0, output_data = 0;

private:
    friend struct AdspFormTest;   // native/tests/mk3_adsp_tests.cpp: per-form checks against a transcription of 2100ops.hxx
    friend struct AdspTimerTest;  // tools/mk3/test_adsp_timer.py: internal timer against a transcription of dcs.cpp
    struct Core { uint16_t ax0, ax1, ay0, ay1, ar, af, mx0, mx1, my0, my1, mr0, mr1, mr2, mf, si, se, sb, sr0, sr1; };
    Core core_{}, alt_{};
    std::array<uint32_t, 8> i_{}, l_{}, base_{}, lmask_{};
    std::array<int32_t, 8> m_{};
    std::array<uint32_t, 16> pc_stack_{};
    std::array<uint32_t, 4> cntr_stack_{}, loop_stack_{};
    std::array<std::array<uint16_t, 3>, 4> stat_stack_{};
    uint32_t cntr_ = 0, loop_ = 0xffff, loop_condition_ = 0, pc_sp_ = 0, cntr_sp_ = 0, loop_sp_ = 0, stat_sp_ = 0;
    uint16_t ppc_ = 0, astat_ = 0, astat_clear_ = 0, mstat_ = 0, mstat_prev_ = 0, sstat_ = 0x55, imask_ = 0, icntl_ = 0;
    uint8_t px_ = 0, flagout_ = 0, flagin_ = 0, fl0_ = 0, fl1_ = 0, fl2_ = 0;
    std::array<uint8_t, 10> irq_state_{}, irq_latch_{};
    std::array<uint32_t, 0x400> pm_{};
    std::array<uint32_t, 0x1800> external_pm_{};   // PM 0x0800-0x1fff = DM 0x0800-0x1fff (bits 23..8)
    std::array<uint16_t, 0x800> dm_{};
    std::array<uint16_t, 0x200> internal_dm_{};
    std::array<uint16_t, 32> control_{};
    const std::vector<uint8_t>& u2_;
    std::vector<uint8_t> sound_data_;               // U3, U4, U5: banks 0x100-0x3ff
    const uint8_t* DataByte(uint16_t a) const;   // data bank window byte, nullptr outside the attached ROMs
    bool sync_ = false, reboot_ = false, eat_ = false, unverified_ = false, last_unverified_ = false;
    uint16_t last_pc_ = 0;
    unsigned page_ = 0;                             // sound-data bank whose boot page dcs_boot loaded (code identity)
    // dcs.cpp m_reg_timer / autobuffer state and the IRQ1 pulse-end timer (diexec.cpp pulse_input_line)
    static constexpr uint64_t kNever = ~0ull;
    uint64_t dac_next_ns_ = kNever, dac_period_ns_ = 0, irq1_clear_ns_ = kNever, read_slice_min_ns_ = 0, read_slice_max_ns_ = 0;
    uint32_t dac_ireg_ = 0, dac_size_ = 0, dac_sample_ns_ = 0;
    int32_t dac_incs_ = 0;
    uint32_t dac_arms_ = 0;
    bool dac_enabled_ = false;
    // dcs.cpp internal timer (ADSP-2105 TSCALE/TCOUNT/TPERIOD at DM 0x3ffb-0x3ffd, enabled by MSTAT bit 5 through
    // m_timer_fired_cb); member types as dcs.h. int_next_ns_: m_internal_timer expiry.
    uint64_t timer_start_cycles_ = 0, int_next_ns_ = kNever;
    uint32_t timer_start_count_ = 0, timer_scale_ = 0, timer_period_ = 0, timers_fired_ = 0;
    uint8_t timer_enable_ = 0;
    bool timer_ignore_ = false;
    static constexpr uint32_t kTimerPath = 0x21050000;   // DevicePath ids: | 1 enable change, 2 write, 3 read, 4 expiry

    // Save states (mk3_save_state.hpp): value fields only. U2/U3-U5 ROM data, observers, logs and the profile stay with
    // the live Dcs8k.
    template<class A> friend void Fields(A& a, Dcs8k& d) {
        a(d.rows, d.accesses, d.latch_changes, d.core_resets, d.dac_blocks, d.pcm, d.host_reads, d.local_ns, d.totalcycles,
          d.instructions, d.synchronizations, d.host_next_ns, d.return_at_sync, d.boot, d.suspended, d.pc, d.latch_control,
          d.input_data, d.sounddata_bank, d.output_data, d.core_, d.alt_, d.i_, d.l_, d.base_, d.lmask_, d.m_, d.pc_stack_,
          d.cntr_stack_, d.loop_stack_, d.stat_stack_, d.cntr_, d.loop_, d.loop_condition_, d.pc_sp_, d.cntr_sp_, d.loop_sp_,
          d.stat_sp_, d.ppc_, d.astat_, d.astat_clear_, d.mstat_, d.mstat_prev_, d.sstat_, d.imask_, d.icntl_, d.px_,
          d.flagout_, d.flagin_, d.fl0_, d.fl1_, d.fl2_, d.irq_state_, d.irq_latch_, d.pm_, d.external_pm_, d.dm_,
          d.internal_dm_, d.control_, d.sync_, d.reboot_, d.eat_, d.unverified_, d.last_unverified_, d.last_pc_, d.page_,
          d.dac_next_ns_, d.dac_period_ns_, d.irq1_clear_ns_, d.read_slice_min_ns_, d.read_slice_max_ns_, d.dac_ireg_,
          d.dac_size_, d.dac_sample_ns_, d.dac_incs_, d.dac_arms_, d.dac_enabled_, d.timer_start_cycles_, d.int_next_ns_,
          d.timer_start_count_, d.timer_scale_, d.timer_period_, d.timers_fired_, d.timer_enable_, d.timer_ignore_);
        if constexpr (A::reading) { d.wait_turn_ = 0; d.wait_clean_ = false; }   // a loaded state: the wait is found again
    }
    [[noreturn]] void Reject(const std::string& what) const;
    bool Step();                       // one instruction; true when it synchronized (aborts the ADSP timeslice)
    // The idle wait (Run). WaitState: every value field but the clocks, a vector as its length, the memories left out;
    // access_count_ (bus accesses, Log) stands for them. wait_pc_, wait_turn_, wait_state_: a loop found to change
    // nothing, its state on arrival and its length in instructions (0: none). wait_clean_: nothing from outside (a
    // timer, a host event, a loaded state) has touched the DSP since it last arrived in that state, so the next
    // arrival is in it too. Scratch of this session, not state.
    uint64_t access_count_ = 0, wait_turn_ = 0;
    bool wait_clean_ = false;
    uint16_t wait_pc_ = 0xffff;
    std::vector<uint8_t> wait_state_, wait_now_;
    void WaitState(std::vector<uint8_t>& state) const;
    void CoreReset();                  // device_t::reset(): interface_pre_reset + adsp21xx device_reset
    void DcsReset(uint64_t ns);        // dcs_audio_device::dcs_reset (REV_DCS1)
    void Boot();                       // dcs_boot: load_boot_data from the current sound-data bank
    void SetLatch(uint16_t value, uint64_t ns);
    void Log(char kind, uint16_t pc, uint64_t cycle, uint16_t address, uint32_t value);
    void SetIrq(unsigned line, bool state);   // adsp21xx_device::execute_set_input
    void CheckIrqs();
    void SportTx1();                   // dcs.cpp sound_tx_callback(1) + recompute_sample_rate
    void StopDac();                    // dmadac_enable(0) + m_reg_timer->reset()
    void DacTransfer();                // dcs.cpp dcs_irq
    bool FireTimers();                 // true when a timer expired (the next executed slice starts with check_irqs)
    // dcs.cpp timer management; Strict rejects each path unless route-verified, Game notes a DevicePath
    void TimerPath(uint32_t n, uint32_t from);
    void TimerEnable(bool state);      // timer_enable_callback
    void UpdateTimerCount();           // update_timer_count
    void ResetTimer();                 // reset_timer
    void InternalTimer();              // internal_timer_callback
    uint32_t Opcode() const;
    void Fetch(uint32_t expected);
    bool Condition(unsigned c);
    void UpdateMstat();
    void UpdateI(unsigned n) { base_[n] = i_[n] & lmask_[n]; }
    void UpdateL(unsigned n);
    void PcPush(); void PcPop(); uint32_t PcPopVal(); void CntrPush(); void CntrPop();
    void LoopPush(uint32_t value); void LoopPop(); void StatPush(); void StatPop();
    void WriteReg0(unsigned r, int32_t v); void WriteReg1(unsigned r, int32_t v);
    void WriteReg2(unsigned r, int32_t v); void WriteReg3(unsigned r, int32_t v);
    int32_t ReadReg0(unsigned r) const; int32_t ReadReg1(unsigned r) const;
    int32_t ReadReg2(unsigned r) const; int32_t ReadReg3(unsigned r);
    void Modify(unsigned ireg, unsigned mreg);
    uint16_t DataRead(uint16_t address);
    void DataWrite(uint16_t address, uint16_t value);
    uint32_t ProgramRead(uint16_t address);
    void ProgramWrite(uint16_t address, uint32_t value);
    uint16_t DataReadDag1(uint32_t op); void DataWriteDag1(uint32_t op, int32_t v);
    uint16_t DataReadDag2(uint32_t op); void DataWriteDag2(uint32_t op, int32_t v);
    uint16_t PgmReadDag2(uint32_t op); void PgmWriteDag2(uint32_t op, int32_t v);
    void Alu(bool af, unsigned fn, unsigned x, unsigned y);      // alu_op_ar / alu_op_af
    void Mac(bool mf, unsigned fn, unsigned x, unsigned y);      // mac_op_mr / mac_op_mf
    void Shift(unsigned fn, unsigned x, int sc, bool imm = false);   // shift_op / shift_op_imm (imm: no EXP/EXPADJ)
    void Divs(unsigned x, unsigned y); void Divq(unsigned x);
    void FlagOut(uint32_t op);
    void EatSlice() { eat_ = true; }   // IDLE / jump to self: m_icount = 0 (adsp2100.cpp execute_run)
    void Unverified(uint32_t op, bool measured_pc);   // a swept block without a measured pair (profile rule above)
    [[noreturn]] void Untranslated() const;
    void SatMr();
};
}
