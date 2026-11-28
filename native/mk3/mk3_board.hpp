#pragma once
#include "mk3_cpu.hpp"
#include "mk3_adsp.hpp"
#include "mk3_scanout.hpp"
#include <algorithm>
#include <deque>
#include <functional>

namespace mk3 {
// Wolf-Unit board co-simulation: the generated TMS34010 slices and the DCS 8K ADSP-2105 under the pinned MAME
// scheduler contract (schedule.cpp device_scheduler::timeslice/execute_timers/add_quantum, diexec.cpp
// abort_timeslice). Per timeslice the main CPU runs first to the target (next timer or base + quantum), ending at
// the instruction that reaches it or that made a synchronizing write; the ADSP then runs whole cycles to the
// (possibly lowered) target, ending early when it synchronizes itself; then the due timers fire. A TMS read of the
// DCS latch therefore sees the ADSP as of the end of the previous timeslice, and every DCS host event comes from
// the native TMS writes and sound-port reads at their native cycles. A read (dcs data_r, auto-ack) does not
// synchronize: it empties the output latch at once, inside the current timeslice.
// Timers modeled (all others in the pinned sources stay unarmed in this scope): synchronize() callbacks (DCS host
// writes; TMS input-line changes from HSTCTLH and DMA_COMMAND writes, diexec.cpp set_state_synced), the
// TMS34010 scanline and DMA-done timers of the main CPU's VideoTiming (mk3_video.hpp: scanline_callback over the fixed
// 506x289 8 MHz raster, DPYINT, VCOUNT, DPYADR), the screen's VBLANK begin/end and scanline-0 timers and the 50 Hz
// sound update (sound.cpp). A screen reconfiguration rejects. Every TMS timeslice starts with check_interrupt
// (tms34010.cpp execute_run): a pending enabled interrupt is taken before the first instruction.
class Board {
public:
    // attoseconds since power-on; 128 bits as VideoTiming::Atto (64 bits wrap at 18.446744 s, attotime keeps seconds apart)
    using As = unsigned __int128;
    static constexpr As kNs = 1000000000ull, kSecond = kNs * kNs, kNever = ~As(0);
    static constexpr As kTmsCycle = 160 * kNs, kAdspCycle = 100 * kNs;       // 50 MHz / 8; 10 MHz

    ResetTrace s;
    Ops o;
    Dcs8k dcs;
    As base = 0;                 // device_scheduler::m_basetime
    std::string dcs_frontier;    // the ADSP's rejection once it stops at its translation frontier
    struct Applied { HostEvent::Kind kind; uint64_t write_cycle, end_cycle; uint16_t value; uint64_t adsp_ns;
        template<class A> friend void Fields(A& a, Applied& e) { a(e.kind, e.write_cycle, e.end_cycle, e.value, e.adsp_ns); } };
    std::vector<Applied> host_events;
    std::function<bool(const Board&)> stop;       // checked before every TMS instruction; Run resumes after it (below)
    std::function<bool(const Dcs8k&)> dcs_stop;   // checked before every ADSP instruction
    As boost_quantum = 500 * kNs;                 // dcs_delayed_data_w add_quantum(500 ns, 5 us)
    As data_skew = 0;                             // negative control: extra main-CPU time after a sound-port write
    bool input_syncs = true;                      // negative control: false drops the HSTCTLH / DMA_COMMAND synchronize()
    // Execution profile of the Board's own paths (mk3_profile.hpp); SetProfile also hands it to the ADSP and the DMA.
    Profile profile = Profile::Strict;
    FirstExecution* first = nullptr;
    // routes: the route-verified keys Strict also accepts (mk3_profile.hpp, e.g. &RouteVerifiedSet()); null = lot only.
    const RouteVerified* routes = nullptr;
    void SetProfile(Profile p, FirstExecution* log, const RouteVerified* route_keys = nullptr) {
        profile = dcs.profile = o.blitter.profile = p;
        first = dcs.first = o.blitter.first = log;
        routes = dcs.routes = o.blitter.routes = o.routes = route_keys;
        o.profile = p; o.first = log;
    }
    // Optional screen bitmap (mk3_scanout.hpp): update_partial from the scanline timer, reset at the scanline-0 timer,
    // Present() with the palette RAM at VBLANK begin. Observation only; nothing feeds back into the emulation.
    Scanout* scanout = nullptr;
    void AttachScanout(Scanout& out) {
        scanout = &out;
        o.video.update_partial = [this](int vcount) { scanout->Line(vcount, o.video.io, o.vram); };
    }
    // IrqPath ids recorded by the Board (from = 0): the vector of a pending enabled interrupt left for the next
    // check_interrupt, at an instruction boundary inside a timeslice.

    // graphics: the loaded "video" region bytes (DMA source), authenticated by the caller; the Board keeps a reference.
    Board(const Program& program, const std::vector<uint8_t>& cmos_image, const std::vector<uint8_t>& u2,
          const std::vector<uint8_t>& graphics = kNoGraphics)
        : s(PowerOn(program)), o(program, s, CmosWords(cmos_image), graphics), dcs(u2) {
        o.cosim = true;   // from the machine reset every instruction is generated code
    }
    uint64_t Cycles() const { return s.cycles; }   // TMS cycles executed so far

    // Exploration snapshot (K4 decision 3: search only, never in an acceptance or a count): the whole machine and
    // scheduler state, timeslice in progress included. Observation attachments (scanout, VRAM/DMA observers) and the stop
    // callbacks are not copied; `first` keeps pointing at the source's log until SetProfile rebinds it.
    Board(const Board& x)
        : s(x.s), o(x.o, s), dcs(x.dcs), base(x.base), dcs_frontier(x.dcs_frontier), host_events(x.host_events),
          boost_quantum(x.boost_quantum), data_skew(x.data_skew), input_syncs(x.input_syncs), profile(x.profile), first(x.first),
          routes(x.routes), sync_(x.sync_), adsp_sync_(x.adsp_sync_), boost_(x.boost_),
          in_tms_(x.in_tms_), boost_expire_(x.boost_expire_), dcs_cut_(x.dcs_cut_), target_(x.target_), limit_(x.limit_),
          queued_(x.queued_), vblank_begin_(x.vblank_begin_), vblank_end_(x.vblank_end_), line0_(x.line0_), sound_(x.sound_) {
        o.vram_observer = nullptr; o.blitter.observer = nullptr; o.video.update_partial = nullptr;
    }
    Board& operator=(const Board&) = delete;
    // Save states (mk3_save_state.hpp): the machine and scheduler value fields. Observers, stop callbacks, ROM references
    // and the configuration (profile, logs, negative controls) stay with the live Board.
    template<class A> friend void Fields(A& a, Board& b) {
        a(b.s, b.o, b.dcs, b.base, b.dcs_frontier, b.host_events, b.sync_, b.adsp_sync_, b.boost_, b.in_tms_, b.boost_expire_,
          b.dcs_cut_, b.target_, b.limit_, b.queued_, b.vblank_begin_, b.vblank_end_, b.line0_, b.sound_);
    }
    bool StoppedInTimeslice() const { return in_tms_; }   // Run returned from `stop`: the next Run resumes here
    size_t PendingSyncs() const { return sync_.size(); }   // snapshot diagnostics: synchronize() callbacks not yet applied
    bool Boosted() const { return boost_; }                // the 500 ns quantum of a DCS data write is in force

    // Resumable: after `stop` ended it, the next Run continues the same timeslice at that instruction boundary (checking
    // `stop` there again). A `dcs_stop` end is not resumable.
    void Run() {
        for (;;) {
            if (!in_tms_) {
                if (FireTimers() && boost_ && base >= boost_expire_) boost_ = false;   // timeslice(): expired quantum
                target_ = std::min(base + (boost_ ? boost_quantum : kSecond / 60), NextTimer());
                // The DCS timers (autobuffer transfer, internal timer, IRQ1 pulse end) are scheduler timers too: they end the TMS timeslice
                // (MAME execute_timers); the ADSP applies them itself when it runs to that time, nothing to do on the host side.
                if (const uint64_t t = dcs_frontier.empty() ? dcs.NextTimerNs() : ~0ull; t != ~0ull && t * kNs > base && t * kNs < target_) {
                    target_ = t * kNs; dcs_cut_ = target_;
                }
                const As tms = Cycles() * kTmsCycle;
                if (target_ >= tms + kTmsCycle) { queued_ = sync_.size(); StartTms((target_ - tms) / kTmsCycle); in_tms_ = true; }
            }
            if (in_tms_) {
                if (!RunTms()) return;
                in_tms_ = false;
                const bool data = sync_.size() != queued_ && sync_.back().dcs && sync_.back().event.kind == HostEvent::DataWrite;
                const As end = Cycles() * kTmsCycle + (data ? data_skew : 0);
                if (end < target_) target_ = std::max(end, base);
            }
            if (dcs_frontier.empty() && target_ >= dcs.local_ns * kNs + kAdspCycle) {
                if (!RunAdsp(target_ / kNs)) return;
                if (dcs.local_ns * kNs < target_) target_ = std::max(dcs.local_ns * kNs, base);
            }
            base = target_;
        }
    }

private:
    struct Sync { As expire; Applied event; bool dcs;
        template<class A> friend void Fields(A& a, Sync& x) { a(x.expire, x.event, x.dcs); } };   // dcs false: a TMS input-line change (nothing modeled to apply)
    std::deque<Sync> sync_;
    bool adsp_sync_ = false, boost_ = false, in_tms_ = false;   // in_tms_: Run stopped inside a TMS timeslice
    As boost_expire_ = 0, dcs_cut_ = kNever, target_ = 0;   // dcs_cut_: a DCS timer that ended the last timeslice; target_: of this one
    uint64_t limit_ = 0;                                    // TMS cycle budget end of this timeslice
    size_t queued_ = 0;                                     // sync_ size at its start
    // screen.cpp timers on the raster VideoTiming uses: VBLANK starts at k frames and lasts 35 lines.
    As vblank_begin_ = As(o.video.frame), vblank_end_ = kNever, line0_ = As(o.video.TimeUntilPos(0, 0)), sound_ = kSecond / 50;

    As NextTimer() const {
        return std::min({As(o.video.NextExpire()), vblank_begin_, vblank_end_, line0_, sound_, sync_.empty() ? kNever : sync_.front().expire});
    }
    void Queue(HostEvent::Kind kind, uint64_t write_cycle, uint16_t value, bool dcs = true) {
        sync_.push_back({write_cycle * kTmsCycle, {kind, write_cycle, Cycles(), value, 0}, dcs});
    }
    void StartTms(uint64_t budget) {
        limit_ = Cycles() + budget;
        o.slice_end = limit_;   // m_icount for the restartable FILL (Ops::FillXY)
        if (dcs_frontier.empty()) { s.dcs_control = dcs.latch_control; o.dcs_output = dcs.output_data; }   // the ADSP ran to the previous target
        o.TimesliceStart();   // at least one instruction follows, even past the budget
    }
    // Returns false when `stop` asked to end the run.
    bool RunTms() {
        do {
            if (stop && stop(*this)) return false;
            StepGenerated(o);
            if (const uint32_t vector = o.video.Vector(s.status)) DeferredInterrupt(vector);
            for (const auto& w : o.dcs_writes) {
                // The event time is the instruction start only where the write precedes every counted cycle:
                // MOVE Rs,@abs,0 (MOVE_RA) and MOVE Rs,*Rd,0 (MOVE_RN) in 34010ops.hxx.
                const uint16_t op = o.program.Word(o.instruction_pc);
                if ((op & 0xffe0) != 0x0580 && (op & 0xfe00) != 0x8000)
                    o.Reject("DCS write from an unqualified instruction form", o.instruction_pc);
                if (!dcs_frontier.empty()) o.Reject("DCS host write past the ADSP translation frontier", o.instruction_pc);
                Queue(w.sound ? HostEvent::DataWrite : (w.value & 0x10) ? HostEvent::ResetAssert : HostEvent::ResetRelease,
                      o.instruction_cycles, w.value);
            }
            if (o.syncs) {   // HSTCTLH / DMA_COMMAND writes, at the access time: MOVE Rs,@abs,0, MOVE *Rs+,*Rd+,0 and
                             // MOVE -*Rs,-*Rd,F before counting cycles (measured, tools/mk3/reference_dma.py); MMTM at start + elapsed;
                             // MOVE Rs,*Rd,0 (MOVE_RN: WFIELD0 then COUNT_CYCLES(1), J2 attract route 0xff801b10)
                const uint16_t op = o.program.Word(o.instruction_pc);
                if (o.syncs != 1 || !o.dcs_writes.empty() ||
                    ((op & 0xffe0) != 0x0580 && (op & 0xfe00) != 0x9800 && (op & 0xfc00) != 0xa800 && (op & 0xffe0) != 0x0980 &&
                     (op & 0xfe00) != 0x8000))
                    o.Reject("synchronizing TMS input-line write from an unqualified instruction form", o.instruction_pc);
                if (input_syncs) Queue(HostEvent::ResetAssert, o.sync_cycle, 0, false);
                o.syncs = 0;
            }
            o.dcs_writes.clear();
            for (const uint16_t value : o.dcs_reads) {
                // dcs data_r (auto-ack) does not synchronize: the output latch empties at the read, inside this timeslice,
                // which started at `base`. Qualified: MOVE @abs,Rd,0 (MOVE_AR) reads before counting its cycles.
                if ((o.program.Word(o.instruction_pc) & 0xffe0) != 0x05a0) o.Reject("DCS read from an unqualified instruction form", o.instruction_pc);
                const uint64_t ns = o.instruction_cycles * kTmsCycle / kNs, slice = base / kNs;
                host_events.push_back({HostEvent::DataRead, o.instruction_cycles, Cycles(), value, dcs.local_ns});
                AdspEvent({HostEvent::DataRead, ns, ns, 0, slice, slice});
            }
            o.dcs_reads.clear();
        } while (sync_.size() == queued_ && Cycles() < limit_);   // a synchronize() aborts the timeslice
        return true;
    }
    // Returns false when `dcs_stop` asked to end the run. Input-latch accesses (IRQ2 line, output latch) and the
    // SYSCONTROL reboot synchronize: the ADSP timeslice ends after that instruction.
    bool RunAdsp(uint64_t target_ns) {
        bool user = false;
        dcs.return_at_sync = true;   // the timeslice ends after a self-synchronizing instruction
        const As next = NextTimer();   // an internal-timer arming before it aborts the ADSP timeslice
        dcs.host_next_ns = next == kNever ? ~0ull : uint64_t((next + kNs - 1) / kNs);
        const uint64_t before = dcs.synchronizations;
        try {
            // no predicate without a dcs_stop: the ADSP's loop then calls nothing before each instruction
            if (dcs_stop) dcs.Run({}, target_ns, [&](const Dcs8k& d) { return user = dcs_stop(d); });
            else dcs.Run({}, target_ns, nullptr);
        } catch (const std::runtime_error& e) {
            dcs_frontier = e.what();
            o.dcs_known = false;
        }
        adsp_sync_ |= dcs.synchronizations != before;
        return !user;
    }
    void Apply(const Sync& sync) {
        if (!dcs_frontier.empty()) o.Reject("DCS host write past the ADSP translation frontier", 0);
        auto event = sync.event;
        event.adsp_ns = dcs.local_ns;
        // Dcs8k applies a host event after running to its end: an event that ends now applies now.
        AdspEvent({event.kind, dcs.local_ns, dcs.local_ns, event.value, 0, 0});
        if (event.kind == HostEvent::DataWrite) { boost_ = true; boost_expire_ = sync.expire + 5000 * kNs; }
        host_events.push_back(event);
    }
    // A rejection while applying a host event (e.g. a DCS reset the Dcs8k does not verify) stops the ADSP there; the
    // TMS goes on until its next DCS access, which rejects.
    void AdspEvent(const HostEvent& event) {
        try { dcs.Run({event}, dcs.local_ns, nullptr); }
        catch (const std::runtime_error& e) { dcs_frontier = e.what(); o.dcs_known = false; }
    }
    // tms34010.cpp check_interrupt runs only at execute_run's start (every timeslice), in SET_ST (POPST, PUTST, RETI),
    // EINT, INTENB writes and the window-mode-1 paths of PIXBLT/FILL (rejected natively, Ops::GfxBits); execute_run's loop never checks between
    // instructions. A pending enabled interrupt after any other instruction therefore waits for the next timeslice
    // start, where TimesliceStart takes it. After an instruction that checks it itself, the executor should have taken
    // it: that stays a rejection in both profiles. Strict: the deferral is not yet compared with the Original.
    void DeferredInterrupt(uint32_t vector) {
        const uint16_t op = o.program.Word(o.instruction_pc);
        const bool checks = op == 0x0d60 || op == 0x01c0 || (op & 0xffe0) == 0x01a0 || op == 0x0940;
        if (checks || (profile == Profile::Strict && !Accepted(routes, Unverified::IrqPath, vector)))
            o.Reject("interrupt pending at an instruction boundary inside a timeslice", s.pc);
        if (first) first->Note(Unverified::IrqPath, vector, Cycles());
    }
    bool FireTimers() {   // execute_timers: every timer due at the base time, earliest first
        bool fired = adsp_sync_ || dcs_cut_ <= base;
        adsp_sync_ = false; if (dcs_cut_ <= base) dcs_cut_ = kNever;
        for (As next; (next = NextTimer()) <= base; fired = true) {
            if (!sync_.empty() && sync_.front().expire == next) { if (sync_.front().dcs) Apply(sync_.front()); sync_.pop_front(); }
            else if (As(o.video.NextExpire()) == next) o.Video([&] { o.video.FireNext(); }, 0xc0000000);
            else if (vblank_begin_ == next) {
                if (scanout) scanout->Present(o.palette);
                vblank_end_ = next + 35 * As(o.video.scan); vblank_begin_ += As(o.video.frame);
            }
            else if (vblank_end_ == next) vblank_end_ = kNever;
            else if (line0_ == next) { if (scanout) scanout->ResetPartial(); line0_ += As(o.video.frame); }
            else sound_ += kSecond / 50;
        }
        return fired;
    }
};
}
