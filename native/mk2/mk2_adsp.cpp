#include "mk2_adsp.hpp"
#include "mk2_board.hpp"
#include "mk2_live_limits.hpp"
#include "mk2_operator_boundaries.hpp"
#include "mk2_oracle_digest.hpp"
#include "mk2_valid_cmos_startup.hpp"
#include <limits>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "mk5_program.hpp"

namespace mk2 {
void DcsBootMachine::AdvanceVideo(BoardBootControls& board, const BootVideo& video) const {
    board.AdvanceScanlines(ScanlineEvents(), [&](const BoardBootControls& edge) {
        if (scanline_observer_) scanline_observer_(edge, video);
    });
}

AdspBoot::AdspBoot(const std::vector<uint32_t>& boot, DcsLink& link, const std::vector<uint8_t>& rom)
    : link_(link), boot_(boot), rom_storage_(std::make_shared<const std::vector<uint8_t>>(rom)), rom_(*rom_storage_) {
    if (!rom.empty() && !((rom.size() == 0x80000 &&
        Sha256(rom) == "82bdcbbd9413071f20a52cf7fd0fcf094fa474a08938b4f8c1d617d1490a137c") ||
        (rom.size() == 6 * 0x80000 && Sha256(rom) == kDcsDataSha256)))
        throw std::runtime_error("ADSP banked boot requires authenticated U2 ROM");
    if (boot.size() != 376) throw std::runtime_error("ADSP requires the MK2 U2 boot length");
    if (!rom.empty() && DecodeDcsBoot({rom.begin(), rom.begin() + 0x1000}) != boot)
        throw std::runtime_error("ADSP initial boot does not match authenticated U2 ROM");
    ReloadBoard();
}

void AdspBoot::ReloadBoard() {
    rom_bank = 0; rebooted_ = reset_pending_ = false;
    std::copy(boot_.begin(), boot_.end(), pm.begin());
    control.fill(0);
    sport_ireg = sport_size = sport_incs = 0; sport_enabled = false;
    sport_irq1_asserted = false; // Board IRQ CLEAR preserves its latch until core reset.
}

void AdspBoot::ResetCore() {
    // ADSP warm reset does not erase data/DAG registers or RAM (pinned device_reset).
    SelectBank(0);
    pc = 0; cycles = 0; imask = 0; sstat = 0x55;
    pc_sp = cntr_sp = loop_sp = stat_sp = 0;
    sport_irq1_asserted = sport_irq1_latched = false;
}

void AdspBoot::CheckOpcode(uint32_t expected) const {
    const auto actual=ReadProgram(pc);
    if (actual != expected) throw std::runtime_error("ADSP translated opcode changed at PC " + std::to_string(pc)+
        "; expected "+std::to_string(expected)+"; actual "+std::to_string(actual));
}

void AdspBoot::Advance() {
    if (loop_sp && pc == loops_[loop_sp - 1].end) {
        if (cntr > 1) { --cntr; pc = loops_[loop_sp - 1].start; return; }
        // CE is evaluated before the loop's final instruction executes.
        if (!cntr_sp || !pc_sp) throw std::runtime_error("ADSP loop stack underflow");
        cntr = counters_[--cntr_sp];
        --loop_sp; --pc_sp;
        if (!cntr_sp) sstat |= 4;
        if (!loop_sp) sstat |= 0x40;
        if (!pc_sp) sstat |= 1;
    }
    ++pc;
}

void AdspBoot::SetCounter(uint16_t value) {
    if (cntr_sp == counters_.size()) throw std::runtime_error("ADSP counter stack outside qualified scope");
    counters_[cntr_sp++] = cntr;
    cntr = value & 0x3fff;
    sstat &= ~uint8_t(4);
}

void AdspBoot::StartLoop(uint16_t end) {
    if (loop_sp == loops_.size() || pc_sp == 16) throw std::runtime_error("ADSP loop stack outside qualified scope");
    loops_[loop_sp++] = {pc, end};
    PushPc();
    sstat &= ~uint8_t(0x41);
}

void AdspBoot::ReadProgramAr() {
    if (l[5]) throw std::runtime_error("ADSP PM read outside boot scope");
    ar = ReadProgramDag(5, 5);
}

void AdspBoot::WriteProgramAr() {
    if (l[4]) throw std::runtime_error("ADSP PM write outside boot scope");
    const uint32_t value = (uint32_t(ar) << 8) | px;
    WriteProgram(i[4], value);
    i[4] = uint16_t((uint32_t(i[4]) + uint32_t(m[6])) & 0x3fff);
}

void AdspBoot::CheckSoftwareBoot() const {
    if (si != 0x208 || rom_bank != 1 || rom_.empty())
        throw std::runtime_error("ADSP software boot requires qualified bank one and authenticated U2 ROM");
}

void AdspBoot::LoadBootPage() {
    const size_t offset = size_t(rom_bank) * 0x1000;
    const size_t count = (size_t(rom_.at(offset + 3)) + 1) * 8;
    if (count > pm.size()) throw std::runtime_error("ADSP boot page exceeds internal PM");
    for (size_t n = 0; n < count; ++n)
        pm[n] = (uint32_t(rom_[offset + 4*n]) << 16) |
            (uint32_t(rom_[offset + 4*n + 1]) << 8) | rom_[offset + 4*n + 2];
    ++boot_reloads;
}

// Context guards compare live PM with the U2 word at the same PC, read from the authenticated ROM
// (never stored here). Without the ROM no guarded context qualifies. First boot: bank-zero page, its
// IRQ2 vector is the page word 0x1c copied to PM 4. Firmware: bank-one page, external PM from U2 0x2000.
uint32_t AdspBoot::BootWord(uint16_t address) const {
    if (rom_.empty()) return 0xffffffffu;
    const size_t offset = size_t(address == 4 ? 0x1c : address) * 4;
    return (uint32_t(rom_[offset]) << 16) | (uint32_t(rom_[offset + 1]) << 8) | rom_[offset + 2];
}

uint32_t AdspBoot::FirmwareWord(uint16_t address) const {
    if (rom_.empty() || (address >= 0x400 && address < 0x800) || address >= 0x2000) return 0xffffffffu;
    const size_t offset = address < 0x400 ? 0x1000 + size_t(address) * 4 : 0x2000 + size_t(address - 0x800) * 4;
    return (uint32_t(rom_[offset]) << 16) | (uint32_t(rom_[offset + 1]) << 8) | rom_[offset + 2];
}

bool AdspBoot::ApplyPendingReset() {
    if (!reset_pending_) return false;
    reboot_events.push_back({InstructionCount(), cycles, rom_bank, boot_reloads});
    reset_pending_ = false;
    ResetCore();
    return true;
}

uint32_t AdspBoot::ReadProgram(uint16_t address) const {
    if (address < pm.size()) return pm[address];
    if (address >= 0x0800 && address < 0x2000) return external_pm[address & 0x7ff];
    throw std::runtime_error("ADSP PM read outside DCS 2K map");
}

void AdspBoot::WriteProgram(uint16_t address, uint32_t value) {
    if (address < pm.size()) pm[address] = value & 0xffffff;
    else if (address >= 0x0800 && address < 0x2000) external_pm[address & 0x7ff] = value & 0xffffff;
    else throw std::runtime_error("ADSP PM write outside DCS 2K map");
    Record(address, value & 0xffffff, 'p');
}

void AdspBoot::PushPc() {
    if (pc_sp == pc_stack_.size()) throw std::runtime_error("ADSP PC stack outside qualified scope");
    pc_stack_[pc_sp++] = pc; sstat &= ~uint8_t(1);
}

void AdspBoot::ReturnCall() {
    if (!pc_sp) throw std::runtime_error("ADSP call stack underflow");
    pc = pc_stack_[--pc_sp];
    if (!pc_sp) sstat |= 1;
}

void AdspBoot::Modify(unsigned index, unsigned modifier) {
    if (index >= 8 || modifier >= 8)
        throw std::runtime_error("ADSP unqualified DAG addressing");
    const uint32_t length = l[index];
    uint32_t mask = 0x3fff;
    if (length) {
        unsigned bits = 0; for (uint32_t n = length - 1; n; n >>= 1) ++bits;
        mask = (uint32_t(1) << bits) - 1;
    }
    const uint32_t base = i[index] & ~mask;
    uint32_t next = (uint32_t(i[index]) + uint32_t(m[modifier])) & 0x3fff;
    if (next < base) next += length;
    else if (next >= base + length) next -= length;
    i[index] = uint16_t(next);
}

uint16_t AdspBoot::ReadDag(unsigned index, unsigned modifier) {
    const uint16_t value = ReadData(DagAddress(index)); Modify(index, modifier); return value;
}

void AdspBoot::WriteDag(unsigned index, unsigned modifier, uint16_t value) {
    WriteData(DagAddress(index), value); Modify(index, modifier);
}

uint16_t AdspBoot::ReadProgramDag(unsigned index, unsigned modifier) {
    const uint16_t address = i.at(index);
    const uint32_t word = ReadProgram(address);
    Record(address, word, 'q');
    px = uint8_t(word);
    Modify(index, modifier);
    return uint16_t(word >> 8);
}

void AdspBoot::WriteProgramDag(unsigned index, unsigned modifier, uint16_t value) {
    WriteProgram(i.at(index), (uint32_t(value) << 8) | px); Modify(index, modifier);
}

void AdspBoot::LogicAr(uint16_t value) {
    ar = Logic(value);
}

uint16_t AdspBoot::DagAddress(unsigned index) const {
    uint16_t address = i.at(index);
    if (index < 4 && (mstat & 2)) {
        uint16_t reversed = 0;
        for (unsigned bit = 0; bit < 14; ++bit) {
            reversed = uint16_t((reversed << 1) | (address & 1)); address >>= 1;
        }
        address = reversed;
    }
    return address;
}

uint16_t AdspBoot::Logic(uint16_t value) {
    astat = uint8_t((astat & ((mstat & 4) ? 0xf4 : 0xf0)) | ((value >> 14) & 2) | (value == 0));
    return value;
}

void AdspBoot::AddAr(uint16_t x, uint16_t y) {
    ar = Add(x, y, true);
}

uint16_t AdspBoot::Absolute(uint16_t value, bool saturate) {
    const uint16_t result=Logic((value&0x8000)?uint16_t(0u-value):value);
    astat=uint8_t((astat&~0x10u)|((value&0x8000)?0x10u:0));
    if(value==0x8000) astat|=4;
    return Saturated(result,saturate);
}

void AdspBoot::DivideSign() {
    const uint32_t sign=ax0^af;
    astat=uint8_t((astat&~0x20u)|((sign>>10)&0x20));
    af=uint16_t((uint32_t(af)<<1)|(ay0>>15));
    ay0=uint16_t((uint32_t(ay0)<<1)|(sign>>15));
}

void AdspBoot::DivideQuotient() {
    const uint32_t result=(astat&0x20)?uint32_t(af)+ax0:uint32_t(af)-ax0;
    const uint32_t sign=result^ax0;
    astat=uint8_t((astat&~0x20u)|((sign>>10)&0x20));
    af=uint16_t((result<<1)|(ay0>>15));
    ay0=uint16_t((uint32_t(ay0)<<1)|((~sign>>15)&1));
}

uint16_t AdspBoot::Saturated(uint16_t value, bool saturate) const {
    return (saturate && (mstat & 8) && (astat & 4)) ? ((astat & 8) ? 0x8000 : 0x7fff) : value;
}

uint16_t AdspBoot::Add(uint16_t x, uint16_t y, bool saturate) {
    const uint32_t value = uint32_t(x) + y;
    Logic(uint16_t(value));
    astat |= uint8_t(((uint32_t(x) ^ y ^ value ^ (value >> 1)) >> 13) & 4);
    astat |= uint8_t((value >> 13) & 8);
    return Saturated(uint16_t(value), saturate);
}

uint16_t AdspBoot::Sub(uint16_t x, uint16_t y, bool saturate) {
    const uint32_t value = uint32_t(x) - y;
    Logic(uint16_t(value));
    astat |= uint8_t(((uint32_t(x) ^ y ^ value ^ (value >> 1)) >> 13) & 4);
    astat |= uint8_t((~value >> 13) & 8);
    return Saturated(uint16_t(value), saturate);
}

uint16_t AdspBoot::Increment(uint16_t value, bool saturate) {
    const uint16_t result = Logic(uint16_t(value + 1));
    if (value == 0x7fff) astat |= 4;
    else if (value == 0xffff) astat |= 8;
    return Saturated(result, saturate);
}

uint16_t AdspBoot::Decrement(uint16_t value, bool saturate) {
    const uint16_t result = Logic(uint16_t(value - 1));
    if (value == 0x8000) astat |= 4;
    else if (!value) astat |= 8;
    return Saturated(result, saturate);
}

uint16_t AdspBoot::Negate(uint16_t value, bool saturate) {
    const uint16_t result = Logic(uint16_t(0u - value));
    if (value == 0x8000) astat |= 4;
    if (!value) astat |= 8;
    return Saturated(result, saturate);
}

void AdspBoot::ShiftLogical(uint16_t value, int count, bool high, bool accumulate) {
    const uint32_t source = high ? uint32_t(value) << 16 : value;
    const uint32_t shifted = count >= 0 ? (count < 32 ? source << unsigned(count) : 0)
        : (count > -32 ? source >> unsigned(-count) : 0);
    SetShift(shifted | (accumulate ? ((uint32_t(sr1) << 16) | sr0) : 0));
}

void AdspBoot::ShiftArithmetic(uint16_t value, int count) {
    const uint32_t source = uint32_t(int32_t(int16_t(value)));
    uint32_t result;
    if (count >= 0) result = count < 32 ? source << unsigned(count) : 0;
    else if (count <= -32) result = (value & 0x8000) ? ~uint32_t(0) : 0;
    else {
        const unsigned amount = unsigned(-count);
        result = (source >> amount) | ((value & 0x8000) ? (~uint32_t(0) << (32 - amount)) : 0);
    }
    SetShift(result);
}

void AdspBoot::Exponent(uint16_t value) {
    astat = uint8_t((astat & ~0x80u) | ((value & 0x8000) ? 0x80 : 0));
    uint16_t normalized = (value & 0x8000) ? uint16_t(~value) : value;
    unsigned leading = 0;
    while (leading < 16 && !(normalized & 0x8000)) { ++leading; normalized <<= 1; }
    se = uint16_t(int16_t(1 - int(leading)));
}

void AdspBoot::MacMr(uint16_t x, uint16_t y, bool signed_x, bool signed_y, unsigned action, bool rounded) {
    const int64_t left = signed_x ? int64_t(int16_t(x)) : int64_t(x);
    const int64_t right = signed_y ? int64_t(int16_t(y)) : int64_t(y);
    const uint32_t product_bits = uint32_t(left * right) << ((mstat & 0x10) ? 0 : 1);
    const int64_t product = int32_t(product_bits);
    const int64_t previous = int64_t(int16_t(mr2)) * 0x100000000ll + (uint32_t(mr1) << 16) + mr0;
    int64_t result = action == 1 ? product : action == 2 ? previous + product : previous - product;
    if (rounded) {
        result += 0x8000;
        if ((product_bits & 0xffff) == 0x8000) result = int64_t(uint64_t(result) & ~uint64_t(0x10000));
    }
    const unsigned overflow = unsigned((uint64_t(result) >> 31) & 0x1ff);
    astat = uint8_t((astat & ~0x40u) | ((overflow && overflow != 0x1ff) ? 0x40 : 0));
    mr0 = uint16_t(result); mr1 = uint16_t(uint64_t(result) >> 16);
    mr2 = uint16_t(uint64_t(result) >> 32);
}

void AdspBoot::SaturateMr() {
    if (!(astat & 0x40)) return;
    if (mr2 & 0x80) { mr2 = 0xffff; mr1 = 0x8000; mr0 = 0; }
    else { mr2 = 0; mr1 = 0x7fff; mr0 = 0xffff; }
}

void AdspBoot::SetShift(uint32_t value) { sr0 = uint16_t(value); sr1 = uint16_t(value >> 16); }

void AdspBoot::ShiftSi() {
    const int count = int8_t(se);
    const uint32_t value = count >= 0 ? (count < 32 ? uint32_t(si) << unsigned(count) : 0)
        : (count > -32 ? uint32_t(si) >> unsigned(-count) : 0);
    SetShift(value);
}

void AdspBoot::SetIfc(uint16_t value) {
    if (value != 0x3f || link_.Irq2Asserted())
        throw std::runtime_error("ADSP IFC outside qualified inactive-IRQ context");
    ifc = value;
}

void AdspBoot::SportTransmit(uint16_t value) {
    serial_events.push_back({instruction_pc_, 1, value, cycles});
    sport_enabled = (control[31] & 0x0800) && (control[15] & 2);
    if (!sport_enabled) return;
    sport_ireg = (control[15] >> 9) & 7;
    const unsigned modifier = ((control[15] >> 7) & 3) | (sport_ireg & 4);
    sport_size = l[sport_ireg]; sport_incs = uint16_t(m[modifier]);
    sport_base = i[sport_ireg] & uint16_t(~15); i[sport_ireg] = sport_base;
    if (sport_ireg != 7 || sport_size != 0x1e0 || sport_incs != 1 ||
        (sport_base != 0x400 && (rebooted_ || sport_base != 0x3800)))
        throw std::runtime_error("ADSP SPORT configuration outside qualified boot scope");
    dac_enabled_ = true;
}

void AdspBoot::WriteControl(uint16_t address, uint16_t value) {
    if (address < 0x3fe0 || address > 0x3fff)
        throw std::runtime_error("ADSP control address outside DCS map");
    if (address == 0x3fff && value != 0x10 && value != 0x208 && value != 0xc10)
        throw std::runtime_error("ADSP SYSCONTROL outside qualified boot scope");
    if (address == 0x3fff && (value & 0x200) && (rom_.empty() || rom_bank > 1))
        throw std::runtime_error("ADSP software boot requires authenticated U2 bank zero or one");
    if (address != 0x3fff && !((address == 0x3ffe && value == 0x2280) ||
        (address == 0x3ff1 && value == 9) || (address == 0x3ff2 && value == 0x4a9f) ||
        (address == 0x3fef && value == 0x0e82)))
        throw std::runtime_error("ADSP control write outside qualified boot configuration");
    const bool disable_sport = (address == 0x3fff && !(value & 0x800)) ||
                              (address == 0x3fef && !(value & 2));
    if (disable_sport) {
        DacUpdate(instruction_ticks_); // dmadac_enable flushes before clearing pointers.
        dac_enabled_ = false;
        dac_bufin_ = dac_bufout_ = 0;
        sport_timer_cancelled = true;
    }
    control[address - 0x3fe0] = value;
    Record(address, value, 'd');
    if (address == 0x3fff && (value & 0x200)) {
        LoadBootPage(); rebooted_ = rom_bank == 1; reset_pending_ = true; control[31] = 0;
    }
    if (disable_sport)
        sport_enabled = false;
}

void AdspBoot::WriteData(uint16_t address, uint16_t value) {
    if(presentation_only_ && address>=0x3000 && address<0x3800 && address!=0x3000)
        throw std::runtime_error("presentation transform attempted device I/O");
    if (address < 0x2000) {
        auto& word = external_pm[address & 0x7ff]; word = (uint32_t(value) << 8) | (word & 0xff);
    } else if (address >= 0x3000 && address <= 0x33ff) {
        if ((value & 0x7ff) > 0xff && rom_.size() != 6 * 0x80000)
            throw std::runtime_error("ADSP ROM bank outside authenticated U2 mapping; bank " +
            std::to_string(value & 0x7ff) + "; DSP PC " + std::to_string(pc));
        rom_bank = value & 0x7ff;
    } else if (address >= 0x3400 && address <= 0x37ff) {
        link_.DspWriteOutput(value);
    } else if (address >= 0x3800 && address < 0x3a00) dm[address - 0x3800] = value;
    else if (address >= 0x3fe0) { WriteControl(address, value); return; }
    else if (address < 0x2000 || address < 0x3a00 || address >= 0x3fe0)
        throw std::runtime_error("ADSP DM write outside qualified DCS map");
    // 3a00..3fdf is unmapped; original writes retain their physical observation.
    Record(address, value, 'd');
}

uint16_t AdspBoot::ReadData(uint16_t address) {
    if(presentation_only_ && address>=0x3400 && address<0x3800)
        throw std::runtime_error("presentation transform attempted latch input");
    uint16_t value;
    if (address < 0x2000) value = uint16_t(external_pm[address & 0x7ff] >> 8);
    else if (address < 0x3000) {
        if (rom_.empty() || (rom_bank > 0xff && rom_.size() != 6 * 0x80000))
            throw std::runtime_error("ADSP ROM read outside authenticated U2 mapping");
        const unsigned bank = rom_bank % 0x600;
        const size_t offset = (bank >> 8) * 0x80000 + (bank & 0x7f) * 0x1000 + address - 0x2000;
        value = uint16_t(0xff00 | rom_.at(offset));
    } else if (address >= 0x3400 && address <= 0x37ff) value = link_.DspReadInput();
    else if (address >= 0x3800 && address < 0x3a00) value = dm[address - 0x3800];
    else if (address >= 0x3fe0) value = ReadControl(address);
    else value = 0xffff;
    Record(address, value, 'r');
    return value;
}

void AdspBoot::CopyArToAf() {
    // AF = AR + 0 clears NZVC (except sticky V); no carry or overflow can arise.
    af = ar;
    astat = uint8_t((astat & ((mstat & 4) ? 0xf4 : 0xf0)) | ((ar >> 14) & 2) | (ar == 0));
}

void AdspBoot::DelayBranch() {
    if (!(astat & 1)) pc = 0xf4;
}

uint16_t AdspBoot::ReadControl(uint16_t address) const {
    // DCS1 has no IDMA controller: the mapped read returns the undriven value.
    if (address == 0x3fe0) return 0xffff;
    return control.at(address - 0x3fe0);
}

uint16_t AdspBoot::ReadInput() {
    if (i[4] != 0x3400 || m[4] || l[4]) throw std::runtime_error("ADSP input DAG outside boot scope");
    const uint16_t value = link_.DspReadInput();
    Record(0x3400, value, 'r');
    return value;
}

void AdspBoot::SetInterruptControl(unsigned reg, uint16_t value) {
    if (reg == 3) {
        if (value && link_.Irq2Asserted()) throw std::runtime_error("ADSP IRQ entry outside translated boot scope");
        imask = uint8_t(value);
    } else icntl = uint8_t(value);
}

void AdspBoot::SetMstat() {
    if (si != 0x10) throw std::runtime_error("ADSP MSTAT outside boot scope");
    SelectBank(uint8_t(si));
}

void AdspBoot::SelectBank(uint8_t value) {
    if ((value ^ mstat) & 1) {
        std::swap(ar, alternate_.ar); std::swap(si, alternate_.si);
        std::swap(af, alternate_.af); std::swap(mx0, alternate_.mx0); std::swap(mx1, alternate_.mx1);
        std::swap(ax0, alternate_.ax0);
        std::swap(ax1, alternate_.ax1);
        std::swap(ay0, alternate_.ay0);
        std::swap(ay1, alternate_.ay1);
        std::swap(my0, alternate_.my0);
        std::swap(my1, alternate_.my1);
        std::swap(mr0, alternate_.mr0);
        std::swap(mr1, alternate_.mr1);
        std::swap(mr2, alternate_.mr2);
        std::swap(mf, alternate_.mf);
        std::swap(se, alternate_.se);
        std::swap(sb, alternate_.sb);
        std::swap(sr0, alternate_.sr0);
        std::swap(sr1, alternate_.sr1);
    }
    mstat = value;
}

void AdspBoot::EnterIrq2() {
    const bool boot_context = pc == 0x44 && pm[4] == BootWord(4);
    // Original sound-board General Test, IRQ2 in the two nested checksum loops.
    const bool checksum_context = !rebooted_ && !rom_.empty() && pm[4] == BootWord(4) &&
        ((pc == 0x8e && pm[0x8e] == BootWord(0x8e)) || (pc == 0x90 && pm[0x90] == BootWord(0x90))) &&
        imask == 0x24 && mstat == 0x10 &&
        pc_sp == 2 && loop_sp == 2 && cntr_sp == 2 && i[4] == 0x1000 && !l[4] && !m[4] &&
        loops_[0].start == 0x86 && loops_[0].end == 0x93 &&
        loops_[1].start == 0x8a && loops_[1].end == 0x92 && sport_enabled && sport_base == 0x3800;
    const bool firmware_context = rebooted_ && pm[4] == FirmwareWord(4) &&
        (((live_host_domain_ || main_horizon_ == 5077311 || main_horizon_ == 6087360 || main_horizon_ == 7005800 || main_horizon_ == 22081952 || main_horizon_ == 23108559 || main_horizon_ == 24004881 || main_horizon_ == 30060282 || main_horizon_ == 100091930 || main_horizon_ == 101005964 || main_horizon_ == 104090574 || main_horizon_ == 110030700 || main_horizon_ == 118028828 || main_horizon_ == 126026668 || main_horizon_ == 134024907 || main_horizon_ == 150023965) && loop_sp <= loops_.size()) ||
         (mstat == 8 && (pc == 0x136 || pc == 0x138 ||
             ((OperatorHorizon(main_horizon_,OperatorBoundary::Setup)&&main_horizon_!=6883473) && pc==0x137) ||
             (OperatorCreditStartupHorizon(main_horizon_) && pc>=0x130 && pc<=0x138)) && !pc_sp && !loop_sp));
    // U2 software boot page: compare the live return word and its active stacks.
    const auto matches = [&](uint16_t first, uint16_t count) {
        return pc>=first && pc-first<count && ReadProgram(pc)==FirmwareWord(pc);
    };
    const bool service_firmware=service_irq_domain_ && rebooted_;
    const bool idle=mstat==8 && !pc_sp && !loop_sp && !cntr_sp && matches(0x0130,9);
    // FC7C phase +13 interrupted the scaling loop at its MAC and final store; the counters there are SHA-256 only.
    static constexpr const char* scale_counters[]={"23ee4f4d0d1c336aa9e75c985dc6431dee3891f56d49061bda19a988010b38c8",
                                                   "e4e2387f25e6efb639af0ef312c33065a3f623682eca61e67438c0c92d1b6ed8"};
    const bool scale_loop=mstat==8 && (pc==0x0127 || pc==0x0129) && pm[pc]==FirmwareWord(pc) &&
        pc_sp==1 && pc_stack_[0]==0x0126 && loop_sp==1 &&
        loops_[0].start==0x0126 && loops_[0].end==0x0129 &&
        cntr_sp==1 && DigestIn(WordsDigest({pc,cntr,counters_[0]}),scale_counters);
    const bool mixer=mstat==24 && pc_sp>=2 && pc_stack_[0]==0x00a5 && pc_stack_[1]==0x0178 &&
        ((pc_sp==2 && !loop_sp && !cntr_sp && matches(0x08d0,11)) ||
         (pc_sp==3 && loop_sp==1 && cntr_sp==1 && pc_stack_[2]==0x09fa &&
          loops_[0].start==0x09fa && loops_[0].end==0x09ff && matches(0x09fa,6)));
    const bool service_context=rom_.size()==6*0x80000 && pm[4]==FirmwareWord(4) &&
        pm[0x394]==FirmwareWord(0x394) && pm[0x3e7]==FirmwareWord(0x3e7) && (idle || scale_loop || mixer);
    const bool qualified_context=service_firmware ? service_context && imask==0x20 :
        ((boot_context || firmware_context) && imask==0x20) || checksum_context;
    if (!qualified_context || icntl || stat_sp || pc_sp >= 16)
        throw std::runtime_error("ADSP IRQ entry outside translated boot scope; DSP PC "+std::to_string(pc)+"; mstat/imask/icntl "+std::to_string(mstat)+"/"+std::to_string(imask)+"/"+std::to_string(icntl)+"; stacks "+std::to_string(pc_sp)+"/"+std::to_string(loop_sp)+"/"+std::to_string(stat_sp));
    interrupt_ = {pc, mstat, imask, astat};
    pc_stack_[pc_sp] = pc;
    ++pc_sp; ++stat_sp; sstat &= ~uint8_t(0x11);
    pc = 4; imask &= ~uint8_t(0x3f);
}

void AdspBoot::ReturnIrq() {
    if (stat_sp != 1 || !pc_sp || link_.Irq2Asserted())
        throw std::runtime_error("ADSP IRQ return outside translated boot scope");
    --pc_sp; --stat_sp;
    if (!pc_sp) sstat |= 1;
    sstat |= 0x10;
    pc = interrupt_.pc; SelectBank(interrupt_.mstat);
    imask = interrupt_.imask; astat = interrupt_.astat;
}

void AdspBoot::EnterIrq1() {
    if (rebooted_ || sport_base != 0x3800 || !sport_enabled || !dac_enabled_ ||
        pm[0x10] != BootWord(0x10) || imask != 0x24 || icntl || stat_sp || pc_sp >= 16 ||
        loop_sp > loops_.size() || cntr_sp > counters_.size())
        throw std::runtime_error("ADSP IRQ1 entry outside authenticated first-boot SPORT context");
    interrupt_ = {pc,mstat,imask,astat};
    sport_irq1_latched = false;
    pc_stack_[pc_sp++] = pc;
    ++stat_sp;sstat &= ~uint8_t(0x11);
    pc = 0x10;imask &= ~uint8_t(0x3f);
}

std::vector<uint64_t> AdspBoot::Registers() const {
    std::vector<uint64_t> values;
    values.reserve(76);
    values.insert(values.end(), {pc, cycles, ar, si, px, cntr, astat, sstat, mstat,
                                imask, icntl, pc_sp, cntr_sp, loop_sp, stat_sp, uint64_t(link_.Irq2Asserted())});
    for (auto value : i) values.push_back(value);
    for (auto value : m) values.push_back(uint16_t(value) & 0x3fff);
    for (auto value : l) values.push_back(value);
    // device_state_entry::value masks these symbols; signed_mask affects imports only.
    values.insert(values.end(), {ax0, ax1, ay0, ay1, af, mx0, mx1, my0, my1,
                                mr0, mr1, uint16_t(mr2 & 0xff), mf, uint16_t(se & 0xff), uint16_t(sb & 0x1f), sr0, sr1});
    values.insert(values.end(), {alternate_.ax0, alternate_.ax1, alternate_.ay0, alternate_.ay1,
        alternate_.ar, alternate_.af, alternate_.mx0, alternate_.mx1, alternate_.my0, alternate_.my1,
        alternate_.mr0, alternate_.mr1, uint16_t(alternate_.mr2 & 0xff), alternate_.mf, alternate_.si,
        uint16_t(alternate_.se & 0xff), uint16_t(alternate_.sb & 0x1f), alternate_.sr0, alternate_.sr1});
    return values;
}

std::vector<uint64_t> AdspBoot::DeviceState() const {
    // Timer/flags stay disabled throughout this fixed path; unsupported writes reject above.
    return {rom_bank, sport_ireg, sport_base, sport_incs, sport_size, 1,
            0, 1, 0, 0, 0, 0, 0, 0, uint64_t(dac_enabled_), dac_bufin_, dac_bufout_};
}

std::array<uint32_t, 27> AdspBoot::ActiveStacks() const {
    if (pc_sp > pc_stack_.size() || cntr_sp > counters_.size() ||
        loop_sp > loops_.size() || stat_sp > 1)
        throw std::runtime_error("ADSP stack observation outside native scope");
    // Inactive slots are padding. DO UNTIL CE stores its end word and condition 14.
    std::array<uint32_t, 27> values{};
    std::copy_n(pc_stack_.begin(), pc_sp, values.begin());
    std::copy_n(counters_.begin(), cntr_sp, values.begin() + 16);
    for (unsigned n = 0; n < loop_sp; ++n)
        values[20 + n] = uint32_t(loops_[n].end) << 4 | 14;
    if (stat_sp) {
        values[24] = interrupt_.mstat;
        values[25] = interrupt_.imask;
        values[26] = interrupt_.astat;
    }
    return values;
}

std::vector<uint64_t> AdspBoot::ExecutionState() const {
    std::vector<uint64_t> values={pc,cycles,si,ar,af,cntr,mx0,mx1,ax0,ax1,ay0,ay1,my0,my1,
        mr0,mr1,mr2,mf,se,sb,sr0,sr1,px,mstat,imask,icntl,astat,sstat,pc_sp,cntr_sp,loop_sp,stat_sp,
        rom_bank,ifc,boot_reloads,sport_ireg,sport_size,sport_incs,sport_base,uint64_t(sport_enabled),
        sport_dma_position,uint64_t(sport_irq1_asserted),uint64_t(sport_irq1_latched),uint64_t(sport_timer_cancelled),
        uint64_t(rebooted_),uint64_t(reset_pending_),dac_write_sample_,dac_last_ticks_,dac_bufin_,dac_bufout_,
        instruction_ticks_,instruction_count_,instruction_pc_,uint64_t(dac_enabled_),
        alternate_.ar,alternate_.si,alternate_.af,alternate_.mx0,alternate_.mx1,alternate_.ax0,alternate_.ax1,
        alternate_.ay0,alternate_.ay1,alternate_.my0,alternate_.my1,alternate_.mr0,alternate_.mr1,alternate_.mr2,
        alternate_.mf,alternate_.se,alternate_.sb,alternate_.sr0,alternate_.sr1,
        interrupt_.pc,interrupt_.mstat,interrupt_.imask,interrupt_.astat};
    for(auto value:i)values.push_back(value);
    for(auto value:m)values.push_back(uint16_t(value));
    for(auto value:l)values.push_back(value);
    for(auto value:pc_stack_)values.push_back(value);
    for(const auto& loop:loops_){values.push_back(loop.start);values.push_back(loop.end);}
    for(auto value:counters_)values.push_back(value);
    values.push_back(dag_base0_);
    return values;
}

std::vector<uint32_t> AdspBoot::DacBuffer(unsigned count) const {
    if (count > dac_buffer_.size()) throw std::runtime_error("ADSP DAC observation outside ring buffer");
    std::vector<uint32_t> bits;
    bits.reserve(count);
    for (unsigned n = 0; n < count; ++n) {
        const float sample = float(dac_buffer_[n]) * (1.0f / 32767.0f);
        uint32_t word;
        std::memcpy(&word, &sample, sizeof word);
        bits.push_back(word);
    }
    return bits;
}

void AdspBoot::DacUpdate(uint64_t ticks) {
    if (ticks < dac_last_ticks_ || dac_bufin_ >= 32768 || dac_bufout_ >= 32768)
        throw std::runtime_error("ADSP DAC update outside qualified enabled stream");
    if (!ticks) return; // disound.cpp::update skips machine time zero.
    // The 31250 Hz stream includes its current sample, even when the queue is empty.
    const uint64_t end = ticks / 3200 + 1;
    if (end < dac_write_sample_)
        throw std::runtime_error("ADSP DAC sample clock moved backwards");
    const uint64_t queued = (dac_bufin_ + 32768 - dac_bufout_) % 32768;
    if (dac_observer_) {
        std::array<float,512> samples{};
        for (uint64_t first=dac_write_sample_;first<end;) {
            const auto count=size_t(std::min<uint64_t>(samples.size(),end-first));
            for (size_t n=0;n<count;++n) {
                const auto offset=first+n-dac_write_sample_;
                samples[n]=offset<queued ? float(dac_buffer_[(dac_bufout_+offset)%32768])*(1.0f/32767.0f) : 0.0f;
            }
            dac_observer_(first,samples.data(),count);
            first+=count;
        }
    }
    dac_bufout_ = (dac_bufout_ + std::min(queued, end - dac_write_sample_)) % 32768;
    dac_write_sample_ = end;
    dac_last_ticks_ = ticks;
}

void AdspBoot::SportTransfer(uint64_t ticks) {
    if (link_.ResetAsserted() && !rebooted_ && !sport_enabled && !sport_ireg && !sport_size &&
        !sport_incs && dac_enabled_ && !sport_irq1_asserted && ticks <= main_horizon_ * 16) {
        // The pending callback still flushes the DAC and checks the DAG wrap with zero frames.
        DacUpdate(ticks);
        sport_base = dag_base0_;
        uint16_t index = i[0];
        if (index >= sport_base) {
            index = sport_base;
            sport_irq1_asserted = sport_irq1_latched = true;
            sport_irq1_events.push_back({ticks, pc, cycles, 1});
        }
        WriteIndex0(index);
        return;
    }
    const uint64_t packet = SportPacketCount();
    const bool first_boot = !rebooted_ && sport_base == 0x3800;
    const bool live_irq2_lease = live_host_domain_ && main_horizon_ > 7316296 &&
        live_instruction_ceiling_ && InstructionCount() <= live_instruction_ceiling_;
    // FC7C SPORT witnesses after IRQ2 auto-ack: first half without a pulse, second half with wrap.
    const bool service_first_half = i[7]==0x0400 &&
        pc==0x03a9 && pm[0x03a9]==FirmwareWord(0x03a9) &&
        interrupt_.pc==0x0136 && pm[0x0136]==FirmwareWord(0x0136);
    const bool service_wrap_half = i[7]==0x04f0 &&
        pc==0x03e4 && pm[0x03e4]==FirmwareWord(0x03e4) &&
        interrupt_.pc==0x0131 && pm[0x0131]==FirmwareWord(0x0131);
    const bool service_irq2_dma = service_irq_domain_ && rom_.size()==6*0x80000 &&
        (service_first_half || service_wrap_half) && mstat==9 && interrupt_.mstat==8 &&
        !link_.Irq2Asserted() && sport_irq1_latched;
    // DCS timer DMA continues while a non-nested IRQ2 frame masks CPU interrupts.
    const bool firmware_irq2_dma = (service_irq_domain_ ? service_irq2_dma :
        (!live_host_domain_ && main_horizon_ == 150023965) || live_irq2_lease) &&
        rebooted_ && pm[4] == FirmwareWord(4) &&
        imask == 0 && stat_sp == 1 && pc_sp == 1 && !loop_sp && !cntr_sp &&
        interrupt_.imask == 0x20 && pc_stack_[0] == interrupt_.pc;
    if (!sport_enabled || !dac_enabled_ || sport_ireg != 7 || (!first_boot && sport_base != 0x400) || sport_size != 480 ||
        sport_incs != 1 || (i[7] != sport_base && i[7] != sport_base+240) || l[7] != 480 ||
        (main_horizon_ == 3658091 ? packet > 61 : ticks > main_horizon_*16) ||
        (packet >= 1 && ((imask != (first_boot ? 0x24 : 0x20) && !firmware_irq2_dma) || icntl || sport_irq1_asserted)) ||
        (packet == 1 && sport_irq1_latched))
        throw std::runtime_error("ADSP SPORT transfer outside qualified sixty-two half-buffers");
    const uint16_t start = i[7];
    DacUpdate(ticks);
    if(music_sport_)music_sport_(*this,start);
    if (!packet) sport_dma_position = AccessCount();
    sport_dma_positions.push_back(AccessCount());
    for (unsigned n = 0; n < 240; ++n) {
        const int16_t sample = int16_t(first_boot ? dm[start+n-0x3800] : uint16_t(external_pm[start+n] >> 8));
        sport_pcm.push_back(sample);
        dac_buffer_[dac_bufin_] = sample;
        dac_bufin_ = (dac_bufin_ + 1) % 32768;
    }
    i[7] = start == sport_base ? uint16_t(start + 240) : sport_base;
    if (i[7] == sport_base) {
        // First-boot self-test unmasks IRQ1; the rebooted sound firmware leaves it masked.
        sport_irq1_asserted = sport_irq1_latched = true;
        sport_irq1_events.push_back({ticks, pc, cycles, 1});
    }
    sport_transfers.push_back({ticks, pc, cycles, sport_ireg, start, i[7], 240});
    if(live_host_domain_ && sport_pcm.size()>=32768)DiscardLiveObservations();
}

void AdspBoot::ClearSportIrq1(uint64_t ticks) {
    const bool first_boot = !rebooted_ && sport_base == 0x3800;
    if (!sport_irq1_asserted || (!first_boot && (!sport_irq1_latched || (imask & 4))))
        throw std::runtime_error("ADSP IRQ1 clear outside qualified masked pulse");
    sport_irq1_asserted = false;
    sport_irq1_events.push_back({ticks, pc, cycles, 0});
}

void AdspBoot::Step() {
    ExecuteInstruction();
    // Standalone callers publish output through their own link synchronization.
    link_.RetireDspInstruction(false);
}

uint64_t AdspBoot::InstructionRoom() const {
    if (presentation_only_) return ~uint64_t(0);
    const uint64_t ceiling = live_host_domain_ ? live_instruction_ceiling_ : main_horizon_*16/10 + 1;
    return InstructionCount() < ceiling ? ceiling - InstructionCount() : 0;
}

void AdspBoot::WaitState(std::vector<uint64_t>& state) const {
    state.clear();
    const auto put = [&](const auto&... value) { (state.push_back(uint64_t(value)), ...); };
    const auto all = [&](const auto& values) { for (const auto& value : values) state.push_back(uint64_t(value)); };
    const auto bank = [&](const auto& b) { put(b.ar, b.si, b.af, b.mx0, b.mx1, b.ax0, b.ax1, b.ay0, b.ay1, b.my0, b.my1,
                                               b.mr0, b.mr1, b.mr2, b.mf, b.se, b.sb, b.sr0, b.sr1); };
    put(pc, cntr, px, mstat, imask, icntl, astat, sstat, pc_sp, cntr_sp, loop_sp, stat_sp, rom_bank, ifc, boot_reloads);
    bank(*this); bank(alternate_);
    all(i); all(l); all(m); all(pc_stack_); all(counters_); all(control); all(discarded_observations_);
    for (const auto& loop : loops_) put(loop.start, loop.end);
    put(dag_base0_, interrupt_.pc, interrupt_.mstat, interrupt_.imask, interrupt_.astat, link_.Irq2Asserted(), link_.ResetAsserted(),
        sport_ireg, sport_size, sport_incs, sport_base, sport_enabled, sport_dma_position, sport_irq1_asserted, sport_irq1_latched,
        sport_timer_cancelled, presentation_only_, main_horizon_, live_host_domain_, service_irq_domain_, live_instruction_ceiling_,
        rebooted_, reset_pending_, dac_write_sample_, dac_last_ticks_, dac_bufin_, dac_bufout_, dac_enabled_, observe_trace_,
        instruction_pc_, AccessCount(), music_calls_, trace.size(), reboot_events.size(), serial_events.size(), sport_transfers.size(),
        sport_pcm.size(), sport_dma_positions.size(), sport_irq1_events.size());
}

void AdspBoot::ExecuteInstruction() {
    // At least ten half-ticks per instruction across the finite cooperative-batch main horizon.
    if (!presentation_only_ && InstructionCount() >= (live_host_domain_ ? live_instruction_ceiling_ : main_horizon_*16/10 + 1)) throw std::runtime_error("ADSP diagnostic instruction budget exceeded");
    if (!presentation_only_ && (imask & 0x20) && link_.Irq2Asserted()) EnterIrq2();
    else if (!presentation_only_ && (imask & 4) && (sport_irq1_asserted || ((icntl & 2) && sport_irq1_latched))) EnterIrq1();
    if(music_instruction_ && rebooted_ && (pc==8 || pc==0x21 || pc==0x31 || pc==0xa4 || pc==0xa5 || pc==0xaa || pc==0x13b)){++music_calls_;music_instruction_(*this);}
    // Unknown PCs/opcodes fail before CPU mutation; other scope failures are terminal.
    instruction_pc_ = pc;
    if (observe_trace_) trace.push_back(Registers());
    try {
        switch (uint32_t(pc) | (rebooted_ ? 0x10000u : 0)) {
#include "mk2_adsp_boot.inc"
        default: throw std::runtime_error("ADSP untranslated PC " + std::to_string(pc)+
            "; opcode "+(pc<pm.size() || (pc>=0x800 && pc<0x2000) ? std::to_string(ReadProgram(pc)) : "unavailable"));
        }
    } catch (...) { if (observe_trace_) trace.pop_back(); throw; }
    ++instruction_count_;
    ++cycles;
    if(live_host_domain_ && !observe_trace_ && (accesses.size()+unrecorded_accesses_>=4096 || serial_events.size()>=4096 || reboot_events.size()>=4096 ||
       sport_pcm.size()>=32768 || sport_transfers.size()>=4096 || sport_dma_positions.size()>=4096 || sport_irq1_events.size()>=4096))DiscardLiveObservations();
}

namespace {
// IO 28 followed by the given board scalars, as one SHA-256 (mk2_oracle_digest.hpp).
std::string BoardDigest(const BoardBootControls& board,std::initializer_list<uint64_t> scalars) {
    std::vector<uint64_t> words(board.io.begin(),board.io.end());
    words.insert(words.end(),scalars.begin(),scalars.end());
    return WordsDigest(words);
}
}

void DcsBootMachine::AdmitCoinPair(BoardBootControls& board) {
    // Original route prefix (SHA-256 of the flattened events and of vcount, DMA command, IO 17/18; mk2_oracle_digest.hpp).
    static constexpr char inputs[]="9838844cdf0ee00c1edbe56b2bde754010a66e1af80d879a9f3e82aedf0481f8";
    if (CoinPairAdmitted() || board.CoinPairAdmitted() || dsp.main_horizon_ != 3658091 ||
        last_main_ != 3658091 || scanline_events_ != 9239 || dma_completions_ != 21 ||
        dma_deadline_subticks_ != uint64_t(3657534)*160 + uint64_t(4864)*41 ||
        dsp.InstructionCount() != 5832972 || dsp.pc != 0x131 || dsp.cycles != 4578992 ||
        dsp.sport_transfers.size() != 62 || dsp.sport_dma_positions.size() != 62 ||
        board.scanline_events != scanline_events_ ||
        WordsDigest({board.vcount,board.dma_command,board.io[17],board.io[18]}) != "8eece5fb248473ff083bdf4c7a4c6ba5e84cb88f6f16a99a60cf8aaf947637ef" ||
        board.input0 != 0xffff || board.input1 != 0xffff || board.input2 != 0xffff ||
        board.requested_p1_high_punch || board.requested_coin1 || board.requested_start1 || board.dip_switches != 0xfc7d ||
        board.protection.Read()!=0 || RowsDigest(board.input_events) != inputs)
        throw std::runtime_error("MK2 coin pair admission requires synchronized actual CPU/DSP/board prefix");
    main_horizon_ = dsp.main_horizon_ = board.main_horizon_ = 5077311;
}

void DcsBootMachine::AdmitStartWait(BoardBootControls& board) {
    // Original C7/C8 prefix (SHA-256: input events, DSP76, device, IO 28 + board scalars; mk2_oracle_digest.hpp).
    static constexpr char inputs[]="f7f4c86e0af31609f1fcf362156766385bcbaaf2541a683431431a9bcf296761", registers[]="9ed2928e3848df5e33adfd1f760dd5b7f380f35b1b726a9ee132c0e3a619bbb8",
        device[]="d82efd000991aa55b418ad5c7fc24239f44a2a3dc2b9f1af57371967ac63368e", board_state[]="b231e4a26cbb0a79e26273c5be7501f31e57312d85af3081238bf2d07c07b9dc";
    if (main_horizon_ != 5077311 || dsp.main_horizon_ != 5077311 || board.main_horizon_ != 5077311 ||
        last_main_ != 5077311 || scanline_events_ != 12829 || board.scanline_events != 12829 ||
        dma_completions_ != 1490 || dma_deadline_subticks_ != 812607865 ||
        dsp.InstructionCount() != 8103647 || WordsDigest(dsp.Registers()) != registers || WordsDigest(dsp.DeviceState()) != device ||
        dsp.sport_transfers.size() != 92 || dsp.sport_dma_positions.size() != 92 ||
        BoardDigest(board,{board.vcount,board.display_address,board.dma_command}) != board_state ||
        board.input0 != 0xffff || board.input1 != 0xffff || board.input2 != 0xffff ||
        board.requested_p1_high_punch || board.requested_coin1 || board.requested_start1 || board.dip_switches != 0xfc7d ||
        board.protection.Read()!=0 || RowsDigest(board.input_events) != inputs)
        throw std::runtime_error("MK2 Start wait requires the synchronized actual C7 CPU/DSP/board prefix");
    main_horizon_ = dsp.main_horizon_ = board.main_horizon_ = 6087360;
}

void DcsBootMachine::AdmitWaitAfterC8(BoardBootControls& board, uint64_t end_cycle) {
    // Original C7/C8 prefix (SHA-256: input events, DSP76, device, IO 28 + board scalars; mk2_oracle_digest.hpp).
    static constexpr char inputs[]="f7f4c86e0af31609f1fcf362156766385bcbaaf2541a683431431a9bcf296761", registers[]="2b369756ad416352e2d06c6fe3ea07c5bf84671a72470ac96a690b07c5e740f0",
        device[]="87e66abfc6720ca3442ab9404f3f6f98632b35ab72364df8b9bdb0872a57fdca", board_state[]="09043ff9c511024622473aff38bfe1d0b3d53cd12104ec85558f8deac334bf8a";
    if (main_horizon_ != 6087360 || dsp.main_horizon_ != 6087360 || board.main_horizon_ != 6087360 ||
        last_main_ != 6087360 || scanline_events_ != 15384 || board.scanline_events != 15384 ||
        sport_deadline_ticks_ != 98032570 || dma_deadline_subticks_ != 973990832 || dma_completions_ != 3250 ||
        dsp.InstructionCount() != 9719685 || WordsDigest(dsp.Registers()) != registers || WordsDigest(dsp.DeviceState()) != device ||
        dsp.sport_transfers.size() != 113 || dsp.sport_dma_positions.size() != 113 ||
        BoardDigest(board,{board.vcount,board.display_address,board.active_updates,board.dma_command,board.video_control}) != board_state ||
        board.input0 != 0xffff || board.input1 != 0xffff || board.input2 != 0xffff ||
        board.dip_switches != 0xfc7d || board.requested_p1_high_punch || board.requested_coin1 || board.requested_start1 ||
        board.protection.Read()!=0 || RowsDigest(board.input_events) != inputs)
        throw std::runtime_error("MK2 next wait requires the synchronized actual C8 CPU/DSP/board prefix");
    main_horizon_ = dsp.main_horizon_ = board.main_horizon_ = end_cycle;
}

void DcsBootMachine::AdmitNextWait(BoardBootControls& board) { AdmitWaitAfterC8(board,7005800); }
void DcsBootMachine::AdmitPreStartWait(BoardBootControls& board) { AdmitWaitAfterC8(board,22081952); }
void DcsBootMachine::AdmitStartInput(BoardBootControls& board) { AdmitWaitAfterC8(board,23108559); }
void DcsBootMachine::AdmitStartRelease(BoardBootControls& board) { AdmitWaitAfterC8(board,24004881); }
void DcsBootMachine::AdmitSelection(BoardBootControls& board) { AdmitWaitAfterC8(board,30060282); }
void DcsBootMachine::AdmitFirstFight(BoardBootControls& board) { AdmitWaitAfterC8(board,150023965); }

void DcsBootMachine::AdmitValidCmosDelay(BoardBootControls& board) {
    const bool test_switch=board.dip_switches==kTestSwitchDsw;
    if (test_switch && !HasFullDcsRoms()) throw std::runtime_error("MK2 Test Switch requires the complete DCS ROMs");
    if (main_horizon_ != 3658091 || dsp.main_horizon_ != 3658091 || board.main_horizon_ != 3658091 ||
        last_main_ != (test_switch?kTestSwitchPostCycle:OperatorCycle(board.dip_switches,OperatorBoundary::Post)) ||
        scanline_events_ != board.scanline_events || dsp.boot_reloads ||
        dsp.rebooted_ || dsp.sport_enabled || link.ResetAsserted() || board.HostInputProviderActive() ||
        !board.input_events.empty() || board.requested_p1_high_punch || board.requested_coin1 ||
        board.requested_start1 || board.input0 != 0xffff || board.input1 != 0xffff ||
        board.input2 != 0xffff || (!test_switch && !OperatorDswSupported(board.dip_switches)))
        throw std::runtime_error("MK2 valid-CMOS delay requires the synchronized clean-delay entry");
    main_horizon_ = dsp.main_horizon_ = board.main_horizon_ =
        test_switch?kTestSwitchResetCycle:OperatorCycle(board.dip_switches,OperatorBoundary::Reset);
}

void DcsBootMachine::RunDspUntil(uint64_t ticks) {
    if (sport_deadline_ticks_ && ticks > sport_deadline_ticks_)
        throw std::runtime_error("ADSP scheduler crossed pending SPORT deadline");
    if (ticks < dsp_ticks_) {
        // schedule.cpp skips a CPU whose local time is already beyond the slice target.
        if (CoinPairAdmitted() || (OperatorHorizon(main_horizon_,OperatorBoundary::Setup)&&main_horizon_!=6883473) ||
            (OperatorHorizon(main_horizon_,OperatorBoundary::Draw)&&main_horizon_!=7316296)) return;
        throw std::runtime_error("ADSP clock moved backwards; requested " + std::to_string(ticks) +
            "; current " + std::to_string(dsp_ticks_) + "; main " + std::to_string(last_main_) +
            "; DMA deadline " + std::to_string(dma_deadline_subticks_));
    }
    // An idle DSP waits in a loop of a few instructions that changes nothing (its sound OS polls a register the serial
    // port moves). Three arrivals in a row at one PC by a backward transfer, the last two with the same WaitState and
    // nothing waiting on the link: the instructions between them read and changed nothing that can move before this
    // slice ends, so its remaining whole turns of the loop are counted, not run. The clocks, the instruction counts and
    // the place in the loop come out as if every instruction had run. kWaitRest: backward transfers passed over after
    // a state that differed (a loop at work).
    static constexpr unsigned kWaitRest = 64;
    uint16_t before = dsp.pc, turn_pc = 0xffff;
    uint64_t turn_at = 0;
    unsigned rest = 0;
    bool have = false;
    while (ticks - dsp_ticks_ >= 10) {
        const uint64_t serial_count = dsp.SerialEventCount();
        if (!link.ResetAsserted()) { dsp.SetInstructionTicks(dsp_ticks_); dsp.ExecuteInstruction(); }
        if (dsp.sport_timer_cancelled) {
            sport_deadline_ticks_ = 0;
            dsp.sport_timer_cancelled = false;
        }
        if (dsp.SerialEventCount() != serial_count && dsp.sport_enabled)
            sport_deadline_ticks_ = dsp_ticks_ + 768000; // 240 mono samples at 32 us.
        dsp_ticks_ += 10;
        const bool irq2_was_asserted = link.Irq2Asserted();
        const bool irq2_clear_delivered = link.RetireDspInstruction();
        if (dsp.ApplyPendingReset() || (irq2_was_asserted && irq2_clear_delivered)) break;
        const uint16_t pc = dsp.pc;
        if (pc < before) {
            const uint64_t count = dsp.InstructionCount();
            if (rest) { --rest; have = false; turn_pc = 0xffff; }
            else if (pc != turn_pc) { turn_pc = pc; have = false; }
            else if (!have) { dsp.WaitState(wait_state_); have = true; }
            else {
                dsp.WaitState(wait_now_);
                const uint64_t turn = count - turn_at;
                if (wait_now_ == wait_state_ && link.Quiet() && turn) {
                    const uint64_t turns = std::min((ticks - dsp_ticks_) / 10, dsp.InstructionRoom()) / turn;
                    if (turns) { dsp_ticks_ += turns * turn * 10; dsp.SkipInstructions(turns * turn, dsp_ticks_ - 10); }
                }
                rest = kWaitRest; have = false; turn_pc = 0xffff;
            }
            turn_at = count;
        }
        before = pc;
    }
}

void DcsBootMachine::MainInstruction(uint64_t cycles, bool host_write, uint16_t value, bool input_line_event) {
    // Finite cold boot through the larger cooperative batch.
    if (cycles > main_horizon_) throw std::runtime_error("MK2 scheduler outside qualified loaded drawing frontier");
    if (cycles <= last_main_)
        throw std::runtime_error("Invalid main CPU instruction clock");
    const uint64_t instruction_start_ticks = last_main_ * 16;
    last_main_ = cycles;
    const uint64_t ticks = cycles * 16;
    if (host_write) {
        // The synchronized bus write precedes timers crossed by this opcode's cost.
        RunDspUntil(std::min({ticks, NextTimerTicks(),
            sliced_ && !link.ResetAsserted() ? target_ticks_ : ticks}));
        const bool was_reset = link.ResetAsserted();
        link.HostWrite(0x01d01030, value);
        if (link.ResetAsserted()) { dsp.ReloadBoard(); sport_irq1_clear_ticks_ = 0; }
        else if (was_reset) dsp.ResetCore();
        link.Synchronize();
        boost_expiry_ticks_ = instruction_start_ticks + 500; // dcs_delayed_data_w: 5 us.
    }
    // screen.cpp starts at vblank line 274; TMS line 0 is 15 * 506 / 8 MHz later.
    // The +1 attosecond timer epsilon cannot change either CPU's whole-cycle budget here.
    const uint64_t dsp_before_timers = dsp_ticks_;
    const auto catch_up_to_main_target = [&](uint64_t target) {
        // A timer in this instruction may have already taken the DSP past the target.
        if (target >= dsp_ticks_ || dsp_ticks_ == dsp_before_timers) RunDspUntil(target);
    };
    while (cycles >= NextTimerTicks() / 16) {
        const uint64_t deadline = NextTimerTicks();
        catch_up_to_main_target(std::min(ticks, deadline));
        RunDspUntil(deadline);
        if (dma_deadline_subticks_ && deadline == dma_deadline_subticks_ / 10) {
            dma_deadline_subticks_ = 0;
            if(live_host_domain_ && dma_completions_==std::numeric_limits<unsigned>::max())throw std::overflow_error("MK2 live DMA completion counter overflow");
            ++dma_completions_;
        }
        if (sport_deadline_ticks_ && deadline == sport_deadline_ticks_) {
            dsp.SportTransfer(deadline);
            if (dsp.sport_irq1_asserted) sport_irq1_clear_ticks_ = deadline + 10;
            sport_deadline_ticks_ += 768000;
        }
        if (sport_irq1_clear_ticks_ && deadline == sport_irq1_clear_ticks_) {
            dsp.ClearSportIrq1(deadline);
            sport_irq1_clear_ticks_ = 0;
        }
        if (deadline == next_sound_ticks_) {
            dsp.DacUpdate(deadline);
            next_sound_ticks_ += 2000000; // Original sound_manager 50 Hz update.
        }
        if (deadline == next_scanline_ticks_) {
            if(live_host_domain_ && scanline_events_==std::numeric_limits<unsigned>::max())throw std::overflow_error("MK2 live scanline counter overflow");
            ++scanline_events_;
            next_scanline_ticks_ += 6325; // 506 pixels / 8 MHz, exactly 63.25 us.
        }
        // Returning to timeslice() expires an old DCS quantum before the next timer.
        sliced_ = ticks < boost_expiry_ticks_;
    }
    if (host_write || input_line_event) {
        // Respect the old slice target; a reset-held CPU only advances its suspended clock.
        if (!host_write)
            catch_up_to_main_target(sliced_ && !link.ResetAsserted() ? std::min(ticks, target_ticks_) : ticks);
        target_ticks_ = dsp_ticks_ + 50;
        main_limit_ = cycles + (target_ticks_ > ticks ? (target_ticks_ - ticks) / 16 : 0);
        // schedule.cpp expires quanta on timeslice re-entry, not inside its quantum loop.
        // These input timers end that loop even when the input line is already clear.
        sliced_ = ticks < boost_expiry_ticks_;
    } else if (sliced_ && !link.ResetAsserted() && cycles >= main_limit_) {
        // Whole main instructions can overshoot the slice; shorter slices pull
        // back the target, matching schedule.cpp's local-time rule.
        const uint64_t end = std::min(ticks, target_ticks_);
        catch_up_to_main_target(end);
        target_ticks_ = dsp_ticks_ + 50;
        main_limit_ = cycles + (target_ticks_ > ticks ? (target_ticks_ - ticks) / 16 : 0);
        // A long TMS instruction may consume several quanta. Catch the DSP up
        // through slices with no whole main cycle before starting another instruction.
        while (main_limit_ == cycles) {
            RunDspUntil(target_ticks_);
            target_ticks_ = dsp_ticks_ + 50;
            main_limit_ = cycles + (target_ticks_ > ticks ? (target_ticks_ - ticks) / 16 : 0);
        }
    }
}
}

namespace mk2 {
void DcsBootMachine::AdmitValidCmosStartup(BoardBootControls& board,unsigned profile) {
    const bool test_switch=board.dip_switches==kTestSwitchDsw;
    if (test_switch) {
        if (profile || !HasFullDcsRoms()) throw std::runtime_error("MK2 Test Switch requires the factory startup profile");
    } else OperatorProfileIndex(board.dip_switches,profile);
    const auto reset=test_switch?kTestSwitchResetCycle:OperatorCycle(board.dip_switches,OperatorBoundary::Reset);
    // Native executed-instruction clock; the original concurrent terminal cache is one behind.
    // FC7D factory DSP76 (SHA-256; the native executed clock as stored).
    static constexpr char expected[]="1bd2d4d9635e3672a069a45b3e690d887345fbd97bc17e846033fac1bf0a5c89";
    if(main_horizon_!=reset || dsp.main_horizon_!=reset || board.main_horizon_!=reset ||
       last_main_!=reset || scanline_events_!=board.scanline_events ||
       (test_switch?!TestSwitchDspBoundary(dsp.Registers(),1):
        board.dip_switches==0xfc7d?WordsDigest(dsp.Registers())!=expected:!OperatorDspBoundary(dsp.Registers(),board.dip_switches,OperatorBoundary::Reset)) ||
       dsp.boot_reloads || dsp.rebooted_ || !dsp.sport_enabled || link.ResetAsserted() ||
       board.HostInputProviderActive() || !board.input_events.empty() || board.requested_p1_high_punch ||
       board.requested_coin1 || board.requested_start1 || board.input0!=0xffff || board.input1!=0xffff ||
       board.input2!=0xffff || (!test_switch && !OperatorDswSupported(board.dip_switches)))
        throw std::runtime_error("MK2 valid-CMOS startup requires the synchronized measured clean-boot prefix");
    if (test_switch) {
        dsp.service_irq_domain_=true;
        main_horizon_=dsp.main_horizon_=board.main_horizon_=
            live_detail::ChunkDeadline(last_main_,kTestSwitchStartupCycle);
        return;
    }
    // Fixed original second cooperative yield, not an arbitrary caller-selected horizon.
    operator_startup_profile_=profile;
    operator_startup_horizon_=OperatorCycle(board.dip_switches,OperatorBoundary::Setup,profile);
    main_horizon_=dsp.main_horizon_=board.main_horizon_=operator_startup_horizon_;
}
}

namespace mk2 {
void DcsBootMachine::AdmitValidCmosDraw(BoardBootControls& board) {
    // FC7D factory DSP76 (SHA-256; the native executed clock as stored) and IO 17/18.
    static constexpr char expected[]="1dbb6bc4da7d53e90cffa17b4f3f288ec76d6af6202b6aaac3f19bd91a9f688e";
    if(main_horizon_!=OperatorCycle(board.dip_switches,OperatorBoundary::Setup,operator_startup_profile_) || dsp.main_horizon_!=OperatorCycle(board.dip_switches,OperatorBoundary::Setup,operator_startup_profile_) || board.main_horizon_!=OperatorCycle(board.dip_switches,OperatorBoundary::Setup,operator_startup_profile_) ||
       last_main_!=OperatorCycle(board.dip_switches,OperatorBoundary::Setup,operator_startup_profile_) || scanline_events_!=board.scanline_events || (board.dip_switches==0xfc7d&&!operator_startup_profile_?WordsDigest(dsp.Registers())!=expected:!OperatorDspBoundary(dsp.Registers(),board.dip_switches,OperatorBoundary::Setup,operator_startup_profile_)) ||
       dsp.boot_reloads!=1 || !dsp.rebooted_ || link.ResetAsserted() || dma_deadline_subticks_ || dma_completions_ ||
       board.HostInputProviderActive() || !board.input_events.empty() || board.requested_p1_high_punch ||
       board.requested_coin1 || board.requested_start1 || board.input0!=0xffff || board.input1!=0xffff ||
       board.input2!=0xffff || !OperatorDswSupported(board.dip_switches) || WordsDigest({board.io[17],board.io[18]})!="5eda06a451bcb43e86f85cd9b2897b6b5cf0eeb06f36ed992422cf4f53214c03")
    {
        std::string differences;
        const auto actual=dsp.Registers();
        differences=" DSP76="+(actual.size()==76?WordsDigest(actual):"size "+std::to_string(actual.size()));
        throw std::runtime_error("MK2 valid-CMOS draw requires synchronized measured task-setup prefix; horizons="+std::to_string(main_horizon_)+"/"+std::to_string(dsp.main_horizon_)+"/"+std::to_string(board.main_horizon_)+" last="+std::to_string(last_main_)+" scans="+std::to_string(scanline_events_)+"/"+std::to_string(board.scanline_events)+" io="+std::to_string(board.io[17])+"/"+std::to_string(board.io[18])+differences);
    }
    operator_draw_horizon_=OperatorCycle(board.dip_switches,OperatorBoundary::Draw,operator_startup_profile_);
    operator_dma_start_=OperatorCycle(board.dip_switches,OperatorBoundary::Dma,operator_startup_profile_);
    main_horizon_=dsp.main_horizon_=board.main_horizon_=operator_draw_horizon_;
}
}

namespace mk2 {
void DcsBootMachine::AdmitValidCmosHost(BoardBootControls& board) {
    // Native executed clock is774112. Original concurrent debugger cache is774111; neither state is adjusted.
    // FC7D factory DSP76 (SHA-256; the native executed clock as stored).
    static constexpr char expected[]="4422a597ea58aac7756d9e4d4b6a3c1a6005528a5fcf024be830ef58ea644cdf";
    // IO 28 + vcount, display address, active updates, video control, DMA command (SHA-256).
    static constexpr char board_state[]="c12027618cc2cddb1f8b4198d1a6bdaa8b5def18c5f18633c716d6de14f56fe4";
    const uint64_t first_draw=OperatorCycle(board.dip_switches,OperatorBoundary::Draw,operator_startup_profile_);
    if(valid_cmos_host_ || main_horizon_!=first_draw || dsp.main_horizon_!=first_draw || board.main_horizon_!=first_draw ||
       last_main_!=first_draw || (board.dip_switches==0xfc7d&&!operator_startup_profile_?WordsDigest(dsp.Registers())!=expected:!OperatorDspBoundary(dsp.Registers(),board.dip_switches,OperatorBoundary::Draw,operator_startup_profile_)) || scanline_events_!=18493 || board.scanline_events!=18493 ||
       dsp.boot_reloads!=1 || !dsp.rebooted_ || link.ResetAsserted() || dma_deadline_subticks_ || dma_completions_!=1 ||
       sport_deadline_ticks_!=OperatorSportDeadline(board.dip_switches,operator_startup_profile_) || BoardDigest(board,{board.vcount,board.display_address,board.active_updates,board.video_control,board.dma_command})!=board_state ||
       !board.HostInputProviderActive() || !board.input_events.empty() || board.requested_p1_high_punch ||
       board.requested_coin1 || board.requested_start1 || board.input0!=0xffff || board.input1!=0xffff ||
       board.input2!=0xffff || !OperatorDswSupported(board.dip_switches))
    {
        std::string differences;const auto actual=dsp.Registers();
        differences=" DSP76(native clock)="+(actual.size()==76?Dsp76Digest(actual,true):"size "+std::to_string(actual.size()))+
            " expected="+(board.dip_switches==0xfc7d&&!operator_startup_profile_?std::string(expected)+" (native)":
            std::string(OperatorDspDigest(board.dip_switches,OperatorBoundary::Draw,operator_startup_profile_)));
        throw std::runtime_error("MK2 valid host requires the exact original first-draw CPU/device origin and exact native DSP76 clock; main="+
            std::to_string(last_main_)+" sport="+std::to_string(sport_deadline_ticks_)+" scans="+std::to_string(scanline_events_)+
            " vcount="+std::to_string(board.vcount)+" active="+std::to_string(board.active_updates)+differences);
    }
    // Timers, pending DMA IRQ, guest clocks and all CPU/DSP state remain unchanged.
    main_horizon_=dsp.main_horizon_=board.main_horizon_=150023965;
    valid_cmos_host_=true;
}
}

namespace mk2 {
void AdspBoot::DiscardLiveObservations() {
    if((!live_host_domain_ && !service_irq_domain_) || observe_trace_ || !trace.empty())
        throw std::runtime_error("MK2 live DSP drain requires bounded observation mode");
    const std::array<uint64_t,6> sizes{reboot_events.size(),serial_events.size(),sport_pcm.size(),sport_transfers.size(),sport_dma_positions.size(),sport_irq1_events.size()};
    for(unsigned n=0;n<sizes.size();++n)
        if(discarded_observations_[n]>std::numeric_limits<uint64_t>::max()-sizes[n])throw std::overflow_error("MK2 live observation counter overflow");
    for(unsigned n=0;n<sizes.size();++n)discarded_observations_[n]+=sizes[n];
    reboot_events.clear();serial_events.clear();sport_pcm.clear();sport_transfers.clear();sport_dma_positions.clear();sport_irq1_events.clear();
    DiscardObservedAccesses();
}
void DcsBootMachine::RenewTestSwitch(BoardBootControls& board) {
    if(!TestSwitchAdmitted() || board.dip_switches!=kTestSwitchDsw ||
       board.main_horizon_!=main_horizon_ || dsp.main_horizon_!=main_horizon_ ||
       board.scanline_events!=scanline_events_ || !board.HostInputProviderActive() ||
       dsp.TraceObservationEnabled() || !dsp.trace.empty())
        throw std::runtime_error("MK2 Test Switch renewal requires its own synchronized input and device state");
    const auto next=live_detail::ChunkDeadline(last_main_,last_main_+live_detail::kRequestCycles);
    dsp.DiscardLiveObservations();
    board.input_events.clear();
    main_horizon_=dsp.main_horizon_=board.main_horizon_=next;
}
void DcsBootMachine::AdmitValidCmosLiveHost(BoardBootControls& board,uint64_t deadline) {
    if(deadline<live_detail::kCooperativeSlack || live_detail::ChunkDeadline(last_main_,deadline-live_detail::kCooperativeSlack)!=deadline ||
       live_host_domain_ || live_generation_ || dsp.live_host_domain_ || board.live_host_domain_ || dsp.TraceObservationEnabled() || !dsp.trace.empty())
        throw std::runtime_error("MK2 live admission requires an increasing bounded grant and untouched original first draw");
    const auto instruction_limit=live_detail::DspInstructionCeiling(dsp.InstructionCount(),dsp_ticks_,deadline);
    AdmitValidCmosHost(board); // Exact existing DSP76/timer/board/origin guard; no guest execution or normalization.
    live_host_domain_=dsp.live_host_domain_=board.live_host_domain_=true;
    live_generation_=1;dsp.live_instruction_ceiling_=instruction_limit;
    main_horizon_=dsp.main_horizon_=board.main_horizon_=deadline;
    board.live_last_input_tick_=last_main_*16;
    dsp.DiscardLiveObservations();
}
void DcsBootMachine::RenewValidCmosLiveHost(BoardBootControls& board,uint64_t generation,uint64_t deadline) {
    if(!ValidCmosLiveHostAdmitted() || generation!=live_generation_ || live_generation_==std::numeric_limits<uint64_t>::max() ||
       !board.live_host_domain_ || board.main_horizon_!=main_horizon_ || !board.HostInputProviderActive() ||
       board.scanline_events!=scanline_events_ || deadline<live_detail::kCooperativeSlack ||
       live_detail::ChunkDeadline(last_main_,deadline-live_detail::kCooperativeSlack)!=deadline ||
       dsp.TraceObservationEnabled() || !dsp.trace.empty())
        throw std::runtime_error("MK2 live renewal requires its own synchronized generation and bounded increasing grant");
    const auto instruction_limit=live_detail::DspInstructionCeiling(dsp.InstructionCount(),dsp_ticks_,deadline);
    ++live_generation_;dsp.live_instruction_ceiling_=instruction_limit;
    main_horizon_=dsp.main_horizon_=board.main_horizon_=deadline;
    board.DiscardLiveInputEvents();dsp.DiscardLiveObservations();
}
}
