#pragma once
#include "mk3_reset.hpp"
#include "mk3_pixblt.hpp"
#include "mk3_video.hpp"
#include "mk3_dma.hpp"
#include "mk3_profile.hpp"
#include <algorithm>
#include <climits>
#include <array>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace mk3 {
inline const std::vector<uint8_t> kNoGraphics;   // runs without the graphics ROMs: any DMA ROM read rejects
// Read-only observation of the CPU's stores into m_local_videoram, after each store: midtunit_vram_w (two words per
// access; mask = the bits it replaced: 0xffff with videobank 1, 0xff00 with videobank 0) and shift-register transfers to
// VRAM (1024 words, mask 0xffff). pc and cycle are the writing instruction's PC and start cycle (ResetTrace::writes).
// It must not touch the emulation; Ops::vram_observer is null by default.
struct CpuVramObserver {
    virtual ~CpuVramObserver() = default;
    virtual void CpuVram(uint32_t pc, size_t word, uint16_t value, uint16_t mask, uint64_t cycle) = 0;
    // `count` whole words one instruction stored in a row (a shift-register transfer): as that many CpuVram calls.
    virtual void CpuVramRun(uint32_t pc, size_t word, size_t count, const uint16_t* values, uint64_t cycle) {
        for (size_t k = 0; k < count; ++k) CpuVram(pc, word + k, values[k], 0xffff, cycle);
    }
};
// Fixed-translation primitives for blocks emitted by tools/mk3/generate.py (MK2 emitter semantics,
// costs from the pinned tms34010 model). Wolf-Unit bus: main RAM, CMOS with write enable, program ROM.
// Anything else rejects with its PC and address. TMS34010 I/O registers, display timing and interrupt sources live in
// VideoTiming (mk3_video.hpp), the DMA registers, blitter and video RAM in Dma (mk3_dma.hpp); check_interrupt runs inside
// EINT/POPST/PUTST/RETI and, through the board scheduler, at every timeslice start (tms34010.cpp execute_run).
// Every Ops member but the references, so that a Board snapshot (mk3_board.hpp) copies the whole CPU-side state at once.
struct OpsState {
    std::vector<uint16_t> ram = std::vector<uint16_t>(0x40000); // 0x01000000-0x013fffff; MAME RAM starts zeroed.
    std::vector<uint16_t> cmos;                                  // 0x01400000-0x0145ffff, 24576 words.
    bool cmos_enable = false;
    std::vector<uint16_t> palette = std::vector<uint16_t>(0x8000); // 0x01880000-0x018fffff RAM share (xRGB555).
    Dma blitter;                                                   // DMA blitter: m_dma_register, m_local_videoram
    VideoTiming video;                                             // TMS34010 I/O registers, scanline and DMA timers
    unsigned elapsed = 0;    // cycles the current instruction had counted at its access (MMTM/MMFM), else 0
    // Every handler entry (interrupt taken, or TRAP): entry cycle, vector, handler, pushed PC and ST, SP after the pushes.
    struct Interrupt { uint64_t entry; uint32_t vector, handler, pc, status, sp; };
    std::vector<Interrupt> interrupts;
    // midwunit.h m_ports {IN0, IN1, DSW, IN2} at io offsets 0-3. Default: nothing pressed (all active low) and the
    // INPUT_PORTS(mk3) DIP defaults: 0x0001|0x007c|0x0100|0x0400|0x0800|0x1000|0x2000|0x4000|0x8000 = 0xfd7d.
    std::array<uint16_t, 4> ports{0xffff, 0xffff, 0xfd7d, 0xffff};
    PixbltProgress pixblt;                                         // PIXBLT B,XY restart state (ST.P).
    uint32_t instruction_pc = 0;
    bool taken_ = false;               // an INTENB write took an interrupt: Finish continues at taken_handler_
    uint32_t taken_handler_ = 0;
    size_t taken_index_ = 0;
    uint64_t instruction_cycles = 0;
    // DCS host port, driven by the board co-simulation (mk3_board.hpp). Without it the latch stays at the value
    // proven up to the first sound-port write and sound-port accesses reject.
    struct DcsWrite { bool sound; uint16_t value; };
    bool cosim = false;
    bool dcs_known = true;              // false once the ADSP stopped at its translation frontier
    std::vector<DcsWrite> dcs_writes;   // synchronizing writes of the current instruction (io[1], sound port)
    uint16_t dcs_output = 0;            // dcs m_output_data as of the timeslice start (set by the board)
    std::vector<uint16_t> dcs_reads;    // sound-port reads (dcs data_r) of the current instruction, values returned
    unsigned syncs = 0;                 // other synchronize() requests of the current instruction (TMS input lines)
    uint64_t sync_cycle = 0;            // machine time (cycles) of the last such request: instruction start + elapsed
    uint64_t int1_seen = 0;             // VideoTiming::int1_count already applied to the DMA registers (dma_done)
    int dma_skew = 0;                   // negative control: whole cycles added to every DMA done time
    // Profiles of the whole-image translation (tools/mk3/generate.py, mk3_profile.hpp). Strict: a block or direct branch
    // outside the measured PCs rejects exactly as the measured-only build did. Game: it runs and `first` logs its first execution.
    Profile profile = Profile::Strict;
    FirstExecution* first = nullptr;
    const RouteVerified* routes = nullptr;   // Strict: route-verified keys also run (mk3_profile.hpp); null = lot only
    std::vector<uint8_t> noted;         // Game: per PC slot, 1 = block entry, 2/4 = edge to the taken/fallthrough target noted
    uint32_t io_written = 0;            // TMS34010 I/O registers written since reset, one bit each (CPU drawing state)
    CpuVramObserver* vram_observer = nullptr;   // read-only CPU VRAM store observation (above)
    static constexpr uint32_t kIoDefaultRead = 0x01800000;   // DevicePath ids: | io offset 5-7 (io_r default case)
    static constexpr uint32_t kControlRead = 0x01b00000;     // DevicePath id: midwunit_control_r
    bool io_default_reads = true;       // negative control: false rejects the io_r default case and midwunit_control_r
    std::array<uint16_t, 1024> shiftreg{};   // TMS34010 m_shiftreg (Ops::ShiftRegister)
    uint64_t slice_end = ~0ull;              // m_icount end of the timeslice (Ops graphics ops, Board::RunTms)
    uint64_t empty_reads = 0;   // DCS data reads on an empty output latch (each ate min(4444, m_icount) cycles)
    // Widescreen shadow pass (mk3_wide.hpp; not state, never on at an instruction boundary of the game): while on, a
    // main RAM store is journaled (word index, old value) and nothing else is stored, recorded or noted; a device access
    // or an interrupt is refused (shadow_fault). The pass undoes the journal and restores the CPU itself.
    bool shadow_on = false, shadow_fault = false;
    std::vector<std::pair<uint32_t, uint16_t>> shadow_journal;
    OpsState(std::vector<uint16_t> cmos_words, const std::vector<uint8_t>& graphics) : cmos(std::move(cmos_words)), blitter(graphics) {}
};
class Ops : public OpsState {
public:
    const Program& program;
    ResetTrace& s;
    std::array<uint16_t, 18>& dma = blitter.reg;                   // midtunit m_dma_register (DMA_LRSKIP..DMA_RIGHTCLIP)
    std::vector<uint16_t>& vram = blitter.vram;                    // midtunit m_local_videoram (0x100000 bytes)
    std::array<uint16_t, 32>& tms_io = video.io;                   // 0xc0000000-0xc00001f0

    Ops(const Program& p, ResetTrace& trace, std::vector<uint16_t> cmos_words, const std::vector<uint8_t>& graphics = kNoGraphics)
        : OpsState(std::move(cmos_words), graphics), program(p), s(trace) {
        if (cmos.size() != 0x6000) throw std::runtime_error("MK3 CMOS image must hold 24576 words");
        for (const auto& [address, value] : s.ram) ram.at((address - 0x01000000) / 16) = value;
    }
    // Board snapshot: the state of `x`, the references bound to `trace` and to this copy's own devices.
    Ops(const Ops& x, ResetTrace& trace) : OpsState(x), program(x.program), s(trace) {}
    Ops(const Ops&) = delete;
    void Begin() { instruction_pc = s.pc; instruction_cycles = s.cycles; }
    // Entry into a translated block that was not compared with the Original.
    void Block(uint32_t pc) {
        if (profile == Profile::Strict && !Accepted(routes, Unverified::TmsBlock, pc)) Reject("PC outside the authenticated fixed translation", pc);
        Note(Unverified::TmsBlock, pc, 0, 1);
    }
    // Direct branch of a measured block to a candidate target outside the measured PCs (next: the fallthrough).
    void Edge(uint32_t target, uint32_t from, bool next) {
        if (profile == Profile::Strict && !Accepted(routes, Unverified::TmsEdge, target, from)) Reject("unobserved branch", from);
        Note(Unverified::TmsEdge, target, from, next ? 4 : 2);
    }
    void Note(Unverified kind, uint32_t id, uint32_t from, uint8_t bit) {
        if (!first || shadow_on) return;
        if (noted.empty()) noted.resize(size_t(0x100000000ull - kProgramBaseBit) >> 4);
        auto& seen = noted[((kind == Unverified::TmsBlock ? id : from) - kProgramBaseBit) >> 4];
        if (!(seen & bit)) { seen |= bit; first->Note(kind, id, s.cycles, from); }
    }
    [[noreturn]] void Reject(const char* why, uint32_t address) const {
        throw std::runtime_error(std::string("MK3 ") + why + "; PC " + std::to_string(instruction_pc) + "; address " + std::to_string(address));
    }
    // VideoTiming's and Dma's rejections (unmodelled behaviour) as explicit rejections at this instruction. A done
    // timer fired by the call clears DMA_COMMAND bit 15 (dma_done, midtunit_v.cpp:628-632) at once.
    template <class F> auto Video(F f, uint32_t address) {
        try {
            if constexpr (std::is_void_v<decltype(f())>) { f(); DmaDone(); }
            else { auto value = f(); DmaDone(); return value; }
        } catch (const std::runtime_error& e) { Reject(e.what() + 4, address); }
    }
    void DmaDone() { if (video.int1_count != int1_seen) { int1_seen = video.int1_count; blitter.Done(); } }
    void Finish(uint32_t next, unsigned cost) {
        if (taken_) { next = taken_handler_; cost += 16; taken_ = false; interrupts[taken_index_].entry = s.cycles + cost; }
        s.pc = next; s.cycles += cost; ++s.instructions;
    }
    void Zero(uint32_t value) { s.status = (s.status & ~0x20000000u) | (value ? 0 : 0x20000000u); }
    void Load(uint32_t& reg, uint32_t value, uint32_t next, unsigned cost) {
        reg = value; s.status = (s.status & ~0xb0000000u) | (value & 0x80000000u); Zero(value); Finish(next, cost);
    }
    void Clear(uint32_t& reg, uint32_t next) { reg = 0; Zero(reg); Finish(next, 1); }
    void Arithmetic(uint32_t& reg, uint32_t value, bool subtract, uint32_t next, unsigned cost) {
        const uint32_t old = reg, result = subtract ? old - value : old + value;
        s.status = (s.status & ~0xf0000000u) | (result & 0x80000000u);
        if (subtract ? value > old : result < old) s.status |= 0x40000000u;
        if ((subtract ? old ^ value : ~(old ^ value)) & (old ^ result) & 0x80000000u) s.status |= 0x10000000u;
        reg = result; Zero(reg); Finish(next, cost);
    }
    void Shift(uint32_t& reg, unsigned count, bool right, uint32_t next) {
        if (count > 31) Reject("shift outside translated counts", count);
        const uint32_t old = reg;
        s.status &= ~0x60000000u;
        if (count) { s.status |= ((old >> (right ? count - 1 : 32 - count)) & 1) << 30; reg = right ? old >> count : old << count; }
        Zero(reg); Finish(next, 1);
    }
    // Register n of file A or B: 15 is SP in both (tms34010.cpp AREG(i) = m_regs[i], BREG(i) = m_regs[30 - i]), so Rd 14/15
    // of MPYS/MPYU/DIVS/DIVU reach SP through R##REG(DSTREG|1) / R##REG(DSTREG+1) as in 34010ops.hxx.
    uint32_t& Reg(bool bank_b, unsigned n) { return n == 15 || !bank_b ? s.a[n] : s.b[n]; }
    void MultiplyUnsigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {
        const unsigned width = FieldWidth(1);
        if (width != 32) value &= (uint32_t(1) << width) - 1;
        const uint64_t product = uint64_t(value) * Reg(bank_b, dest);
        Zero(product ? 1 : 0);
        Reg(bank_b, dest) = uint32_t(product >> 32); Reg(bank_b, dest | 1) = uint32_t(product); // Odd Rd keeps the low half.
        Finish(next, 21);
    }
    void DivideUnsigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {  // DIVU: even Rd = 64/32, odd Rd = 32/32.
        uint32_t& rd = Reg(bank_b, dest);
        s.status &= ~0x30000000u;
        if (!value) s.status |= 0x10000000u;
        else if (dest & 1) { rd /= value; Zero(rd); }
        else {
            uint32_t& rd2 = Reg(bank_b, dest + 1);
            const uint64_t dividend = (uint64_t(rd) << 32) | rd2;
            if (dividend / value > 0xffffffffu) s.status |= 0x10000000u;
            else { rd = uint32_t(dividend / value); rd2 = uint32_t(dividend % value); Zero(rd); }
        }
        Finish(next, 37);
    }
    void ModuloUnsigned(uint32_t value, uint32_t& dest, uint32_t next) {  // MODU
        s.status &= ~0x30000000u;
        if (value) { dest %= value; Zero(dest); } else s.status |= 0x10000000u;
        Finish(next, 35);
    }
    unsigned FieldWidth(unsigned field) const { const unsigned width = (s.status >> (field * 6)) & 31; return width ? width : 32; }
    void LeadingOne(uint32_t value, uint32_t& dest, uint32_t next) {  // LMO: leading zero count, Z from the source.
        Zero(value); dest = 0;
        if (value) while (!(value & 0x80000000u)) { ++dest; value <<= 1; }
        Finish(next, 1);
    }
    // ADDC, SLA/SRA, SEXT, RL: MK2 boot-ops semantics (34010ops.hxx ADDC, SLA, SRA, SEXT, RL).
    void AddCarry(uint32_t& reg, uint32_t value, uint32_t next) {
        const uint32_t old = reg, result = old + value + ((s.status >> 30) & 1);
        s.status = (s.status & ~0xf0000000u) | (result & 0x80000000u);
        if (~value < old) s.status |= 0x40000000u;
        if (~(old ^ value) & (old ^ result) & 0x80000000u) s.status |= 0x10000000u;
        reg = result; Zero(reg); Finish(next, 1);
    }
    void ShiftArithmetic(uint32_t& reg, unsigned count, bool right, uint32_t next) {
        if (count > 31) Reject("arithmetic shift count outside translated counts", count);
        const uint32_t old = reg;
        s.status &= right ? ~0xe0000000u : ~0xf0000000u;
        if (!count) {}  // register count 0 (SLA_R/SRA_R): no shift, C (and V for SLA) cleared, NZ of Rd
        else if (right) { s.status |= ((old >> (count - 1)) & 1) << 30; reg = (old >> count) | ((old & 0x80000000u) ? (~uint32_t(0) << (32 - count)) : 0); }
        else {
            s.status |= ((old >> (32 - count)) & 1) << 30;
            reg = old << count;
            if ((old >> (31 - count)) != ((old & 0x80000000u) ? uint32_t((uint64_t(1) << (count + 1)) - 1) : 0)) s.status |= 0x10000000u;
        }
        s.status |= reg & 0x80000000u; Zero(reg); Finish(next, right ? 1 : 3);
    }
    void SignExtend(uint32_t& reg, unsigned field, uint32_t next) {
        const unsigned width = FieldWidth(field);
        if (width != 32) { const uint32_t mask = (uint32_t(1) << width) - 1; reg &= mask; if (reg & (uint32_t(1) << (width - 1))) reg |= ~mask; }
        s.status = (s.status & ~0xa0000000u) | (reg & 0x80000000u);
        Zero(reg); Finish(next, 3);
    }
    void Rotate(uint32_t& reg, unsigned count, uint32_t next) {
        const uint32_t old = reg;
        s.status &= ~0x60000000u;
        if (count) { s.status |= ((old >> (32 - count)) & 1) << 30; reg = (old << count) | (old >> (32 - count)); }
        Zero(reg); Finish(next, 1);
    }
    // ADDXY / SUBXY (34010ops.hxx ADD_XY, SUB_XY), MK2 boot-ops semantics.
    void SubtractXY(uint32_t& reg, uint32_t value, uint32_t next) {
        const int16_t dx = int16_t(reg), dy = int16_t(reg >> 16), sx = int16_t(value), sy = int16_t(value >> 16);
        s.status &= ~0xf0000000u;
        if (sx == dx) s.status |= 0x80000000u;
        if (sy > dy) s.status |= 0x40000000u;
        if (sy == dy) s.status |= 0x20000000u;
        if (sx > dx) s.status |= 0x10000000u;
        reg = uint16_t(dx - sx) | (uint32_t(uint16_t(dy - sy)) << 16); Finish(next, 1);
    }
    void AddXY(uint32_t& reg, uint32_t value, uint32_t next) {
        const uint16_t x = uint16_t(uint16_t(reg) + uint16_t(value)), y = uint16_t(uint16_t(reg >> 16) + uint16_t(value >> 16));
        s.status &= ~0xf0000000u;
        if (!x) s.status |= 0x80000000u;
        if (y & 0x8000) s.status |= 0x40000000u;
        if (!y) s.status |= 0x20000000u;
        if (x & 0x8000) s.status |= 0x10000000u;
        reg = x | (uint32_t(y) << 16); Finish(next, 1);
    }
    void MoveX(uint32_t source, uint32_t& dest, uint32_t next) { dest = (dest & 0xffff0000u) | (source & 0xffff); Finish(next, 1); }
    // Instruction families of the whole-image candidate (34010ops.hxx at the pinned commit; tools/mk3/test_families.py).
    void SubtractBorrow(uint32_t& reg, uint32_t value, uint32_t next) {  // SUBB: C and V from Rd - Rs (borrow-in not in C)
        const uint32_t old = reg, result = old - value - ((s.status >> 30) & 1);
        s.status = (s.status & ~0xf0000000u) | (result & 0x80000000u);
        if (value > old) s.status |= 0x40000000u;
        if ((old ^ value) & (old ^ result) & 0x80000000u) s.status |= 0x10000000u;
        reg = result; Zero(reg); Finish(next, 1);
    }
    void NegateBorrow(uint32_t& reg, uint32_t next) {  // NEGB: t = Rd + C, Rd = 0 - t, NZCV of 0 - t
        const uint32_t t = reg + ((s.status >> 30) & 1), result = 0u - t;
        s.status = (s.status & ~0xf0000000u) | (result & 0x80000000u);
        if (t) s.status |= 0x40000000u;
        if (t & result & 0x80000000u) s.status |= 0x10000000u;
        reg = result; Zero(reg); Finish(next, 1);
    }
    void Absolute(uint32_t& reg, uint32_t next) {  // ABS: NZV from 0 - Rd; Rd changes only when that is positive
        const uint32_t negated = 0u - reg;
        s.status = (s.status & ~0xb0000000u) | (negated & 0x80000000u) | (negated ? 0 : 0x20000000u) | (negated == 0x80000000u ? 0x10000000u : 0);
        if (negated && !(negated & 0x80000000u)) reg = negated;
        Finish(next, 1);
    }
    void MultiplySigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {  // MPYS: Rs sign-extended to FW1
        if (const unsigned width = (s.status >> 6) & 31) value = uint32_t(int32_t(value << (32 - width)) >> (32 - width));
        const int64_t product = int64_t(int32_t(value)) * int32_t(Reg(bank_b, dest));
        const uint32_t high = uint32_t(uint64_t(product) >> 32);
        s.status = (s.status & ~0xa0000000u) | (high & 0x80000000u) | (product ? 0 : 0x20000000u);
        Reg(bank_b, dest) = high; Reg(bank_b, dest | 1) = uint32_t(product);  // odd Rd keeps the low half
        Finish(next, 20);
    }
    void DivideSignedSingle(uint32_t& reg, uint32_t value, uint32_t next) {  // DIVS, odd Rd: Rd = Rd / Rs (32/32)
        s.status &= ~0xb0000000u;
        if (!value) s.status |= 0x10000000u;
        else if (reg == 0x80000000u && value == 0xffffffffu) Reject("DIVS overflow traps the host in the reference", value);
        else { reg = uint32_t(int32_t(reg) / int32_t(value)); s.status |= (reg & 0x80000000u) | (reg ? 0 : 0x20000000u); }
        Finish(next, 39);
    }
    void DivideSigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {  // DIVS, even Rd: Rd:Rd+1 (64 bits) / Rs
        uint32_t &rd = Reg(bank_b, dest), &rd2 = Reg(bank_b, dest + 1);
        s.status &= ~0xb0000000u;
        const int64_t dividend = int64_t((uint64_t(rd) << 32) | rd2);
        if (!value) s.status |= 0x10000000u;
        else if (dividend == INT64_MIN && value == 0xffffffffu) Reject("DIVS overflow traps the host in the reference", value);
        else {
            const int64_t quotient = dividend / int32_t(value);
            if (quotient != int32_t(quotient)) s.status |= 0x10000000u;
            else {
                rd2 = uint32_t(int32_t(dividend % int32_t(value))); rd = uint32_t(quotient);
                s.status |= (rd & 0x80000000u) | (rd ? 0 : 0x20000000u);
            }
        }
        Finish(next, 40);
    }
    void ModuloSigned(uint32_t value, uint32_t& dest, uint32_t next) {  // MODS: NZ from the remainder, V on a zero divisor
        s.status &= ~0xb0000000u;
        if (!value) s.status |= 0x10000000u;
        else if (dest == 0x80000000u && value == 0xffffffffu) Reject("MODS overflow traps the host in the reference", value);
        else { dest = uint32_t(int32_t(dest) % int32_t(value)); s.status |= (dest & 0x80000000u) | (dest ? 0 : 0x20000000u); }
        Finish(next, 40);
    }
    void ExchangeField(uint32_t& reg, bool field1, uint32_t next) {  // EXGF
        const unsigned shift = field1 ? 6 : 0;
        const uint32_t previous = (s.status >> shift) & 0x3f;
        s.status = (s.status & ~(0x3fu << shift)) | ((reg & 0x3f) << shift);
        reg = previous; Finish(next, 1);
    }
    // XY addressing: B4 OFFSET, B5 WSTART, B6 WEND, B9 COLOR1; CONVSP/CONVDP and the pixel shift as io_register_w sets them.
    bool Written(unsigned reg) const { return (io_written >> reg) & 1; }
    uint32_t XYToLinear(uint32_t xy, bool source) const {  // SXYTOL / DXYTOL
        const uint32_t conv = Conv(source);
        const uint16_t size = video.io[VideoTiming::PSIZE];
        const unsigned shift = !Written(VideoTiming::PSIZE) ? 0 : size == 2 ? 1 : size == 4 ? 2 : size == 8 ? 3 : size == 16 ? 4 : 0;
        return uint32_t(int32_t(int16_t(xy >> 16))) * conv + (uint32_t(int32_t(int16_t(xy))) << shift) + s.b[4];
    }
    void ConvertXY(uint32_t source, uint32_t& destination, uint32_t next) { destination = XYToLinear(source, false); Finish(next, 3); }  // CVXYL
    void CompareWindow(uint32_t source, uint32_t& dest, uint32_t next) {  // CPW: outcode of Rs against WSTART/WEND, V if outside
        const int16_t x = int16_t(source), y = int16_t(source >> 16);
        const uint32_t result = (int16_t(s.b[5]) > x ? 0x20 : 0) | (x > int16_t(s.b[6]) ? 0x40 : 0) |
                                (int16_t(s.b[5] >> 16) > y ? 0x80 : 0) | (y > int16_t(s.b[6] >> 16) ? 0x100 : 0);
        dest = result; s.status = (s.status & ~0x10000000u) | (result ? 0x10000000u : 0); Finish(next, 1);
    }
    bool WindowSkip(uint32_t xy) {  // WINDOW_CHECKING() 1-3 for the XY pixel writes: true when nothing is written
        const unsigned mode = (video.io[VideoTiming::CONTROL] >> 6) & 3;
        if (!mode) return false;
        const int16_t x = int16_t(xy), y = int16_t(xy >> 16);
        s.status &= ~0x10000000u;
        if (x < int16_t(s.b[5]) || x > int16_t(s.b[6]) || y < int16_t(s.b[5] >> 16) || y > int16_t(s.b[6] >> 16)) { s.status |= 0x10000000u; return true; }
        return mode == 1;
    }
    // set_pixel_function: DPYCTL.SRT selects the shift-register functions (0), else PSIZE 1..32 (any other value: 1 bit)
    unsigned PixelSize() const {
        if (!(io_written & (1u << VideoTiming::CONTROL | 1u << VideoTiming::PSIZE | 1u << VideoTiming::DPYCTL)))
            Reject("pixel access before the pixel function is set", 0);
        if (video.io[VideoTiming::DPYCTL] & 0x0800) return 0;
        if ((video.io[VideoTiming::CONTROL] >> 10) & 0x1f) Reject("pixel raster operation is not translated", video.io[VideoTiming::CONTROL]);
        const uint16_t size = video.io[VideoTiming::PSIZE];
        return size == 2 || size == 4 || size == 8 || size == 16 || size == 32 ? size : 1;
    }
    // TMS34010 m_shiftreg (zeroed at reset) and the midtunit callbacks (midtunit_v.cpp:334-343): 1024 VRAM words at the
    // bit address >> 3, whatever the pixel size, raster op or data.
    // (OpsState::shiftreg)
    void ShiftRegister(uint32_t address, bool to_vram) {
        const size_t word = address >> 3;
        if (shadow_on) { shadow_fault = true; return; }
        if (word + shiftreg.size() > vram.size()) Reject("shift-register transfer outside VRAM", address);
        if (to_vram) {
            std::copy(shiftreg.begin(), shiftreg.end(), vram.begin() + ptrdiff_t(word));
            if (vram_observer) vram_observer->CpuVramRun(instruction_pc, word, shiftreg.size(), vram.data() + word, instruction_cycles);
        } else std::copy_n(vram.begin() + ptrdiff_t(word), shiftreg.size(), shiftreg.begin());
    }
    uint32_t ReadPixel(uint32_t address) {  // RPIXEL
        const unsigned size = PixelSize();
        if (!size) { ShiftRegister(address, false); return shiftreg[0]; }   // read_pixel_shiftreg
        if (size == 32) return Read32(address & ~31u);
        if (size == 16) return Read16(address & ~15u);
        return (Read16(address & ~15u) >> (address & (16 - size))) & ((1u << size) - 1);
    }
    void WritePixel(uint32_t address, uint32_t data) {  // WPIXEL without raster op; CONTROL bit 5: transparency (pixel 0 kept)
        const unsigned size = PixelSize();
        if (!size) { ShiftRegister(address, true); return; }   // write_pixel_shiftreg ignores the data
        const bool transparent = video.io[VideoTiming::CONTROL] & 0x20;
        if (size == 32) { if (!transparent || data) Write32(address & ~31u, data); return; }
        if (size == 16) { if (!transparent || data) Write16(address & ~15u, uint16_t(data)); return; }
        const uint32_t mask = (1u << size) - 1, shift = address & (16 - size);
        data &= mask;
        if (transparent && !data) return;
        const uint32_t aligned = address & ~15u;
        Write16(aligned, uint16_t((Read16(aligned) & ~(mask << shift)) | (data << shift)));
    }
    void PixelStore(uint32_t address, uint32_t value, uint32_t next, unsigned cost) { WritePixel(address, value); Finish(next, cost); }  // PIXT Rs,*Rd / *Rs,*Rd
    void PixelLoad(uint32_t value, uint32_t& dest, uint32_t next, unsigned cost) {  // PIXT *Rs,Rd / *Rs.XY,Rd: V = pixel != 0
        dest = value; s.status = (s.status & ~0x10000000u) | (value ? 0x10000000u : 0); Finish(next, cost);
    }
    void WriteXYPixel(uint32_t xy, uint32_t value, uint32_t next) {  // PIXT Rs,*Rd.XY
        if (!WindowSkip(xy)) WritePixel(XYToLinear(xy, false), value);
        Finish(next, 4);
    }
    void CopyXYPixel(uint32_t source, uint32_t xy, uint32_t next) {  // PIXT *Rs.XY,*Rd.XY: no read when the window skips
        if (!WindowSkip(xy)) { const uint32_t value = ReadPixel(XYToLinear(source, true)); WritePixel(XYToLinear(xy, false), value); }
        Finish(next, 7);
    }
    void DrawAdvance(uint32_t& destination, uint32_t delta, uint32_t next) {  // DRAV: COLOR1 at Rd.XY, then Rd.XY += Rs.XY
        if (!WindowSkip(destination)) WritePixel(XYToLinear(destination, false), s.b[9]);
        destination = uint16_t(uint16_t(destination) + uint16_t(delta)) | uint32_t(uint16_t((destination >> 16) + (delta >> 16))) << 16;
        Finish(next, 4);
    }
    // Graphics ops of 34010gfx.hxx (pinned): FILL L/XY (fill), PIXBLT B,L/B,XY (pixblt_b), PIXBLT L,L/L,XY/XY,L/XY,XY
    // (pixblt, PBH clear) and LINE, in the op0 / op0_trans instances (raster op 0, transparency from CONTROL bit 5) at
    // BITS_PER_PIXEL = 1 << pixelsize_lookup[PSIZE & 0x1f]. word_read/word_write: memory_r/memory_w through the bus, or
    // with DPYCTL.SRT shiftreg_r (to_shiftreg, returns m_shiftreg[0]) / shiftreg_w (from_shiftreg, data ignored); fill
    // reads dummy_shiftreg_r. XY destinations: window mode 0 only (apply_window returns 0); window modes 1-3 and raster
    // ops 1-21 reject before any bus access. m_gfxcycles is eaten against m_icount (slice_end, Board::RunTms): a remainder
    // keeps ST.P and the PC (m_pc -= 0x10) for the restart; the counter is shared by every graphics op (pixblt.remaining).
    static constexpr uint32_t kStatusP = 1u << 25;
    uint32_t Conv(bool source) const {   // m_convsp / m_convdp (io_register_w), 0 before the first write
        const unsigned reg = source ? VideoTiming::CONVSP : VideoTiming::CONVDP;
        return Written(reg) ? uint32_t(1) << (~video.io[reg] & 31) : 0;
    }
    int GfxBits(bool xy_destination) const {
        static constexpr uint8_t kLookup[32] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4};
        const uint16_t control = video.io[VideoTiming::CONTROL];
        if ((control >> 10) & 0x1f) Reject("graphics raster operation is not translated", control);
        if (xy_destination && ((control >> 6) & 3)) Reject("graphics window checking is not translated", control);
        return 1 << kLookup[video.io[VideoTiming::PSIZE] & 0x1f];
    }
    bool ShiftMode() const { return video.io[VideoTiming::DPYCTL] & 0x0800; }
    uint16_t GfxRead(uint32_t address, bool dummy = false) {
        if (!ShiftMode()) return Read16(address);
        if (!dummy) ShiftRegister(address, false);
        return shiftreg[0];
    }
    void GfxWrite(uint32_t address, uint16_t data) { if (ShiftMode()) ShiftRegister(address, true); else Write16(address, data); }
    // Partial-word split shared by fill and pixblt_b.
    struct Partials { int left, right, full, words; };
    static Partials Split(uint32_t daddr, int dx, int bpp) {
        const int per_word = 16 / bpp;
        Partials p{(per_word - int((daddr & 15) / unsigned(bpp))) & (per_word - 1), int(((daddr + uint32_t(dx * bpp)) & 15) / unsigned(bpp)), 0, 0};
        p.full = dx - p.left - p.right;
        if (p.full < 0) { p.left = dx; p.right = p.full = 0; } else p.full /= per_word;
        p.words = p.full + (p.left != 0) + (p.right != 0);
        return p;
    }
    // Eat cycles: false = restart (ST.P and PC kept); true = ST.P cleared, `cycles` to count with the end updates.
    bool GfxEat(unsigned& cycles) {
        const uint64_t budget = slice_end > s.cycles ? slice_end - s.cycles : 1;
        if (pixblt.remaining > 0 && uint64_t(pixblt.remaining) > budget) { pixblt.remaining -= int32_t(budget); Finish(s.pc, unsigned(budget)); return false; }
        cycles = unsigned(pixblt.remaining); pixblt.remaining = 0; s.status &= ~kStatusP;
        return true;
    }
    void GfxRestartCheck() const { if (pixblt.remaining <= 0) Reject("ST.P set without native graphics progress", s.status); }
    static uint32_t Rows(uint32_t dydx) { return uint32_t(int32_t(int16_t(dydx >> 16))); }
    static void AddY(uint32_t& xy, uint32_t dydx) { xy = (xy & 0xffffu) | uint32_t(uint16_t((xy >> 16) + (dydx >> 16))) << 16; }
    void FillXY(uint32_t next) { Fill(false, next); }
    void Fill(bool linear, uint32_t next) {   // fill_l / fill_xy -> fill(dst_is_linear)
        if (!(s.status & kStatusP)) {
            const int bpp = GfxBits(!linear);
            const bool trans = video.io[VideoTiming::CONTROL] & 0x20;
            const int dx = int16_t(s.b[7]), dy = int16_t(s.b[7] >> 16);
            int32_t gfx = linear ? 4 : 6;
            uint32_t daddr = (linear ? s.b[2] : XYToLinear(s.b[2], false)) & ~uint32_t(bpp - 1);
            if (dx <= 0 || dy <= 0) { Finish(next, 0); return; }   // returns before ST.P and before eating cycles
            const Partials p = Split(daddr, dx, bpp);
            const uint16_t mask = uint16_t((1u << bpp) - 1), color = uint16_t(s.b[9]);
            gfx += 2; s.status |= kStatusP;
            for (int y = 0; y < dy; ++y, daddr += s.b[3]) {
                uint32_t word = daddr >> 4;
                gfx += p.words * (trans ? 4 : 2);   // compute_fill_cycles
                auto put = [&](int count, uint16_t dstmask, bool read) {
                    uint16_t dst = read ? GfxRead(word << 4, true) : 0;
                    for (int x = 0; x < count; ++x, dstmask = uint16_t(dstmask << bpp)) {
                        const uint16_t pixel = color & dstmask;
                        if (!trans || pixel) dst = uint16_t((dst & ~dstmask) | pixel);
                    }
                    GfxWrite(word++ << 4, dst);
                };
                if (p.left) put(p.left, uint16_t(mask << (daddr & 15)), true);
                for (int w = 0; w < p.full; ++w) put(16 / bpp, mask, trans);
                if (p.right) put(p.right, mask, true);
            }
            pixblt.remaining = gfx;
        } else GfxRestartCheck();
        unsigned cycles;
        if (!GfxEat(cycles)) return;
        if (linear) s.b[2] += Rows(s.b[7]) * s.b[3]; else AddY(s.b[2], s.b[7]);
        Finish(next, cycles);
    }
    void BlitBinary(bool linear, uint32_t next) {   // pixblt_b_l / pixblt_b_xy -> pixblt_b(dst_is_linear)
        if (!(s.status & kStatusP)) {
            const int bpp = GfxBits(!linear);
            const bool trans = video.io[VideoTiming::CONTROL] & 0x20;
            uint32_t saddr = s.b[0];
            const int dx = int16_t(s.b[7]), dy = int16_t(s.b[7] >> 16);
            int32_t gfx = linear ? 4 : 6;
            uint32_t daddr = (linear ? s.b[2] : XYToLinear(s.b[2], false)) & ~uint32_t(bpp - 1);
            if (dx <= 0 || dy <= 0) { Finish(next, 0); return; }
            const Partials p = Split(daddr, dx, bpp);
            gfx += (p.words * (trans ? 4 : 2) + p.words * bpp / 16 * 2) * dy + 2;   // compute_pixblt_b_cycles
            s.status |= kStatusP;
            const uint16_t mask = uint16_t((1u << bpp) - 1);
            for (int y = 0; y < dy; ++y, saddr += s.b[1], daddr += s.b[3]) {
                uint32_t sword = saddr >> 4, dword = daddr >> 4;
                uint16_t src = GfxRead(sword++ << 4), srcmask = uint16_t(1u << (saddr & 15));
                auto put = [&](int count, uint16_t dstmask, bool read) {
                    uint16_t dst = read ? GfxRead(dword << 4) : 0;
                    for (int x = 0; x < count; ++x, dstmask = uint16_t(dstmask << bpp)) {
                        const uint16_t pixel = uint16_t((src & srcmask ? s.b[9] : s.b[8]) & dstmask);
                        if (!trans || pixel) dst = uint16_t((dst & ~dstmask) | pixel);
                        srcmask = uint16_t(srcmask << 1);
                        if (!srcmask) { src = GfxRead(sword++ << 4); srcmask = 1; }
                    }
                    GfxWrite(dword++ << 4, dst);
                };
                if (p.left) put(p.left, uint16_t(mask << (daddr & 15)), true);
                for (int w = 0; w < p.full; ++w) put(16 / bpp, mask, trans);
                if (p.right) put(p.right, mask, true);
            }
            pixblt.remaining = gfx;
        } else GfxRestartCheck();
        unsigned cycles;
        if (!GfxEat(cycles)) return;
        s.b[0] += Rows(s.b[7]) * s.b[1];
        if (linear) s.b[2] += Rows(s.b[7]) * s.b[3]; else AddY(s.b[2], s.b[7]);
        Finish(next, cycles);
    }
    void Blit(bool src_linear, bool dst_linear, uint32_t next) {   // pixblt_l_l .. pixblt_xy_xy -> pixblt (PBH clear)
        if (!(s.status & kStatusP)) {
            const int bpp = GfxBits(!dst_linear);
            const uint16_t control = video.io[VideoTiming::CONTROL];
            if ((control >> 8) & 1) Reject("PIXBLT with PBH set (pixblt_r) is not translated", control);
            const bool trans = control & 0x20, yreverse = (control >> 9) & 1;
            uint32_t saddr = src_linear ? s.b[0] : XYToLinear(s.b[0], true);
            const int dx = int16_t(s.b[7]), dy = int16_t(s.b[7] >> 16);
            const int32_t gfx = 7 + (src_linear ? 0 : 2) + (dst_linear ? 0 : 2 + !src_linear);
            uint32_t daddr = (dst_linear ? s.b[2] : XYToLinear(s.b[2], false)) & ~uint32_t(bpp - 1);
            if (dx <= 0 || dy <= 0) { Finish(next, 0); return; }
            if ((!src_linear || !dst_linear) && yreverse) { saddr += uint32_t(dy - 1) * Conv(true); daddr += uint32_t(dy - 1) * Conv(false); }
            s.status |= kStatusP;
            const uint32_t mask = (1u << bpp) - 1;
            uint32_t readwrites = 0;
            for (int y = 0; y < dy; ++y) {
                uint32_t sword = saddr >> 4, dword = daddr >> 4, sbit = saddr & 15, dbit = daddr & 15, dst = 0;
                uint32_t src = GfxRead(sword++ << 4); ++readwrites;
                if (trans || (daddr & 15)) { dst = GfxRead(dword << 4); ++readwrites; }
                for (int x = 0; x < dx; ++x) {
                    if (sbit + unsigned(bpp) > 16) { src |= uint32_t(GfxRead(sword++ << 4)) << 16; ++readwrites; }
                    uint32_t pixel = (src >> sbit) & mask;
                    sbit += unsigned(bpp);
                    if (sbit > 16) { sbit -= 16; src >>= 16; }
                    if (trans && dbit + unsigned(bpp) > 16) { dst |= uint32_t(GfxRead((dword + 1) << 4)) << 16; ++readwrites; }
                    pixel <<= dbit;
                    const uint32_t dstmask = mask << dbit;
                    if (!trans || pixel) dst = (dst & ~dstmask) | pixel;
                    dbit += unsigned(bpp);
                    if (dbit > 16) { GfxWrite(dword++ << 4, uint16_t(dst)); ++readwrites; dbit -= 16; dst >>= 16; }
                }
                if (dbit > 0) {
                    if (dbit != 16) {
                        const uint32_t orig = GfxRead(dword << 4), keep = uint16_t(0xffffu << dbit);
                        dst = (dst & ~keep) | (orig & keep); ++readwrites;
                    }
                    GfxWrite(dword++ << 4, uint16_t(dst)); ++readwrites;
                }
                if (!yreverse) { saddr += s.b[1]; daddr += s.b[3]; } else { saddr -= s.b[1]; daddr -= s.b[3]; }
            }
            pixblt.remaining = gfx + int32_t(readwrites * 2) + dx * dy * (trans ? 2 : 0);   // PIXEL_OP_TIMING - 2
        } else GfxRestartCheck();
        unsigned cycles;
        if (!GfxEat(cycles)) return;
        if (src_linear) s.b[0] += Rows(s.b[7]) * s.b[1]; else AddY(s.b[0], s.b[7]);
        if (dst_linear) s.b[2] += Rows(s.b[7]) * s.b[3]; else AddY(s.b[2], s.b[7]);
        Finish(next, cycles);
    }
    // LINE 0/1 (34010gfx.hxx line): one pixel per execution (COUNT_UNKNOWN_CYCLES(2), m_pc -= 0x10); TEMP (B14) is the
    // boundary value from opcode bit 7; window mode 3 clips, modes 1-2 write as mode 0 (only a logerror in MAME).
    void Line(bool algorithm1, uint32_t next) {
        if (!(s.status & kStatusP)) { s.status |= kStatusP; s.b[14] = algorithm1 ? 1 : 0; }
        if (int32_t(s.b[10]) > 0) {
            --s.b[10];
            const int16_t x = int16_t(s.b[2]), y = int16_t(s.b[2] >> 16);
            if (((video.io[VideoTiming::CONTROL] >> 6) & 3) != 3 ||
                (x >= int16_t(s.b[5]) && x <= int16_t(s.b[6]) && y >= int16_t(s.b[5] >> 16) && y <= int16_t(s.b[6] >> 16)))
                WritePixel(XYToLinear(s.b[2], false), s.b[9]);
            const int dyy = int16_t(s.b[7] >> 16), dxx = int16_t(s.b[7]);
            const bool first_increment = int32_t(s.b[0]) >= int32_t(s.b[14]);
            s.b[0] += uint32_t(first_increment ? dyy * 2 - dxx * 2 : dyy * 2);
            const uint32_t inc = first_increment ? s.b[11] : s.b[12];
            s.b[2] = uint16_t(x + int16_t(inc)) | uint32_t(uint16_t(y + int16_t(inc >> 16))) << 16;
            Finish(s.pc, 2);
            return;
        }
        s.status &= ~kStatusP;
        Finish(next, 0);
    }
    // unimpl (34010ops.hxx): reserved opcodes the pinned 34010tbl.hxx maps there run as a 1-cycle no-op (m_pc + 16).
    void Reserved(uint32_t next) { Finish(next, 1); }
    void Record(std::vector<BusWrite>& list, uint32_t address, uint16_t value) { list.push_back({instruction_pc, address, value, instruction_cycles}); }
    uint16_t Read16(uint32_t address) {
        if (address & 15) Reject("unaligned word read", address);
        if (address >= kProgramBaseBit) return program.Word(address);
        if (address >= 0x01000000 && address <= 0x013ffff0) return ram[(address - 0x01000000) / 16];
        if (shadow_on) {   // memories only: video RAM, palette RAM, the DMA registers as they stand
            if (address <= 0x003ffff0) { const auto offset = (address / 16) * 2; return videobank() ? uint16_t((vram[offset] & 0xff) | vram[offset + 1] << 8) : uint16_t((vram[offset] >> 8) | (vram[offset + 1] & 0xff00)); }
            if (address >= 0x01880000 && address <= 0x018ffff0) return palette[(address - 0x01880000) / 16];
            if ((address & ~0x00080000u) >= 0x01a00000 && (address & ~0x00080000u) <= 0x01a000f0) return blitter.Read(((address & ~0x00080000u) - 0x01a00000) / 16);
            shadow_fault = true; return 0;
        }
        if (address >= 0x01400000 && address <= 0x0145fff0) {
            const auto value = cmos[(address - 0x01400000) / 16]; Record(s.reads, address, value); return value;
        }
        if (address >= 0x01480000 && address <= 0x014ffff0) { Record(s.reads, address, 0); return 0; } // write-only map: unmapped read 0.
        if (address <= 0x003ffff0) {  // midtunit_vram_r
            const auto offset = (address / 16) * 2;
            return videobank() ? uint16_t((vram[offset] & 0xff) | vram[offset + 1] << 8) : uint16_t((vram[offset] >> 8) | (vram[offset + 1] & 0xff00));
        }
        if (address >= 0x01880000 && address <= 0x018ffff0) return palette[(address - 0x01880000) / 16];
        if (address == 0x01600000 || address == 0x01600010) {  // security_r (0x01600000-0x0160001f): PIC data latch.
            const uint16_t value = s.pic.Read(); Record(s.reads, address, value); return value;
        }
        if (address >= 0x01800000 && address <= 0x0187fff0 && IoOffset(address) < 4) {
            const uint16_t value = ports[IoOffset(address)]; Record(s.reads, address, value); return value;
        }
        if (address >= 0x01800000 && address <= 0x0187fff0 && IoOffset(address) == 4) {
            if (!dcs_known) Reject("DCS latch read past the ADSP translation frontier", address);
            const uint16_t value = uint16_t(uint16_t(s.pic.Status()) << 12 | s.dcs_control); // PIC status_r << 12 | DCS control_r.
            Record(s.reads, address, value); return value;
        }
        if (address >= 0x01800000 && address <= 0x0187fff0) {
            // midwunit io_r, identity m_ioshuffle (machine_reset), offsets 5-7: the default case only logs and returns ~0
            // (midwunit_m.cpp:105-131). A DevicePath {id = kIoDefaultRead | offset, from = PC}: Strict rejects it unless
            // route-verified, Game notes its first execution. io_default_reads = false: negative control (rejects as before).
            const uint32_t path = kIoDefaultRead | IoOffset(address);
            if (!io_default_reads || (profile == Profile::Strict && !Accepted(routes, Unverified::DevicePath, path, instruction_pc)))
                Reject("read outside translated Wolf-Unit map", address);
            if (first) first->Note(Unverified::DevicePath, path, s.cycles, instruction_pc);
            Record(s.reads, address, 0xffff); return 0xffff;
        }
        if ((address == 0x01680000 || address == 0x01680010) && cosim) {  // midwunit sound_r (both words): dcs data_r & 0xff
            if (!dcs_known) Reject("DCS data read past the ADSP translation frontier", address);
            // dcs.cpp data_r: on an empty output latch (IS_OUTPUT_EMPTY, LCTRL bit 0x0400) m_maincpu->eat_cycles(4444);
            // diexec.h eat_cycles: m_icount = cycles > m_icount ? 0 : m_icount - cycles. MOVE @abs,Rd reads before counting
            // its own cycles, so m_icount here is slice_end - instruction start; the instruction's cost follows.
            if (s.dcs_control & 0x0400) {
                const uint64_t icount = slice_end > s.cycles + elapsed ? slice_end - s.cycles - elapsed : 0;
                s.cycles += std::min<uint64_t>(4444, icount); ++empty_reads;
            }
            const uint16_t value = dcs_output & 0xff;
            s.dcs_control |= 0x0400;   // m_auto_ack: delayed_ack_w() directly (output empty), no synchronize
            dcs_reads.push_back(value); Record(s.reads, address, value); return value;
        }
        if ((address & ~0x00080000u) >= 0x01a00000 && (address & ~0x00080000u) <= 0x01a000f0) {  // midtunit dma_r (+ mirror)
            const uint16_t value = blitter.Read(((address & ~0x00080000u) - 0x01a00000) / 16);
            Record(s.reads, address, value); return value;
        }
        if (address >= 0x01b00000 && address <= 0x01b00010) {
            // midwunit_control_r (midtunit_v.cpp:392-395): m_midtunit_control, both words; DevicePath kControlRead as above.
            if (!io_default_reads || (profile == Profile::Strict && !Accepted(routes, Unverified::DevicePath, kControlRead, instruction_pc)))
                Reject("read outside translated Wolf-Unit map", address);
            if (first) first->Note(Unverified::DevicePath, kControlRead, s.cycles, instruction_pc);
            Record(s.reads, address, s.video_control); return s.video_control;
        }
        if (address >= 0x02000000 && address <= 0x06fffff0) {  // midwunit_gfxrom_r: bank 0x800000 * control bits 8-9
            const size_t at = 0x800000u * ((s.video_control >> 8) & 3) + size_t((address - 0x02000000) >> 4) * 2;
            if (at + 1 >= blitter.gfx.size()) Reject("graphics ROM read past the loaded region", address);
            return uint16_t(blitter.gfx[at] | blitter.gfx[at + 1] << 8);   // a ROM: not a device read
        }
        if (address >= 0xc0000000 && address <= 0xc00001f0) {  // io_register_r at the access time
            const uint16_t value = Video([&] { return video.Read((address - 0xc0000000) / 16, instruction_cycles, elapsed); }, address);
            Record(s.reads, address, value); return value;
        }
        if (Unmapped(address)) return 0;   // no handler in main_map: MAME's unmap value 0 (e.g. *A4(80h) at 0xff95daa0, J3 route)
        Reject("read outside translated Wolf-Unit map", address);
    }
    uint32_t Read32(uint32_t address) {  // RFIELDMAC_32: an unaligned long reads the two covering dwords.
        if (const uint32_t shift = address & 15) {
            const uint32_t aligned = address & ~15u;
            const uint32_t low = Read32(aligned), high = Read32(aligned + 32);
            return (low >> shift) | (high << (32 - shift));
        }
        return Read16(address) | uint32_t(Read16(address + 16)) << 16;
    }
    uint32_t Read8(uint32_t address) {
        const auto aligned = address & ~15u, shift = address & 15;
        return ((shift > 8 ? Read32(aligned) : Read16(aligned)) >> shift) & 0xff;
    }
    void Write16(uint32_t address, uint16_t value) {
        if (address & 15) Reject("unaligned word write", address);
        if (shadow_on) {
            if (address >= 0x01000000 && address <= 0x013ffff0) { auto& word = ram[(address - 0x01000000) / 16]; shadow_journal.emplace_back((address - 0x01000000) / 16, word); word = value; }
            else shadow_fault = true;
            return;
        }
        if (cosim && !dcs_known && (address == 0x01680000 || (address >= 0x01800000 && address <= 0x0187fff0 && IoOffset(address) == 1)))
            Reject("DCS host write past the ADSP translation frontier", address);   // before any effect of the instruction
        Record(s.writes, address, value);
        if (address >= 0x01000000 && address <= 0x013ffff0) { ram[(address - 0x01000000) / 16] = value; return; }
        if (address >= 0x01400000 && address <= 0x0145fff0) {
            // midwunit cmos_w: one enabled write per cmos_enable_w; otherwise ignored ("Bad CMOS write").
            if (cmos_enable) cmos[(address - 0x01400000) / 16] = value;
            cmos_enable = false; return;
        }
        if (address >= 0x01480000 && address <= 0x014ffff0) { cmos_enable = true; return; }
        if (address >= 0x01800000 && address <= 0x0187fff0) {  // midwunit io_w; MK3 keeps the identity shuffle.
            const unsigned offset = IoOffset(address);
            s.io[offset] = value;
            if (offset == 1) s.pic.ResetLine(value & 0x20); // PIC reset_w(bit5)
            if (offset == 1 && cosim) dcs_writes.push_back({false, value}); // dcs reset_w(~bit4), synchronized
            // offset 3: watchdog, disabled for MK3.
            return;
        }
        if (address == 0x01680000 && cosim) { dcs_writes.push_back({true, uint16_t(value & 0xff)}); return; } // sound_w offset 0: dcs data_w
        if (address <= 0x003ffff0) {  // midtunit_vram_w, full-word access.
            const auto offset = (address / 16) * 2;
            if (videobank()) { vram[offset] = uint16_t((value & 0xff) | (dma[8] & 0xff) << 8); vram[offset + 1] = uint16_t((value >> 8) | (dma[8] & 0xff00)); }
            else { vram[offset] = uint16_t((vram[offset] & 0xff) | (value & 0xff) << 8); vram[offset + 1] = uint16_t((vram[offset + 1] & 0xff) | (value & 0xff00)); }
            if (vram_observer) {
                const uint16_t mask = videobank() ? 0xffff : 0xff00;
                vram_observer->CpuVram(instruction_pc, offset, vram[offset], mask, instruction_cycles);
                vram_observer->CpuVram(instruction_pc, offset + 1, vram[offset + 1], mask, instruction_cycles);
            }
            return;
        }
        if (address >= 0x01880000 && address <= 0x018ffff0) { palette[(address - 0x01880000) / 16] = value; return; }
        if ((address & ~0x00080000u) >= 0x01a00000 && (address & ~0x00080000u) <= 0x01a000f0) {  // midtunit dma_w (+ mirror)
            // dma_w; a DMA_COMMAND write drops INT1 and (bit 15) draws and arms the done timer at the access time.
            const int pixels = Video([&] { return blitter.Write(((address & ~0x00080000u) - 0x01a00000) / 16, value, 0xffff, instruction_cycles); }, address);
            if (pixels < 0) return;
            Video([&] { video.DmaCommand(instruction_cycles, dma[Dma::COMMAND], pixels, elapsed); }, address);
            if (dma_skew && (dma[Dma::COMMAND] & 0x8000)) video.dma_expire += VideoTiming::Atto(dma_skew) * VideoTiming::kCycle;
            // m_dma_irq_cb(CLEAR_LINE): TMS input line 0, set_state_synced -> synchronize() at the access time
            if (cosim) { ++syncs; sync_cycle = instruction_cycles + elapsed; }
            return;
        }
        if (address >= 0x01b00000 && address <= 0x01b00010) { s.video_control = value; return; } // midwunit_control_w
        if (address >= 0xc0000000 && address <= 0xc00001f0) { WriteTmsIo((address - 0xc0000000) / 16, value); return; }
        if (address == 0x01600000) { s.pic.Write(uint8_t(value)); return; } // security_w offset 0, low byte.
        if (address == 0x01600010) return;                                   // security_w ignores offset 1.
        if (Unmapped(address)) return;   // no handler in midwunit_state::main_map: MAME drops the write (e.g. MOVE A14,@1D01010h at 0xff929380)
        Reject("write outside translated Wolf-Unit map", address);
    }
    // Outside every range of midwunit_state::main_map (midwunit.cpp:113-127) and the TMS34010 I/O registers. The read-only
    // graphics ROM window and program ROM have no write handler either.
    static bool Unmapped(uint32_t a) {
        static constexpr uint32_t kMapped[][2] = {{0x00000000, 0x003fffff}, {0x01000000, 0x0145ffff}, {0x01480000, 0x014fffff},
            {0x01600000, 0x0160001f}, {0x01680000, 0x0168001f}, {0x01800000, 0x018fffff}, {0x01a00000, 0x01a000ff},
            {0x01a80000, 0x01a800ff}, {0x01b00000, 0x01b0001f}, {0xc0000000, 0xc00001ff}};
        if (a >= 0x02000000 && a <= 0x06ffffff) return true;
        if (a >= kProgramBaseBit) return true;
        for (const auto& r : kMapped) if (a >= r[0] && a <= r[1]) return false;
        return true;
    }
    static unsigned IoOffset(uint32_t address) { return ((address - 0x01800000) >> 4) % 16 % 8; }
    bool videobank() const { return (s.video_control >> 11) & 1; }
    void WriteTmsIo(unsigned reg, uint16_t value) {  // tms34010_device::io_register_w
        io_written |= 1u << reg;
        if (reg == VideoTiming::HSTCTLH && (value & 0x8100)) Reject("HSTCTLH halt/NMI is not translated", value);
        Video([&] { video.Write(reg, value, instruction_cycles); }, 0xc0000000 + reg * 16);
        if (reg == VideoTiming::HSTCTLH && cosim) { ++syncs; sync_cycle = instruction_cycles + elapsed; }   // set_input_line(INPUT_LINE_HALT, CLEAR_LINE): set_state_synced -> synchronize()
        // INTENB: io_register_w calls check_interrupt in the middle of the instruction (tms34010.cpp:1344-1346). m_pc is
        // already past the instruction (ROPCODE/PARAM fetches): it pushes that PC and ST, RESET_ST, m_pc = the vector,
        // 16 cycles; the rest of the instruction (e.g. the INTPEND half of a long write) follows, then Finish jumps to the
        // handler. Qualified forms (no ST change after the write): MOVE Rs,@L,F (0x0580/0x0780, 48 bits) and MOVE Rs,*Rd,F
        // (0x8000-0x83ff, 16 bits).
        if (reg == VideoTiming::INTENB && video.Vector(s.status)) {
            const uint16_t op = program.Word(instruction_pc);
            const unsigned size = (op & 0xfde0) == 0x0580 ? 48 : (op & 0xfc00) == 0x8000 ? 16 : 0;
            if (!size || taken_) Reject("interrupt taken inside an INTENB write from an unqualified instruction form", value);
            uint32_t next = instruction_pc + size; unsigned cost = 0;
            CheckInterrupt(next, cost, false);
            taken_ = true; taken_handler_ = next; taken_index_ = interrupts.size() - 1;
        }
    }
    // check_interrupt (tms34010.cpp:619-706) inside an instruction of `cost` cycles, or at a timeslice start (cost 0):
    // the vector taken pushes the PC to resume and ST, resets ST to 0x10 and counts 16 cycles. The pushes are recorded
    // at the instruction doing the check; at a timeslice start, at the last instruction (the debugger's pc/totalcycles).
    // compared: the acceptance points compared with the Original (timeslice start, EINT; lot 10 and the routes). Elsewhere
    // (SET_ST in POPST/PUTST/RETI, INTENB writes) the acceptance is an IrqPath {id = vector, from = instruction PC}: Strict
    // rejects it unless route-verified, Game takes it and notes its first execution.
    void CheckInterrupt(uint32_t& next, unsigned& cost, bool compared = true) {
        if (shadow_on) return;
        const uint32_t vector = Video([&] { return video.Vector(s.status); }, 0xc0000120);
        if (!vector) return;
        if (!compared) {
            if (profile == Profile::Strict && !Accepted(routes, Unverified::IrqPath, vector, instruction_pc))
                Reject("interrupt taken inside POPST/PUTST/RETI/INTENB is not compared with the Original", vector);
            if (first) first->Note(Unverified::IrqPath, vector, s.cycles, instruction_pc);
        }
        const uint32_t status = s.status, pc = next;
        Push(pc); Push(status); s.status = 0x10;
        next = Read32(vector);
        cost += 16;
        interrupts.push_back({s.cycles + cost, vector, next, pc, status, s.a[15]});
    }
    void TimesliceStart() { uint32_t next = s.pc; unsigned cost = 0; CheckInterrupt(next, cost); s.pc = next; s.cycles += cost; }
    // EINT checks for interrupts, DINT does not (34010ops.hxx:506-609).
    void InterruptEnable(bool enabled, uint32_t next) {
        unsigned cost = 3;
        if (enabled) { s.status |= VideoTiming::kStatusIE; CheckInterrupt(next, cost); } else s.status &= ~VideoTiming::kStatusIE;
        Finish(next, cost);
    }
    // SET_ST (tms34010.cpp:218-222) checks for interrupts: POPST, PUTST, RETI (34010ops.hxx:1845-1872).
    void SetStatus(uint32_t status, uint32_t next, unsigned cost) { s.status = status; CheckInterrupt(next, cost, false); Finish(next, cost); }
    void PutStatus(uint32_t status, uint32_t next) { SetStatus(status, next, 3); }
    void ReturnInterrupt() { const uint32_t status = Pop(); const uint32_t next = Pop() & ~15u; SetStatus(status, next, 11); }
    // TRAP n (34010ops.hxx:1895-1906): TRAP 0 pushes nothing; RESET_ST's check sees IE clear.
    void Trap(unsigned n, uint32_t next) {
        if (shadow_on) { shadow_fault = true; Finish(next, 0); return; }
        const uint32_t status = s.status, vector = 0xffffffe0u - (n << 5), handler = Read32(vector) & ~15u;
        if (n) { Push(next); Push(status); }
        s.status = 0x10;
        interrupts.push_back({s.cycles + 16, vector, handler, next, status, s.a[15]});
        Finish(handler, 16);
    }
    void SetField(uint32_t field, uint32_t next, bool field1 = false) {
        const unsigned shift = field1 ? 6 : 0;
        s.status = (s.status & ~(0x3fu << shift)) | (field << shift);
        Finish(next, 1 + unsigned(field1));
    }
    void Write32(uint32_t address, uint32_t value) {  // WFIELDMAC_32: unaligned = read both dwords, write both back.
        if (const uint32_t shift = address & 15) {
            const uint32_t aligned = address & ~15u;
            const uint32_t low = Read32(aligned) & (0xffffffffu >> (32 - shift));
            const uint32_t high = Read32(aligned + 32) & (0xffffffffu << shift);
            Write32(aligned, (value << shift) | low);
            Write32(aligned + 32, (value >> (32 - shift)) | high);
            return;
        }
        Write16(address, uint16_t(value)); Write16(address + 16, uint16_t(value >> 16));
    }
    uint32_t ReadField(uint32_t address, unsigned field) {
        const unsigned width = FieldWidth(field);
        if (width == 32) return Read32(address);
        const auto aligned = address & ~15u, shift = address & 15;
        uint32_t value = (shift + width > 16 ? Read32(aligned) : Read16(aligned)) >> shift;
        // RFIELDMAC_BIG (34010ops.h, widths 18-31): a field past the dword also reads the next word (MAME-measured, oracle).
        if (shift + width > 32) value |= uint32_t(Read16(aligned + 32)) << (32 - shift);
        value &= (uint32_t(1) << width) - 1;
        if ((s.status & (0x20u << (field * 6))) && (value & (uint32_t(1) << (width - 1)))) value |= ~((uint32_t(1) << width) - 1);
        return value;
    }
    void WriteField(uint32_t address, uint32_t value, unsigned field) {  // tms34010 WFIELDMAC: read-modify-write.
        const unsigned width = FieldWidth(field);
        if (width == 32) { Write32(address, value); return; }
        if (width == 16 && !(address & 15)) { Write16(address, uint16_t(value)); return; }
        const uint32_t aligned = address & ~15u, shift = address & 15;
        if (shift + width > 16) {
            const uint32_t mask = ((uint32_t(1) << width) - 1) << shift;
            Write32(aligned, (Read32(aligned) & ~mask) | ((value << shift) & mask));
            // Widths 18-31 past the dword (D10: the chip, not MAME's WFIELDMAC_BIG, which writes this word to `offset`).
            if (shift + width > 32) {
                const uint16_t high = uint16_t((1u << (shift + width - 32)) - 1);
                Write16(aligned + 32, uint16_t((Read16(aligned + 32) & uint16_t(~high)) | ((value >> (32 - shift)) & high)));
            }
            return;
        }
        const uint16_t mask = uint16_t(((uint32_t(1) << width) - 1) << shift);
        Write16(aligned, uint16_t((Read16(aligned) & uint16_t(~mask)) | ((value << shift) & mask)));
    }
    void WriteByte(uint32_t address, uint32_t value) {  // tms34010 WFIELDMAC_8: read-modify-write of the whole word(s).
        const uint32_t aligned = address & ~15u, shift = address & 15;
        if (shift > 8) { const auto old = Read32(aligned), mask = 0xffu << shift; Write32(aligned, (old & ~mask) | ((value << shift) & mask)); return; }
        const uint16_t mask = uint16_t(0xffu << shift);
        Write16(aligned, uint16_t((Read16(aligned) & uint16_t(~mask)) | ((value & 0xff) << shift)));
    }
    void Push(uint32_t value) { s.a[15] -= 32; Write32(s.a[15], value); }
    uint32_t Pop() { const auto value = Read32(s.a[15]); s.a[15] += 32; return value; }
    void Call(uint32_t target, uint32_t return_pc, unsigned cost) { Push(return_pc); Finish(target & ~15u, cost); }
    void ReturnSubroutine(unsigned discard = 0) { const auto target = Pop() & ~15u; s.a[15] += discard << 4; Finish(target, 7); }
    void PushStatus(uint32_t next) { Push(s.status); Finish(next, 2); }
    void PopStatus(uint32_t next) { const uint32_t status = Pop(); SetStatus(status, next, 8); }
    void Mmtm(uint16_t mask, uint32_t next, bool bank_b = false, unsigned base = 15) {
        unsigned cost = 2;
        for (unsigned reg = 0; reg < 16; ++reg)
            if (mask & (0x8000u >> reg)) {
                auto& address = bank_b && base != 15 ? s.b[base] : s.a[base];
                address -= 32; elapsed = cost; Write32(address, reg == 15 ? s.a[15] : bank_b ? s.b[reg] : s.a[reg]); cost += 4;
            }
        elapsed = 0; Finish(next, cost);
    }
    void Mmfm(uint16_t mask, uint32_t next, bool bank_b = false, unsigned base = 15) {
        unsigned cost = 3;
        for (int reg = 15; reg >= 0; --reg)
            if (mask & (0x8000u >> reg)) {
                auto& address = bank_b && base != 15 ? s.b[base] : s.a[base];
                elapsed = cost; const auto value = Read32(address);
                (reg == 15 ? s.a[15] : bank_b ? s.b[reg] : s.a[reg]) = value;
                address += 32; cost += 4;
            }
        elapsed = 0; Finish(next, cost);
    }
};
std::vector<uint16_t> CmosWords(const std::vector<uint8_t>& cmos_image); // MAME nvram image: 24576 LE words.
void StepGenerated(Ops& o);                                               // one instruction at s.pc (translated or runtime helper)
bool StepTranslated(Ops& o);  // generated dispatcher (tools/mk3/generate.py units): false outside the static candidate
ResetTrace PowerOn(const Program& program);   // machine reset of the verified program (mk3_cmos_check.cpp)
// The generated code from the machine reset up to `stop`, without the DCS co-simulation.
ResetTrace RunGenerated(const Program& program, const std::vector<uint8_t>& cmos_image, uint32_t stop);
// Lot 3: CMOS routine 0xffadaf40 from the reset to its return at 0xff814b40.
ResetTrace RunCmosCheck(const Program& program, const std::vector<uint8_t>& cmos_image);
// Lot 4: watchdog write, PIC handshake 0xffb09110 and the JA to the main init, up to the first DMA write 0xffaeff00.
ResetTrace RunPicCheck(const Program& program, const std::vector<uint8_t>& cmos_image);
// Lot 5a: DMA latches, second reset path, RAM clear, TMS I/O table, video/palette/VRAM setup, up to PIXBLT 0xffae8690.
ResetTrace RunInitToText(const Program& program, const std::vector<uint8_t>& cmos_image);
// Lots 5b-5c: boot text (PIXBLT B,XY), PIC serial number clock-out and display setup, up to the first DCS wait 0xffaa8990.
ResetTrace RunToSoundWait(const Program& program, const std::vector<uint8_t>& cmos_image);
ResetTrace RunToSerial(const Program& program, const std::vector<uint8_t>& cmos_image);
}
