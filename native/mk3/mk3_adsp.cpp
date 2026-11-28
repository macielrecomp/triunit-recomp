#include "mk3_adsp.hpp"
#include "mk2_state_io.hpp"
#include "mk3_dcs.hpp"
#include "mk5_program.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace mk3 {
namespace {
constexpr uint16_t CFLAG = 0x08, VFLAG = 0x04, NFLAG = 0x02, ZFLAG = 0x01, SFLAG = 0x10, MVFLAG = 0x40;
constexpr uint16_t MSTAT_BANK = 0x01, MSTAT_REVERSE = 0x02, MSTAT_STICKYV = 0x04, MSTAT_SATURATE = 0x08, MSTAT_INTEGER = 0x10, MSTAT_TIMER = 0x20;
constexpr uint16_t SSFLAG = 0x80, QFLAG = 0x20;   // adsp2100.h
constexpr unsigned IRQ1 = 1, IRQ2 = 2;                            // adsp2100.h ADSP2105_IRQ1 / ADSP2105_IRQ2
constexpr uint16_t kInputEmpty = 0x800, kOutputEmpty = 0x400;   // dcs.cpp LCTRL_*
constexpr uint64_t kCycleNs = 100;                                // XTAL(10'000'000), 1 cycle per clock (diexec default)
int32_t Sext(uint32_t v, unsigned bits) { const uint32_t m = 1u << (bits - 1); v &= (m << 1) - 1; return int32_t(v ^ m) - int32_t(m); }
int32_t Asr(int32_t v, int n) { return v < 0 ? int32_t(~(~uint32_t(v) >> n)) : int32_t(uint32_t(v) >> n); }
uint16_t Reverse14(uint32_t i) {   // adsp21xx_device::create_tables m_reverse_table
    uint16_t r = 0;
    for (unsigned b = 0; b < 14; ++b) if (i >> b & 1) r |= uint16_t(1u << (13 - b));
    return r;
}
uint16_t MaskFor(uint32_t l) {   // adsp21xx_device::device_start m_mask_table: clear the bits below the next power of two
    uint32_t p = 1;
    while (p < l) p <<= 1;
    return uint16_t(0x3fff & ~(p - 1));
}
}

Dcs8k::Dcs8k(const std::vector<uint8_t>& u2) : u2_(u2) {
    if (u2.size() != 0x100000 || Sha256(u2) != kDcsU2Sha256) throw std::runtime_error("MK3 DCS requires the authenticated U2");
    for (unsigned n = 0; n < 8; ++n) UpdateL(n);
    // device_start dcs_reset, then machine reset: device resets and midwunit machine_reset reset_w(0)/reset_w(1),
    // whose queued RESET assert+clear and dcs_reset all fire at t = 0 before any execution.
    DcsReset(0);
    CoreReset();
    boot = 0;
    core_resets = {{0, 0, 0}};
}

void Dcs8k::Reject(const std::string& what) const {
    throw std::runtime_error("MK3 ADSP " + what + " (boot " + std::to_string(boot) + ", PC " + std::to_string(ppc_) +
                             ", cycle " + std::to_string(totalcycles) + ")");
}

void Dcs8k::SetLatch(uint16_t value, uint64_t ns) {
    if (value != latch_control) latch_changes.push_back({ns, value});
    latch_control = value;
}

uint16_t Dcs8k::LatchAt(uint64_t ns) const {
    uint16_t value = latch_changes.front().second;
    for (const auto& [time, v] : latch_changes) if (time < ns) value = v;
    return value;
}

void Dcs8k::Boot() {   // dcs_boot (REV_DCS1): 16-bit region words truncated to bytes = U2 bytes of the bank page
    if (sounddata_bank >= 0x100) Reject("boot bank outside U2");
    const size_t base = size_t(sounddata_bank) * 0x1000, count = (size_t(u2_[base + 3]) + 1) * 8;
    if (count > pm_.size()) Reject("boot page exceeds internal PM");
    for (size_t n = 0; n < count; ++n)
        pm_[n] = uint32_t(u2_[base + 4*n]) << 16 | uint32_t(u2_[base + 4*n + 1]) << 8 | u2_[base + 4*n + 2];
    page_ = sounddata_bank;
}

void Dcs8k::DcsReset(uint64_t ns) {
    // m_size/m_incs/m_ireg are zeroed but m_reg_timer, its period, the DMADAC enable/rate and the IRQ1 pulse-end
    // timer keep running: later dcs_irq calls transfer no sample, rewind I0 to its base and pulse IRQ1.
    dac_size_ = dac_ireg_ = 0; dac_incs_ = 0;
    sounddata_bank = 0;
    control_.fill(0);
    for (unsigned n = 0; n <= IRQ2; ++n) irq_state_[n] = 0;   // IRQ0..2 CLEAR_LINE (latches untouched)
    SetLatch(latch_control | kInputEmpty | kOutputEmpty, ns);
    Boot();
    // reset timers (m_timer_start_count and m_timer_period are kept)
    timer_ignore_ = false; timer_enable_ = 0; timer_scale_ = 1; int_next_ns_ = kNever;
}

void Dcs8k::CoreReset() {
    totalcycles = 0;   // diexec interface_pre_reset
    for (unsigned n = 0; n < 8; ++n) { UpdateL(n); UpdateI(n); }
    pc = ppc_ = 0;
    loop_ = 0xffff; loop_condition_ = 0;
    astat_clear_ = uint16_t(~(CFLAG | VFLAG | NFLAG | ZFLAG));
    mstat_ = 0; sstat_ = 0x55;
    UpdateMstat();
    pc_sp_ = cntr_sp_ = stat_sp_ = loop_sp_ = 0;
    flagout_ = flagin_ = fl0_ = fl1_ = fl2_ = 0;
    imask_ = 0;
    irq_state_.fill(0); irq_latch_.fill(0);
    suspended = false;
    ++boot;
    core_resets.push_back({local_ns, boot, instructions});
}

void Dcs8k::UpdateL(unsigned n) { lmask_[n] = MaskFor(l_[n] & 0x3fff); base_[n] = i_[n] & lmask_[n]; }

void Dcs8k::UpdateMstat() {
    if ((mstat_ ^ mstat_prev_) & MSTAT_BANK) std::swap(core_, alt_);
    if ((mstat_ ^ mstat_prev_) & MSTAT_TIMER) TimerEnable(mstat_ & MSTAT_TIMER);   // m_timer_fired_cb
    astat_clear_ = uint16_t(mstat_ & MSTAT_STICKYV ? ~(CFLAG | NFLAG | ZFLAG) : ~(CFLAG | VFLAG | NFLAG | ZFLAG));
    mstat_prev_ = mstat_;
}

bool Dcs8k::Condition(unsigned c) {   // m_condition_table / slow_condition
    if (c == 14) {
        if (int32_t(--cntr_) > 0) return true;
        CntrPop();
        return false;
    }
    const bool az = astat_ & ZFLAG, an = astat_ & NFLAG, av = astat_ & VFLAG, ac = astat_ & CFLAG;
    const bool as = astat_ & SFLAG, mv = astat_ & MVFLAG;
    switch (c) {
    case 0: return az;  case 1: return !az; case 2: return !((an ^ av) | az); case 3: return (an ^ av) | az;
    case 4: return an ^ av; case 5: return !(an ^ av); case 6: return av; case 7: return !av;
    case 8: return ac; case 9: return !ac; case 10: return as; case 11: return !as;
    case 12: return mv; case 13: return !mv; default: return true;
    }
}

void Dcs8k::PcPush() { if (pc_sp_ < 16) { pc_stack_[pc_sp_++] = pc; sstat_ &= ~1; } else sstat_ |= 2; }
uint32_t Dcs8k::PcPopVal() { if (pc_sp_ > 0 && --pc_sp_ == 0) sstat_ |= 1; return pc_stack_[pc_sp_]; }
void Dcs8k::PcPop() { pc = uint16_t(PcPopVal()); }
void Dcs8k::CntrPush() { if (cntr_sp_ < 4) { cntr_stack_[cntr_sp_++] = cntr_; sstat_ &= ~4; } else sstat_ |= 8; }
void Dcs8k::CntrPop() { if (cntr_sp_ > 0 && --cntr_sp_ == 0) sstat_ |= 4; cntr_ = cntr_stack_[cntr_sp_]; }
void Dcs8k::LoopPush(uint32_t value) {
    if (loop_sp_ < 4) { loop_stack_[loop_sp_++] = value; loop_ = value >> 4; loop_condition_ = value & 15; sstat_ &= ~0x40; }
    else sstat_ |= 0x80;
}
void Dcs8k::LoopPop() {
    if (!loop_sp_) return;
    if (--loop_sp_ == 0) { loop_ = 0xffff; loop_condition_ = 0; sstat_ |= 0x40; }
    else { loop_ = loop_stack_[loop_sp_ - 1] >> 4; loop_condition_ = loop_stack_[loop_sp_ - 1] & 15; }
}
void Dcs8k::StatPush() {
    if (stat_sp_ < 4) { stat_stack_[stat_sp_++] = {mstat_, imask_, astat_}; sstat_ &= ~0x10; } else sstat_ |= 0x20;
}
void Dcs8k::StatPop() {
    if (stat_sp_ > 0 && --stat_sp_ == 0) sstat_ |= 0x10;
    mstat_ = stat_stack_[stat_sp_][0]; UpdateMstat();
    imask_ = stat_stack_[stat_sp_][1]; astat_ = stat_stack_[stat_sp_][2];
    CheckIrqs();
}

void Dcs8k::SetIrq(unsigned line, bool state) {
    if (state && !irq_state_[line]) irq_latch_[line] = 1;
    irq_state_[line] = state;
}

void Dcs8k::CheckIrqs() {   // adsp2101_device::check_irqs / generate_irq (lines 2 IRQ2, 4 SPORT0 TX, 3 SPORT0 RX, 1 IRQ1, 0 IRQ0, 5 timer)
    auto take = [&](unsigned which, unsigned indx) {
        if (!(imask_ & (0x20 >> indx))) return false;
        irq_latch_[which] = 0;
        PcPush(); StatPush();
        pc = uint16_t(0x04 + indx * 4);
        imask_ &= (icntl_ & 0x10) ? ~(0x3f >> indx) : ~0x3f;
        return true;
    };
    if (((icntl_ & 4) ? irq_latch_[2] : irq_state_[2]) && take(2, 0)) return;
    if (irq_latch_[4] && take(4, 1)) return;
    if (irq_latch_[3] && take(3, 2)) return;
    if (((icntl_ & 2) ? irq_latch_[1] : irq_state_[1]) && take(1, 3)) return;
    if (((icntl_ & 1) ? irq_latch_[0] : irq_state_[0]) && take(0, 4)) return;
    if (irq_latch_[5]) take(5, 5);
}

uint32_t Dcs8k::Opcode() const {
    if (pc < 0x400) return pm_[pc];
    if (pc >= 0x0800 && pc < 0x2000) return external_pm_[pc - 0x0800];
    Reject("PC outside the mapped program memory");
}

void Dcs8k::Fetch(uint32_t expected) {
    if (Opcode() != expected) Reject("opcode differs from the translated block");
    if (pc != loop_) ++pc;
    else if (Condition(loop_condition_)) pc = uint16_t(pc_sp_ ? pc_stack_[pc_sp_ - 1] : pc_stack_[0]);
    else { LoopPop(); PcPopVal(); ++pc; }
}

void Dcs8k::WriteReg0(unsigned r, int32_t v) {
    auto& c = core_;
    switch (r) {
    case 0x0: c.ax0 = uint16_t(v); break; case 0x1: c.ax1 = uint16_t(v); break;
    case 0x2: c.mx0 = uint16_t(v); break; case 0x3: c.mx1 = uint16_t(v); break;
    case 0x4: c.ay0 = uint16_t(v); break; case 0x5: c.ay1 = uint16_t(v); break;
    case 0x6: c.my0 = uint16_t(v); break; case 0x7: c.my1 = uint16_t(v); break;
    case 0x8: c.si = uint16_t(v); break;  case 0x9: c.se = uint16_t(int16_t(int8_t(v))); break;
    case 0xa: c.ar = uint16_t(v); break;  case 0xb: c.mr0 = uint16_t(v); break;
    case 0xc: c.mr1 = uint16_t(v); c.mr2 = uint16_t(int16_t(int16_t(v) >> 15)); break;
    case 0xd: c.mr2 = uint16_t(int16_t(int8_t(v))); break;
    case 0xe: c.sr0 = uint16_t(v); break; default: c.sr1 = uint16_t(v); break;
    }
}
int32_t Dcs8k::ReadReg0(unsigned r) const {
    const auto& c = core_;
    const uint16_t v[16] = {c.ax0, c.ax1, c.mx0, c.mx1, c.ay0, c.ay1, c.my0, c.my1, c.si, c.se, c.ar, c.mr0, c.mr1, c.mr2, c.sr0, c.sr1};
    return int16_t(v[r & 15]);
}
void Dcs8k::WriteReg1(unsigned r, int32_t v) {
    const unsigned n = r & 3;
    switch (r >> 2) {
    case 0: i_[n] = uint32_t(v) & 0x3fff; UpdateI(n); break;
    case 1: m_[n] = Sext(uint32_t(v), 14); break;
    case 2: l_[n] = uint32_t(v) & 0x3fff; UpdateL(n); break;
    default: Reject("invalid register group 1 write");
    }
}
void Dcs8k::WriteReg2(unsigned r, int32_t v) {
    const unsigned n = 4 + (r & 3);
    switch (r >> 2) {
    case 0: i_[n] = uint32_t(v) & 0x3fff; UpdateI(n); break;
    case 1: m_[n] = Sext(uint32_t(v), 14); break;
    case 2: l_[n] = uint32_t(v) & 0x3fff; UpdateL(n); break;
    default: Reject("invalid register group 2 write");
    }
}
int32_t Dcs8k::ReadReg1(unsigned r) const {
    const unsigned n = r & 3;
    return (r >> 2) == 0 ? int32_t(i_[n]) : (r >> 2) == 1 ? m_[n] : int32_t(l_[n]);   // 12..15 alias L (m_read1_ptr)
}
int32_t Dcs8k::ReadReg2(unsigned r) const {
    const unsigned n = 4 + (r & 3);
    return (r >> 2) == 0 ? int32_t(i_[n]) : (r >> 2) == 1 ? m_[n] : int32_t(l_[n]);
}
void Dcs8k::WriteReg3(unsigned r, int32_t v) {
    switch (r) {
    case 0x0: astat_ = uint16_t(v & 0xff); break;
    case 0x1: mstat_ = uint16_t(v & 0x7f); UpdateMstat(); break;
    case 0x3: imask_ = uint16_t(v & 0x3f); CheckIrqs(); break;
    case 0x4: icntl_ = uint16_t(v & 0x1f); CheckIrqs(); break;
    case 0x5: CntrPush(); cntr_ = uint32_t(v) & 0x3fff; break;
    case 0x6: core_.sb = uint16_t(Sext(uint32_t(v), 5)); break;
    case 0x7: px_ = uint8_t(v); break;
    case 0xb: SportTx1(); break;   // TX1: m_sport_tx_cb(1, val); the DCS uses the write only as the autobuffer start
    case 0xc:                      // IFC (ADSP-2101 family): clear / force the edge latches
        for (unsigned bit = 1; bit <= 5; ++bit) {   // bits 1-5 clear, 7-11 force: IRQ0, IRQ1, SPORT0 RX, SPORT0 TX, IRQ2
            static constexpr unsigned kLine[6] = {0, 0, 1, 3, 4, 2};
            if (v >> bit & 1) irq_latch_[kLine[bit]] = 0;
            if (v >> (bit + 6) & 1) irq_latch_[kLine[bit]] = 1;
        }
        CheckIrqs();
        break;
    case 0xd: cntr_ = uint32_t(v) & 0x3fff; break;
    case 0xf: if (pc_sp_ < 16) { pc_stack_[pc_sp_++] = uint32_t(v) & 0x3fff; sstat_ &= ~1; } else sstat_ |= 2; break;
    default: Reject("register group 3 write outside the modeled devices");
    }
}
int32_t Dcs8k::ReadReg3(unsigned r) {
    switch (r) {
    case 0x0: return astat_; case 0x1: return mstat_; case 0x2: return sstat_; case 0x3: return imask_;
    case 0x4: return icntl_; case 0x5: return int32_t(cntr_); case 0x6: return int16_t(core_.sb); case 0x7: return px_;
    case 0xf: return int32_t(PcPopVal());
    default: Reject("register group 3 read outside the modeled devices");
    }
}

uint16_t Dcs8k::DataRead(uint16_t a) {
    uint16_t v;
    if (a < 0x0800) v = dm_[a];
    else if (a < 0x2000) v = uint16_t(external_pm_[a - 0x0800] >> 8);   // dcs_dataram_r
    else if (a < 0x3000) { if (!DataByte(a)) Reject("data bank outside U2"); v = DebugData(a); }
    else if (a >= 0x3400 && a < 0x3404) {   // input_latch_r with m_auto_ack: ack, IRQ2 CLEAR (synchronized)
        SetLatch(latch_control | kInputEmpty, local_ns);
        irq_state_[IRQ2] = 0;
        sync_ = true;
        v = input_data;
    } else if (a >= 0x3800 && a < 0x3a00) v = internal_dm_[a - 0x3800];
    else if (a >= 0x3ffb && a <= 0x3ffd) {   // adsp_control_r: TIMER_COUNT_REG updates the count first
        TimerPath(3, a - 0x3fe0u);
        if (a == 0x3ffc) UpdateTimerCount();
        v = control_[a - 0x3fe0];
    }
    else if (a >= 0x3fe0 && a != 0x3fe0 && a != 0x3fe5) v = control_[a - 0x3fe0];
    else Reject("data read outside the modeled DCS 8K map");
    Log('r', ppc_, totalcycles, a, v);
    return v;
}

void Dcs8k::DataWrite(uint16_t a, uint16_t v) {
    Log('w', ppc_, totalcycles, a, v);
    if (a < 0x0800) dm_[a] = v;
    else if (a < 0x2000) external_pm_[a - 0x0800] = uint32_t(v) << 8 | (external_pm_[a - 0x0800] & 0xff);   // dcs_dataram_w
    else if (a == 0x3000) sounddata_bank = v & 0x7ff;                    // dcs_data_bank_select_w (bit 11 = LED)
    else if (a >= 0x3400 && a < 0x3404) {   // output_latch_w: synchronized latch_delayed_w sets output full
        if (local_ns >= read_slice_min_ns_ && local_ns < read_slice_max_ns_) Reject("output write at an unknown slice boundary of a host read");
        sync_ = true;
        output_data = v;
        SetLatch(latch_control & ~kOutputEmpty, local_ns);
    } else if (a >= 0x3800 && a < 0x3a00) internal_dm_[a - 0x3800] = v;
    else if (a >= 0x3fe0) {                                               // adsp_control_w
        const unsigned offset = a - 0x3fe0;
        if (offset >= 0x1b && offset <= 0x1d) TimerPath(2, offset);
        control_[offset] = v;
        if (offset == 0x1b) {   // TIMER_SCALE_REG
            const uint32_t data = (v & 0xffu) + 1;
            if (data != timer_scale_) { UpdateTimerCount(); timer_scale_ = data; ResetTimer(); }
        } else if (offset == 0x1c) { timer_start_count_ = v; ResetTimer(); }                                  // TIMER_COUNT_REG
        else if (offset == 0x1d && v != timer_period_) { UpdateTimerCount(); timer_period_ = v; ResetTimer(); }   // TIMER_PERIOD_REG
        if (offset == 0x1f && (v & 0x0200)) {   // pulse_input_line(RESET) (synchronized) + dcs_boot from the current bank
            sync_ = reboot_ = true;
            Boot();
            control_[offset] = 0;
        }
        // SPORT1 disabled (tested on the written value) or autobuffer off: DAC off, m_reg_timer reset
        if ((offset == 0x1f && !(v & 0x0800)) || (offset == 0x0f && !(v & 2))) StopDac();
    } else Reject("data write outside the modeled DCS 8K map");
}

uint32_t Dcs8k::ProgramRead(uint16_t a) {
    const uint32_t v = DebugProgram(a);
    Log('q', ppc_, totalcycles, a, v);
    return v;
}
void Dcs8k::ProgramWrite(uint16_t a, uint32_t v) {
    v &= 0xffffff;
    Log('p', ppc_, totalcycles, a, v);
    if (a < 0x400) pm_[a] = v;
    else if (a >= 0x0800 && a < 0x2000) external_pm_[a - 0x0800] = v;
    else Reject("program write outside the modeled DCS 8K map");
}

const uint8_t* Dcs8k::DataByte(uint16_t a) const {   // set_entry(m_sounddata_bank % m_sounddata_banks), 0x400 banks of 0x1000 words
    const size_t bank = sounddata_bank % 0x400, offset = (bank & 0xff) * 0x1000 + (a - 0x2000);
    if (bank < 0x100) return &u2_.at(offset);
    return sound_data_.empty() ? nullptr : &sound_data_.at(((bank >> 8) - 1) * 0x100000 + offset);
}

void Dcs8k::AttachSoundData(const std::filesystem::path& directory) {
    static constexpr const char* kRoms[][2] = {
        {"l1_mortal_kombat_3_u3_music_spch.u3", "400ced5872282ce764a36126425af463db512aeb067825d6ce58b1fad6bb3d8f"},
        {"l1_mortal_kombat_3_u4_music_spch.u4", "ab37271fcf97899ed23a4d38f0b02faf959420228e9b48331aeeedc56b1db500"},
        {"l1_mortal_kombat_3_u5_music_spch.u5", "e310a81231c5de8a152bf3d36addd347b113869920801d258a87535e6eae8ada"}};
    std::vector<uint8_t> data;
    for (const auto& rom : kRoms) {
        std::error_code error;
        const auto path = directory / rom[0];
        if (std::filesystem::file_size(path, error) != 0x100000 || error) throw std::runtime_error("MK3 DCS sound-data ROM missing or wrong size");
        std::ifstream input(path, std::ios::binary);
        std::vector<uint8_t> bytes(0x100000);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || Sha256(bytes) != rom[1])
            throw std::runtime_error("MK3 DCS sound-data ROM read failed or SHA-256 mismatch");
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    sound_data_ = std::move(data);
}

uint16_t Dcs8k::DebugData(uint16_t a) const {
    if (a < 0x0800) return dm_[a];
    if (a < 0x2000) return uint16_t(external_pm_[a - 0x0800] >> 8);
    // dcs region: ROM_REGION16_LE ERASEFF, U2 ROM_LOAD16_BYTE at 0 -> word = 0xff00 | U2 byte.
    if (a < 0x3000) { const uint8_t* b = DataByte(a); return b ? uint16_t(0xff00 | *b) : 0xffff; }
    if (a >= 0x3800 && a < 0x3a00) return internal_dm_[a - 0x3800];
    if (a == 0x3fe0) return 0xffff;                                   // adsp_control_r: IDMA only on DSIO/Denver
    if (a == 0x3fe5) return uint16_t(control_[6] & control_[5]);      // PROG_FLAG_DATA with m_progflags = 0
    if (a > 0x3fe0) return control_[a - 0x3fe0];
    throw std::runtime_error("MK3 ADSP debug read of an unmapped data address");
}
uint32_t Dcs8k::DebugProgram(uint16_t a) const {
    if (a < 0x400) return pm_[a];
    if (a >= 0x0800 && a < 0x2000) return external_pm_[a - 0x0800];
    throw std::runtime_error("MK3 ADSP program access outside the modeled DCS 8K map");
}

void Dcs8k::Modify(unsigned n, unsigned mreg) {
    uint32_t i = (i_[n] + uint32_t(m_[mreg])) & 0x3fff;
    if (i < base_[n]) i += l_[n];
    else if (i >= base_[n] + l_[n]) i -= l_[n];
    i_[n] = i;
}
uint16_t Dcs8k::DataReadDag1(uint32_t op) {
    const unsigned n = (op >> 2) & 3;
    const uint16_t v = DataRead((mstat_ & MSTAT_REVERSE) ? Reverse14(i_[n]) : uint16_t(i_[n])); Modify(n, op & 3); return v;
}
void Dcs8k::DataWriteDag1(uint32_t op, int32_t v) {
    const unsigned n = (op >> 2) & 3;
    DataWrite((mstat_ & MSTAT_REVERSE) ? Reverse14(i_[n]) : uint16_t(i_[n]), uint16_t(v)); Modify(n, op & 3);
}
uint16_t Dcs8k::DataReadDag2(uint32_t op) {
    const unsigned n = 4 + ((op >> 2) & 3);
    const uint16_t v = DataRead(uint16_t(i_[n])); Modify(n, 4 + (op & 3)); return v;
}
void Dcs8k::DataWriteDag2(uint32_t op, int32_t v) {
    const unsigned n = 4 + ((op >> 2) & 3);
    DataWrite(uint16_t(i_[n]), uint16_t(v)); Modify(n, 4 + (op & 3));
}
uint16_t Dcs8k::PgmReadDag2(uint32_t op) {
    const unsigned n = 4 + ((op >> 2) & 3);
    const uint32_t v = ProgramRead(uint16_t(i_[n]));
    px_ = uint8_t(v);
    Modify(n, 4 + (op & 3));
    return uint16_t(v >> 8);
}
void Dcs8k::PgmWriteDag2(uint32_t op, int32_t v) {
    const unsigned n = 4 + ((op >> 2) & 3);
    ProgramWrite(uint16_t(i_[n]), (uint32_t(v) << 8) | px_);
    Modify(n, 4 + (op & 3));
}

void Dcs8k::Alu(bool af, unsigned fn, unsigned x, unsigned y) {   // alu_op_ar / alu_op_af
    const auto& c = core_;
    const uint16_t xs[8] = {c.ax0, c.ax1, c.ar, c.mr0, c.mr1, c.mr2, c.sr0, c.sr1}, ys[4] = {c.ay0, c.ay1, c.af, 0};
    const int32_t xop = xs[x & 7], yop = ys[y & 3], carry = (astat_ & CFLAG) >> 3;
    int32_t res = 0;
    // CALC_NZ / CALC_V / CALC_C / CALC_C_SUB (bits 15, 16 of the 32-bit result)
    auto nz = [&] { astat_ &= astat_clear_; astat_ |= (uint32_t(res) >> 14) & NFLAG; astat_ |= (res & 0xffff) == 0; };
    auto nzv = [&](int32_t s, int32_t d) { nz(); astat_ |= ((uint32_t(s) ^ uint32_t(d) ^ uint32_t(res) ^ (uint32_t(res) >> 1)) >> 13) & VFLAG; };
    auto add = [&](int32_t s, int32_t d) { res = s + d; nzv(s, d); astat_ |= (uint32_t(res) >> 13) & CFLAG; };
    auto sub = [&](int32_t s, int32_t d, int32_t borrow) { res = s - d + borrow; nzv(s, d); astat_ |= (~uint32_t(res) >> 13) & CFLAG; };
    switch (fn) {
    case 0x0: res = yop; nz(); break;
    case 0x1: res = yop + 1; nz(); if (yop == 0x7fff) astat_ |= VFLAG; else if (yop == 0xffff) astat_ |= CFLAG; break;
    case 0x2: add(xop, yop + carry); break;
    case 0x3: add(xop, yop); break;
    case 0x4: res = yop ^ 0xffff; nz(); break;
    case 0x5: res = -yop; nz(); if (yop == 0x8000) astat_ |= VFLAG; if (yop == 0) astat_ |= CFLAG; break;
    case 0x6: sub(xop, yop, carry - 1); break;
    case 0x7: sub(xop, yop, 0); break;
    case 0x8: res = yop - 1; nz(); if (yop == 0x8000) astat_ |= VFLAG; else if (yop == 0) astat_ |= CFLAG; break;
    case 0x9: sub(yop, xop, 0); break;
    case 0xa: sub(yop, xop, carry - 1); break;
    case 0xb: res = xop ^ 0xffff; nz(); break;
    case 0xc: res = xop & yop; nz(); break;
    case 0xd: res = xop | yop; nz(); break;
    case 0xe: res = xop ^ yop; nz(); break;
    default:   // ABS X
        res = (xop & 0x8000) ? -xop : xop;
        astat_ &= astat_clear_; astat_ &= ~SFLAG;
        if (xop == 0) astat_ |= ZFLAG;
        if (xop == 0x8000) astat_ |= NFLAG | VFLAG;
        if (xop & 0x8000) astat_ |= SFLAG;
    }
    if (af) { core_.af = uint16_t(res); return; }
    if ((mstat_ & MSTAT_SATURATE) && (astat_ & VFLAG)) res = (astat_ & CFLAG) ? -32768 : 32767;
    core_.ar = uint16_t(res);
}

void Dcs8k::Mac(bool mf, unsigned fn, unsigned x, unsigned y) {   // mac_op_mr / mac_op_mf, fn != 0
    auto& c = core_;
    const uint16_t xs[8] = {c.mx0, c.mx1, c.ar, c.mr0, c.mr1, c.mr2, c.sr0, c.sr1}, ys[4] = {c.my0, c.my1, c.mf, 0};
    const unsigned sign = fn < 4 ? 0 : fn & 3;         // 0 SS (and RND), 1 SU, 2 US, 3 UU
    const unsigned acc = fn < 4 ? fn - 1 : (fn >> 2) - 1;   // 0 product, 1 MR + product, 2 MR - product
    const int32_t xop = (sign & 2) ? int32_t(xs[x & 7]) : int16_t(xs[x & 7]);
    const int32_t yop = (sign & 1) ? int32_t(ys[y & 3]) : int16_t(ys[y & 3]);
    const int32_t temp = int32_t((uint32_t(xop) * uint32_t(yop)) << ((mstat_ & MSTAT_INTEGER) ? 0 : 1));
    // MR as its 48 architectural bits: the union's fourth word (mrzero) never reaches MR0-MR2, MV or MF.
    const int64_t mr = int64_t(uint64_t(c.mr0) | uint64_t(c.mr1) << 16 | uint64_t(c.mr2) << 32);
    uint64_t res = uint64_t(acc == 0 ? int64_t(temp) : acc == 1 ? mr + temp : mr - temp);
    if (fn < 4) {   // RND
        res += 0x8000;
        if ((temp & 0xffff) == 0x8000) res &= ~uint64_t(0x10000);
    }
    if (mf) { c.mf = uint16_t(uint32_t(res) >> 16); return; }
    const unsigned top = unsigned(res >> 31) & 0x1ff;
    astat_ &= ~MVFLAG;
    if (top != 0 && top != 0x1ff) astat_ |= MVFLAG;
    c.mr0 = uint16_t(res); c.mr1 = uint16_t(res >> 16); c.mr2 = uint16_t(res >> 32);
}

void Dcs8k::SatMr() {   // execute_run case 0x05
    if (!(astat_ & MVFLAG)) return;
    if (core_.mr2 & 0x80) core_.mr2 = 0xffff, core_.mr1 = 0x8000, core_.mr0 = 0x0000;
    else core_.mr2 = 0x0000, core_.mr1 = 0x7fff, core_.mr0 = 0xffff;
}

void Dcs8k::Shift(unsigned fn, unsigned x, int sc, bool imm) {   // shift_op / shift_op_imm (2100ops.hxx)
    auto& c = core_;
    const uint16_t xs[8] = {c.si, c.si, c.ar, c.mr0, c.mr1, c.mr2, c.sr0, c.sr1};
    const uint32_t u = xs[x & 7];
    const int32_t s = int16_t(xs[x & 7]);
    // std::countl_zero of the 32-bit operand (countl_one(v) = countl_zero(~v))
    auto clz = [](uint32_t v) { int n = 0; for (; n < 32 && !(v & 0x80000000u); v <<= 1) ++n; return n; };
    auto exp_hi = [&] {   // EXP (HI): res = leading sign bits of the sign-extended operand - 16 - 1, SE = -res
        const int res = s < 0 ? clz(~uint32_t(s)) - 17 : clz(uint32_t(s)) - 17;
        astat_ = uint16_t(s < 0 ? astat_ | SSFLAG : astat_ & ~SSFLAG);
        c.se = uint16_t(-res);
    };
    if (fn >= 12) {
        if (imm) return;   // shift_op_imm has no case for EXP / EXPADJ
        switch (fn) {
        case 12: exp_hi(); return;
        case 13:   // EXP (HIX)
            if (astat_ & VFLAG) { c.se = 1; astat_ = uint16_t(s < 0 ? astat_ & ~SSFLAG : astat_ | SSFLAG); }
            else exp_hi();
            return;
        case 14:   // EXP (LO): only when SE = -15
            if (int16_t(c.se) == -15) c.se = uint16_t(-(clz(uint32_t((astat_ & SSFLAG) ? ~s : s) & 0xffff) - 1));
            return;
        default: {   // EXPADJ
            const int res = clz(uint32_t(s < 0 ? ~s : s)) - 17;
            if (res < -int16_t(c.sb)) c.sb = uint16_t(-res);
            return;
        }
        }
    }
    uint32_t res;
    if (fn >= 8) {   // NORM
        if (!(fn & 2)) {   // NORM (HI)
            const uint32_t v = uint32_t(s) << 16;
            // x86 SAR/SHL mask the count to 5 bits; MAME compiles this int32 shift unguarded (assumption, sc - 1 > 31).
            if (sc > 0) res = uint32_t(Asr(int32_t((v >> 1) | uint32_t(astat_ & CFLAG) << 28), (sc - 1) & 31));
            else res = sc > -32 ? v << -sc : 0;
        } else {           // NORM (LO)
            if (sc > 0) res = sc < 32 ? u >> sc : 0;
            else res = sc > -32 ? u << -sc : 0;
        }
    } else {
        const bool hi = !(fn & 2), arithmetic = fn & 4;
        const uint32_t v = hi ? (arithmetic ? uint32_t(s) : u) << 16 : (arithmetic ? uint32_t(s) : u);
        if (sc > 0) res = sc < 32 ? v << sc : 0;
        else if (arithmetic) res = uint32_t(Asr(int32_t(v), sc > -32 ? -sc : 31));
        else res = sc > -32 ? v >> -sc : 0;
    }
    const uint32_t sr = (fn & 1) ? (uint32_t(c.sr1) << 16 | c.sr0) | res : res;
    c.sr0 = uint16_t(sr); c.sr1 = uint16_t(sr >> 16);
}

void Dcs8k::Divs(unsigned x, unsigned y) {   // execute_run case 0x06 (DIVS)
    auto& c = core_;
    const uint16_t xs[8] = {c.ax0, c.ax1, c.ar, c.mr0, c.mr1, c.mr2, c.sr0, c.sr1}, ys[4] = {c.ay0, c.ay1, c.af, 0};
    const uint32_t xop = xs[x & 7], yop = ys[y & 3], temp = xop ^ yop;
    astat_ = uint16_t((astat_ & ~QFLAG) | ((temp >> 10) & QFLAG));
    c.af = uint16_t((yop << 1) | (c.ay0 >> 15));
    c.ay0 = uint16_t((uint32_t(c.ay0) << 1) | (temp >> 15));
}

void Dcs8k::Divq(unsigned x) {   // execute_run case 0x07 (DIVQ)
    auto& c = core_;
    const uint16_t xs[8] = {c.ax0, c.ax1, c.ar, c.mr0, c.mr1, c.mr2, c.sr0, c.sr1};
    const uint32_t xop = xs[x & 7];
    const uint32_t res = (astat_ & QFLAG) ? uint32_t(c.af) + xop : uint32_t(c.af) - xop, temp = res ^ xop;
    astat_ = uint16_t((astat_ & ~QFLAG) | ((temp >> 10) & QFLAG));
    c.af = uint16_t((res << 1) | (c.ay0 >> 15));
    c.ay0 = uint16_t((uint32_t(c.ay0) << 1) | ((~temp >> 15) & 1));
}

void Dcs8k::FlagOut(uint32_t op) {   // execute_run case 0x02, modify flag out (ADSP-2101 family: FL0-FL2 too)
    if (op & 0x20) flagout_ = 0;
    if (op & 0x10) flagout_ ^= 1;
    if (op & 0x80) fl0_ = 0;
    if (op & 0x40) fl0_ ^= 1;
    if (op & 0x200) fl1_ = 0;
    if (op & 0x100) fl1_ ^= 1;
    if (op & 0x800) fl2_ = 0;
    if (op & 0x400) fl2_ ^= 1;
}

void Dcs8k::Unverified(uint32_t op, bool measured_pc) {
    if (profile == Profile::Strict && !(Accepted(routes, mk3::Unverified::AdspBlock, uint32_t(page_) << 16 | ppc_, op) &&
                                        (last_unverified_ || Accepted(routes, mk3::Unverified::AdspEdge, ppc_, last_pc_))))
        Reject(measured_pc ? "opcode differs from the translated block" : "PC outside the fixed translation");
    unverified_ = true;
    if (!first) return;
    const uint64_t cycle = local_ns / kCycleNs;
    first->Note(mk3::Unverified::AdspBlock, uint32_t(page_) << 16 | ppc_, cycle, op);
    if (!last_unverified_) first->Note(mk3::Unverified::AdspEdge, ppc_, cycle, last_pc_);
}

void Dcs8k::Untranslated() const {
    char text[64];
    std::snprintf(text, sizeof text, "form 0x%06x is not translated", unsigned(Opcode()));
    Reject(text);
}

void Dcs8k::Log(char kind, uint16_t at, uint64_t cycle, uint16_t address, uint32_t value) {
    ++access_count_;
    const AdspAccess access{boot, cycle, at, kind, address, value};
    if (!observe_access || observe_access(*this, access)) accesses.push_back(access);
}

void Dcs8k::StopDac() { dac_enabled_ = false; dac_next_ns_ = kNever; }

void Dcs8k::SportTx1() {   // dcs.cpp sound_tx_callback(offset 1)
    if (!(control_[0x1f] & 0x0800) || !(control_[0x0f] & 2)) return StopDac();   // SPORT1 enable, autobuffer enable
    dac_ireg_ = (control_[0x0f] >> 9) & 7;
    dac_incs_ = m_[((control_[0x0f] >> 7) & 3) | (dac_ireg_ & 4)];
    dac_size_ = l_[dac_ireg_];
    i_[dac_ireg_] &= ~0xfu; UpdateI(dac_ireg_);
    // recompute_sample_rate, one channel: internal SCLK (S1_CONTROL bit 14) = 16 bits of 2 * (SCLKDIV + 1) clocks,
    // otherwise the external 31.25 kHz sample clock.
    dac_sample_ns_ = (control_[0x12] & 0x4000) ? uint32_t(kCycleNs * 2 * (control_[0x11] + 1u) * 16) : 32000;
    dac_enabled_ = true;
    if (!dac_incs_) return;   // the timer is left as it was
    const uint64_t num = uint64_t(dac_sample_ns_) * dac_size_, den = uint64_t(2) * uint32_t(dac_incs_);
    if (dac_incs_ < 0 || !num || num % den) Reject("autobuffer period is not a positive whole number of nanoseconds");
    dac_period_ns_ = num / den;
    ++dac_arms_;
    dac_next_ns_ = local_ns + dac_period_ns_;   // m_reg_timer->adjust(period, 0, period) at the writing instruction's start
}

void Dcs8k::DacTransfer() {   // dcs.cpp dcs_irq
    uint32_t reg = i_[dac_ireg_];
    const int32_t count = int32_t(dac_size_) / (2 * (dac_incs_ ? dac_incs_ : 1));
    if (count < 0 || count > 0x800) Reject("autobuffer transfer outside the dcs_irq buffer");
    const DacBlock block{dac_next_ns_, boot, totalcycles, uint16_t(reg), dac_incs_, uint32_t(pcm.size()), uint32_t(count), dac_sample_ns_, dac_arms_};
    for (int32_t n = 0; n < count; ++n, reg += uint32_t(dac_incs_)) {
        if (reg >= 0x3000 && !(reg >= 0x3800 && reg < 0x3a00)) Reject("autobuffer read outside data RAM/ROM");
        const uint16_t v = DebugData(uint16_t(reg));
        Log('d', ppc_, totalcycles ? totalcycles - 1 : 0, uint16_t(reg), v);
        if (dac_enabled_) pcm.push_back(int16_t(v));
    }
    if (dac_enabled_) dac_blocks.push_back(block);   // dmadac_transfer stores only while the DAC is enabled
    if (reg >= base_[dac_ireg_] + dac_size_) {       // wrap: pulse_input_line(IRQ1, minimum_quantum_time = 1 cycle)
        reg = base_[dac_ireg_];
        SetIrq(IRQ1, true);
        irq1_clear_ns_ = local_ns + kCycleNs;
    }
    i_[dac_ireg_] = reg & 0x3fff; UpdateI(dac_ireg_);
}

void Dcs8k::TimerPath(uint32_t n, uint32_t from) {
    if (profile == Profile::Strict && !Accepted(routes, mk3::Unverified::DevicePath, kTimerPath | n, from))
        Reject("internal timer path not compared with the Original");
    if (first) first->Note(mk3::Unverified::DevicePath, kTimerPath | n, local_ns / kCycleNs, from);
}

// dcs.cpp timer management, with the dcs.h member types. total_cycles() is `totalcycles` (the instruction's start
// inside execute_run, m_totalcycles between slices; 0 right after a core reset). Times: emu_timer::adjust starts at
// device_scheduler::time() = the ADSP local time at the instruction start, or the expiry inside a timer callback.
void Dcs8k::TimerEnable(bool state) {
    TimerPath(1, state);
    if (state) { timer_enable_ = 1; ResetTimer(); }
    else { UpdateTimerCount(); timer_enable_ = 0; int_next_ns_ = kNever; }   // m_internal_timer->reset()
}

void Dcs8k::UpdateTimerCount() {
    if (!timer_enable_) return;
    const uint64_t elapsed_cycles = totalcycles - timer_start_cycles_;
    uint64_t elapsed_clocks = elapsed_cycles / timer_scale_;
    if (elapsed_clocks < uint32_t(timer_start_count_ + 1)) {
        timer_start_count_ = uint32_t(timer_start_count_ - elapsed_clocks);   // m_timer_start_cycles is not moved
    } else {
        elapsed_clocks -= uint32_t(timer_start_count_ + 1);
        const uint64_t periods_since_start = elapsed_clocks / uint32_t(timer_period_ + 1);
        elapsed_clocks -= periods_since_start * uint32_t(timer_period_ + 1);
        timer_start_count_ = uint32_t(timer_period_ - elapsed_clocks);
    }
    control_[0x1c] = uint16_t(timer_start_count_);
}

void Dcs8k::ResetTimer() {
    if (!timer_enable_) return;
    timer_start_cycles_ = totalcycles;
    timers_fired_ = 0;
    // the DRAM refresh stub check (m_rev < REV_DSIO): internal PM 0x18-0x1c
    if (!timer_ignore_ && pm_[0x18] == 0x0c0030 && pm_[0x19] == 0x804828 && pm_[0x1a] == 0x904828 &&
        pm_[0x1b] == 0x0c0020 && pm_[0x1c] == 0x0a001f) timer_ignore_ = true;
    if (timer_ignore_) return;
    int_next_ns_ = local_ns + uint64_t(uint32_t(timer_scale_ * (timer_start_count_ + 1))) * kCycleNs;
    // inserted as the list head: abort_timeslice, the ADSP slice ends after this instruction
    if (!host_next_ns) Reject("internal timer armed without the host scheduler's next timer");
    if (int_next_ns_ < std::min({dac_next_ns_, irq1_clear_ns_, host_next_ns})) sync_ = true;
}

void Dcs8k::InternalTimer() {
    TimerPath(4, 0);
    const uint64_t now = int_next_ns_;
    int_next_ns_ = kNever;   // a one-shot timer_device stays off unless the callback adjusts it
    ++timers_fired_;
    int64_t target_cycles = int64_t(timer_start_cycles_ + timer_scale_ * (uint32_t(timer_start_count_ + 1) +
                                                                          timers_fired_ * uint64_t(timer_period_ + 1)));
    target_cycles = int64_t(uint64_t(target_cycles) - totalcycles);
    if (!timer_ignore_ && (timer_period_ > 10 || timer_scale_ > 1)) {
        // shortcut: cycles_to_attotime of a negative count (a TCOUNT read moved the start count back) is not modeled
        if (target_cycles < 0) Reject("internal timer target before the current cycle");
        int_next_ns_ = now + uint64_t(target_cycles) * kCycleNs;
    }
    SetIrq(5, true); SetIrq(5, false);   // ADSP2105_TIMER, edge triggered: ASSERT then CLEAR (set_input_line queue)
}

}
namespace mk2::state {
// The value fields an ADSP instruction can change without a bus access: scalars and small arrays byte for byte, a
// vector as its length (its entries are records of what happened), an array of more than 256 bytes (a memory) left out.
struct WaitArchive {
    static constexpr bool reading = false;
    bool snapshot;
    std::vector<uint8_t>& bytes;
    std::array<const void*, 4> clocks;   // fields every instruction moves, and the scheduler's next timer (read at a bus access only)
    void Raw(const void* data, size_t size) {
        for (const void* clock : clocks) if (clock == data) return;
        bytes.insert(bytes.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    }
    uint64_t Size(uint64_t size) { Raw(&size, sizeof size); return size; }
    template<class... T> void operator()(T&... values) { (Io(*this, values), ...); }
};
template<class T, class Al> void Io(WaitArchive& a, std::vector<T, Al>& values) { a.Size(values.size()); }
template<class T, size_t N> void Io(WaitArchive& a, std::array<T, N>& values) {
    if (sizeof values <= 256) for (auto& value : values) Io(a, value);
}
}
namespace mk3 {

static void WaitFields(mk2::state::WaitArchive& archive, Dcs8k& dcs) { Fields(archive, dcs); }   // the friend, not Dcs8k::Fields()
void Dcs8k::WaitState(std::vector<uint8_t>& state) const {
    state.clear();
    mk2::state::WaitArchive archive{false, state, {&local_ns, &totalcycles, &instructions, &host_next_ns}};
    WaitFields(archive, const_cast<Dcs8k&>(*this));
    archive.Raw(&access_count_, sizeof access_count_);
}

bool Dcs8k::FireTimers() {   // schedule.cpp execute_timers: expiries the ADSP cannot fit another whole cycle before
    for (bool fired = false;; fired = true) {
        if (irq1_clear_ns_ < local_ns + kCycleNs && irq1_clear_ns_ <= dac_next_ns_ && irq1_clear_ns_ <= int_next_ns_) {
            if (irq1_clear_ns_ == dac_next_ns_) Reject("IRQ1 pulse end coincides with an autobuffer transfer");
            irq1_clear_ns_ = kNever;
            SetIrq(IRQ1, false);
        } else if (int_next_ns_ < local_ns + kCycleNs && int_next_ns_ <= dac_next_ns_) {
            InternalTimer();   // its effects commute with a coincident autobuffer transfer or IRQ1 pulse end
        } else if (dac_next_ns_ < local_ns + kCycleNs) {
            DacTransfer();
            dac_next_ns_ += dac_period_ns_;
        } else return fired;
    }
}

bool Dcs8k::Step() {
    if (suspended) Reject("execution while held in reset");
    sync_ = false;
    unverified_ = false;
    ppc_ = pc;
    if (observe && observe(*this)) {
        rows.push_back({boot});
        for (const auto v : Fields()) rows.back().push_back(v);
    }
    // The boot page loaded by dcs_boot selects the code identity; Fetch() still checks every executed word.
    if (page_ == 0) switch (pc) {
#include "mk3_adsp_boot.inc"
    default: Reject("PC outside the fixed translation");
    } else if (page_ == 1) switch (pc) {
#include "mk3_adsp_bank1.inc"
    default: Reject("PC outside the fixed translation");
    } else Reject("boot page outside the fixed translation");
    ++totalcycles; ++instructions;
    local_ns += kCycleNs;
    last_unverified_ = unverified_;
    last_pc_ = ppc_;
    return sync_;
}

void Dcs8k::Run(const std::vector<HostEvent>& events, uint64_t horizon_ns, const std::function<bool(const Dcs8k&)>& stop) {
    // MAME timeslice (schedule.cpp): the main CPU runs first and aborts at end_ns after its synchronizing write; the
    // ADSP then runs whole cycles up to that target. If the ADSP synchronizes itself (input-line change) in that same
    // slice, it stops there; the due host timer fires if write_ns <= its local time, else the next target is write_ns.
    // A self-synchronization in an earlier slice only splits slices and does not move the event.
    // check_irqs runs only where a timeslice really executes (execute_run, called when a whole cycle fits), so an
    // input-line change applied between slices stays pending and observable until then.
    // An idle DSP waits in a loop of a few instructions that changes nothing (its sound OS polls a register a timer
    // moves). Three arrivals in a row at one PC by a backward transfer, the last two with the same WaitState: the
    // instructions between them made no bus access and changed nothing, so a turn of the loop from that state always
    // comes back to it. Its whole turns up to the next timer or the slice's end are then counted, not run, here and
    // at any later arrival in that same state (a bus access since makes the state another). The clocks and the place
    // in the loop come out as if every instruction had run. Not with a host event in progress nor with an observer.
    static constexpr unsigned kWaitRest = 64;   // backward transfers passed over after a state that differed (a loop at work)
    auto run_to = [&](uint64_t target, const HostEvent* event) {
        bool slice_start = true;
        uint16_t turn_pc = 0xffff;
        uint64_t turn_at = 0;
        unsigned rest = 0;
        bool have = false;
        const auto turns = [&](uint64_t turn) {
            const uint64_t limit = std::min(target, NextTimerNs());
            if (limit <= local_ns) return;
            const uint64_t skipped = (limit - local_ns) / kCycleNs / turn * turn;
            local_ns += skipped * kCycleNs; totalcycles += skipped; instructions += skipped;
        };
        for (;;) {
            if (FireTimers()) { slice_start = true; have = false; turn_pc = 0xffff; wait_clean_ = false; }   // timers still expire while RESET is held
            // A DCS timer inside the writing main-CPU instruction would end the slice before end_ns.
            if (const uint64_t t = NextTimerNs(); event && t > event->write_ns && t <= event->end_ns)
                Reject("DCS timer inside a host write instruction");
            if (local_ns + kCycleNs > target) return false;
            if (suspended) { local_ns += kCycleNs; ++totalcycles; continue; }   // suspend(RESET, eatcycles): whole cycles
            if (slice_start) { CheckIrqs(); slice_start = false; }
            if (stop && stop(*this)) return true;
            const uint64_t before = local_ns;
            const bool synchronized = Step();
            if (eat_) {   // m_icount = 0, then m_icount--: the slice's remaining whole cycles run out
                eat_ = false;
                const uint64_t end = std::min(target, NextTimerNs()), extra = (end - before) / kCycleNs;
                local_ns += extra * kCycleNs; totalcycles += extra;
                slice_start = true;
            }
            if (synchronized) {
                ++synchronizations;
                slice_start = true;
                if (reboot_) { reboot_ = false; CoreReset(); }   // RESET assert+clear events: device reset, resume
                if (return_at_sync) return true;
                if (event && local_ns > event->slice_min_ns) {
                    if (local_ns <= event->slice_max_ns) Reject("self-synchronization at an unknown slice boundary");
                    if (event->write_ns <= local_ns) return false;
                    target = std::min(target, event->write_ns);
                }
            }
            if (slice_start) { have = false; turn_pc = 0xffff; wait_clean_ = false; }   // it synchronized or ate its slice
            else if (!event && !stop && !observe && pc < ppc_) {   // a backward transfer ends a turn of a loop
                if (wait_turn_ && pc == wait_pc_) {
                    if (!wait_clean_) { WaitState(wait_now_); wait_clean_ = wait_now_ == wait_state_; }
                    if (wait_clean_) { turns(wait_turn_); rest = kWaitRest; }
                    else wait_turn_ = 0;
                } else wait_clean_ = false;
                if (rest) { --rest; have = false; turn_pc = 0xffff; }
                else if (pc != turn_pc) { turn_pc = pc; have = false; }
                else if (!have) { WaitState(wait_state_); have = true; wait_turn_ = 0; }
                else {
                    WaitState(wait_now_);
                    if (wait_now_ == wait_state_ && instructions > turn_at) { wait_pc_ = pc; wait_turn_ = instructions - turn_at; wait_clean_ = true; turns(wait_turn_); }
                    rest = kWaitRest; have = false; turn_pc = 0xffff;
                }
                turn_at = instructions;
            }
        }
    };
    if (!events.empty()) wait_clean_ = false;
    for (const auto& event : events) {
        if (event.write_ns < local_ns || event.end_ns < event.write_ns) throw std::runtime_error("MK3 DCS host events out of order");
        if (event.kind == HostEvent::DataRead) {   // data_r with m_auto_ack: delayed_ack_w() directly, no synchronize
            if (run_to(std::min(event.slice_min_ns, event.write_ns), nullptr)) return;
            host_reads.push_back({event.write_ns, output_data});
            SetLatch(latch_control | kOutputEmpty, event.write_ns);
            read_slice_min_ns_ = event.slice_min_ns; read_slice_max_ns_ = event.slice_max_ns;
            continue;
        }
        if (run_to(event.end_ns, &event)) return;
        switch (event.kind) {
        case HostEvent::ResetAssert: DcsReset(event.write_ns); suspended = true; break;   // dcs_reset timer, then RESET
        case HostEvent::ResetRelease: if (suspended) CoreReset(); break;
        case HostEvent::DataWrite:   // dcs_delayed_data_w (DCS1 without SPORT0 timer): IRQ2 ASSERT, input full
            SetIrq(IRQ2, true);
            SetLatch(latch_control & ~kInputEmpty, event.write_ns);
            input_data = event.value;
            break;
        case HostEvent::DataRead: break;
        }
    }
    run_to(horizon_ns, nullptr);
}

const std::vector<std::string>& Dcs8k::FieldNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n = {"pc", "totalcycles"};
        const char* core[] = {"ax0", "ax1", "ay0", "ay1", "ar", "af", "mx0", "mx1", "my0", "my1",
                              "mr0", "mr1", "mr2", "mf", "si", "se", "sb", "sr0", "sr1"};
        for (const char* c : core) n.push_back(c);
        for (const char* c : core) n.push_back(std::string(c) + "_sec");
        for (const char* g : {"i", "l", "m"}) for (int k = 0; k < 8; ++k) n.push_back(g + std::to_string(k));
        for (const char* s : {"px", "cntr", "astat", "sstat", "mstat", "pcsp", "cntrsp", "statsp", "loopsp", "imask", "icntl",
                              "irq0", "irq1", "irq2", "irq3", "flagin", "flagout", "fl0", "fl1", "fl2"}) n.push_back(s);
        return n;
    }();
    return names;
}

std::vector<uint64_t> Dcs8k::Fields() const {
    std::vector<uint64_t> v = {pc, totalcycles};
    for (const Core* c : {&core_, &alt_})
        for (const uint16_t r : {c->ax0, c->ax1, c->ay0, c->ay1, c->ar, c->af, c->mx0, c->mx1, c->my0, c->my1,
                                 c->mr0, c->mr1, c->mr2, c->mf, c->si, c->se, c->sb, c->sr0, c->sr1}) v.push_back(r);
    for (const auto x : i_) v.push_back(x);
    for (const auto x : l_) v.push_back(x);
    for (const auto x : m_) v.push_back(uint32_t(x));
    for (const uint64_t x : {uint64_t(px_), uint64_t(cntr_), uint64_t(astat_), uint64_t(sstat_), uint64_t(mstat_), uint64_t(pc_sp_),
                             uint64_t(cntr_sp_), uint64_t(stat_sp_), uint64_t(loop_sp_), uint64_t(imask_), uint64_t(icntl_),
                             uint64_t(irq_state_[0]), uint64_t(irq_state_[1]), uint64_t(irq_state_[2]), uint64_t(irq_state_[3]),
                             uint64_t(flagin_), uint64_t(flagout_), uint64_t(fl0_), uint64_t(fl1_), uint64_t(fl2_)}) v.push_back(x);
    return v;
}
}
