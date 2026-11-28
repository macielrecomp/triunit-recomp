#include "mk2_valid_cmos_draw_boundary.hpp"
#include "mk2_valid_cmos_startup.hpp"
#pragma once
#include "mk2_oracle_digest.hpp"
#include "mk2_pixblt.hpp"
#include "mk2_widescreen.hpp"
#include "mk2_adsp.hpp"
#include <initializer_list>
#include <string>

namespace mk2 {
struct BootCheckpoint { std::string name; ResetTrace cpu; std::vector<uint64_t> dsp; };
struct BootExecution {
    unsigned fill_remaining=0;
    std::array<uint32_t,10> fill_b{};
    uint32_t last_instruction_pc=0;
    uint64_t last_instruction_cycles=0;
    bool shift_loaded=false;
    BitmapTextBlit bitmap;
};
struct BootContinuation {
    ResetTrace ready;
    BootVideo video;
    std::vector<uint16_t> main_ram;
    std::vector<PixbltAccess> bus;
    std::vector<BootCheckpoint> points;
    bool observe_history = true;
    BootExecution execution;
};
// Fixed-translation primitives; no instruction decoding or branch dispatch.
class BootOps {
    unsigned& fill_remaining_;
    std::array<uint32_t, 10>& fill_b_;
    uint32_t& last_instruction_pc_;
    uint64_t& last_instruction_cycles_;
    bool recording_interrupt_ = false;
    bool& shift_loaded_;
    bool dma_line_event_ = false;
    uint32_t FighterPointer(uint32_t address)const{
        if(address<0x01000000 || address>0x013fffe0 || (address&15))return 0;
        const auto word=(address-0x01000000)/16;
        return out.main_ram[word]|(uint32_t(out.main_ram[word+1])<<16);
    }
public:
    const Program& program;
    DcsBootMachine& machine;
    BootContinuation& out;
    ResetTrace& s;
    BootOps(const Program& p, DcsBootMachine& m, BootContinuation& result)
        : fill_remaining_(result.execution.fill_remaining),fill_b_(result.execution.fill_b),
          last_instruction_pc_(result.execution.last_instruction_pc),last_instruction_cycles_(result.execution.last_instruction_cycles),
          shift_loaded_(result.execution.shift_loaded),program(p), machine(m), out(result), s(result.ready) {}
    void ObservePresentation() {
        if(out.video.BackdropEnabled()) {
            if(s.next_pc==0xff805020) {
                const auto mode=out.main_ram[(0x010601d0-0x01000000)/16];
                const auto list=s.a[0];
                out.video.world_object_list=list==0x0105d480;
                const auto table=(0x0105d0c0-0x01000000)/16;
                const bool tower=mode==7 && out.main_ram[table]==0x4f00 && out.main_ram[table+1]==0xffa0;
                // The arena table orders the parallax lists around the world-object and HUD lists.
                out.video.backdrop_list=(list>=0x0105d380 && list<=0x0105d460 && !(list&31)) ||
                    (list==0x0105d480 && (mode==4 || mode==13 || tower));
            } else if(s.next_pc==0xff805040){
                out.video.backdrop_list=false;
                out.video.world_object_list=false;
            }
        }
        if(!out.video.BloodEnabled())return;
        if(s.next_pc==0xff818d00) {
            out.video.ClearBlood();
            return;
        }
        const auto word=[&](uint32_t bit){return out.main_ram[(bit-0x01000000)/16];};
        const auto mode=word(0x010601d0);
        if(s.next_pc==0xff805020 && s.a[0]==0x0105d480) {
            // Each object stages six dwords. Compose when the first world-object DMA drains.
            out.video.QueueBloodDraw(s.a[1]-0xc0,s.a[5],s.a[4],s.a[14],s.a[13],
                                     mode==2 || mode==5 || mode==6 || mode==7);
        } else if(s.next_pc==0xff9b0e60 && s.a[9]==0xff9b0120 && (s.board.dip_switches&0x4000) &&
                  (mode==2 || mode==5 || mode==6 || mode==7)) {
            const auto object=s.a[8];
            if(object>=0x01000000 && object<=0x013ffeb0 && !(object&15) &&
               word(object+0x100)==0x8208 && word(object+0x110)==0x0410)
                out.video.KeepBlood(int16_t(word(object+0x90)),int16_t(word(object+0xb0)),
                                    (word(object+0xf0)&0x10)!=0,word(object+0x140));
        }
    }
    uint64_t FighterOwner()const{
        return out.video.world_object_list?out.video.FighterGeneration(s.a[0]):0;
    }
    bool CanSuspend() const { return !recording_interrupt_ && !dma_line_event_; }
    void Finish(uint32_t next, unsigned cost, bool host_write = false, uint16_t value = 0,
                bool input_line_event = false) {
        if (shadow) { s.next_pc = next; return; }
        last_instruction_pc_ = s.next_pc; last_instruction_cycles_ = s.cycles;
        s.next_pc = next; s.cycles += cost; ++s.instructions;
        const auto completed = machine.DmaCompletions();
        machine.MainInstruction(s.cycles, host_write, value, input_line_event || dma_line_event_);
        dma_line_event_ = false;
        if (machine.DmaCompletions() != completed) {
            s.board.dma_command &= 0x7fff;
            s.board.io[18] |= 2;
        }
        machine.AdvanceVideo(s.board, out.video);
    }
    void Checkpoint(const char* name) {
        if (out.observe_history) out.points.push_back({name, s, machine.dsp.Registers()});
    }
    void Zero(uint32_t value) {
        s.status = (s.status & ~uint32_t(0x20000000)) | (value ? 0 : 0x20000000);
    }
    void Load(uint32_t& reg, uint32_t value, uint32_t next, unsigned cost) {
        reg = value; s.status = (s.status & ~uint32_t(0xb0000000)) | (value & 0x80000000);
        Zero(value); Finish(next, cost);
    }
    void Clear(uint32_t& reg, uint32_t next) { reg = 0; Zero(reg); Finish(next, 1); }
    void Arithmetic(uint32_t& reg, uint32_t value, bool subtract, uint32_t next, unsigned cost) {
        const uint32_t old = reg, result = subtract ? old - value : old + value;
        s.status = (s.status & ~uint32_t(0xf0000000)) | (result & 0x80000000);
        if (subtract ? value > old : result < old) s.status |= 0x40000000;
        if ((subtract ? old ^ value : ~(old ^ value)) & (old ^ result) & 0x80000000)
            s.status |= 0x10000000;
        reg = result; Zero(reg); Finish(next, cost);
    }
    void Shift(uint32_t& reg, unsigned count, bool right, uint32_t next) {
        if (count > 31) throw std::runtime_error("MK2 boot shift outside translated counts");
        const uint32_t old = reg;
        s.status &= ~uint32_t(0x60000000);
        if (count) {
            s.status |= ((old >> (right ? count - 1 : 32 - count)) & 1) << 30;
            reg = right ? old >> count : old << count;
        }
        Zero(reg); Finish(next, 1);
    }
    void AddCarry(uint32_t& reg, uint32_t value, uint32_t next) {
        const uint32_t old = reg, result = old + value + ((s.status >> 30) & 1);
        s.status = (s.status & ~uint32_t(0xf0000000)) | (result & 0x80000000);
        if (~value < old) s.status |= 0x40000000;
        if (~(old ^ value) & (old ^ result) & 0x80000000) s.status |= 0x10000000;
        reg = result; Zero(reg); Finish(next, 1);
    }
    void SubtractBorrow(uint32_t& reg, uint32_t value, uint32_t next) {
        const uint32_t old = reg, result = old - value - ((s.status >> 30) & 1);
        s.status = (s.status & ~uint32_t(0xf0000000)) | (result & 0x80000000);
        if (value > old) s.status |= 0x40000000;
        if ((old ^ value) & (old ^ result) & 0x80000000) s.status |= 0x10000000;
        reg = result; Zero(reg); Finish(next, 1);
    }
    void ShiftArithmetic(uint32_t& reg, unsigned count, bool right, uint32_t next) {
        if (count > 31) throw std::runtime_error("MK2 arithmetic shift count outside translated scope");
        const uint32_t old = reg;
        s.status &= ~uint32_t(right ? 0xe0000000 : 0xf0000000);
        if (!count) {}  // 34010ops.hxx: k=0 leaves Rd, clears C (and V for SLA), sets N/Z.
        else if (right) {
            s.status |= ((old >> (count - 1)) & 1) << 30;
            reg = (old >> count) | ((old & 0x80000000) ? (~uint32_t(0) << (32 - count)) : 0);
        } else {
            s.status |= ((old >> (32 - count)) & 1) << 30;
            reg = old << count;
            const uint32_t discarded = old >> (31 - count);
            if (discarded != ((old & 0x80000000) ? uint32_t((uint64_t(1) << (count + 1)) - 1) : 0))
                s.status |= 0x10000000;
        }
        s.status |= reg & 0x80000000; Zero(reg); Finish(next, right ? 1 : 3);
    }
    void SubtractXY(uint32_t& reg, uint32_t value, uint32_t next) {
        const int16_t dx = int16_t(reg), dy = int16_t(reg >> 16);
        const int16_t sx = int16_t(value), sy = int16_t(value >> 16);
        s.status &= ~uint32_t(0xf0000000);
        if (sx == dx) s.status |= 0x80000000;
        if (sy > dy) s.status |= 0x40000000;
        if (sy == dy) s.status |= 0x20000000;
        if (sx > dx) s.status |= 0x10000000;
        reg = uint16_t(dx - sx) | (uint32_t(uint16_t(dy - sy)) << 16); Finish(next, 1);
    }
    void AddXY(uint32_t& reg, uint32_t value, uint32_t next) {
        const uint16_t x = uint16_t(reg) + uint16_t(value);
        const uint16_t y = uint16_t(reg >> 16) + uint16_t(value >> 16);
        s.status &= ~uint32_t(0xf0000000);
        if (!x) s.status |= 0x80000000;
        if (y & 0x8000) s.status |= 0x40000000;
        if (!y) s.status |= 0x20000000;
        if (x & 0x8000) s.status |= 0x10000000;
        reg = x | (uint32_t(y) << 16); Finish(next, 1);
    }
    void SignExtend(uint32_t& reg, unsigned field, uint32_t next) {
        const unsigned width = FieldWidth(field);
        if (width != 32) {
            const uint32_t mask = (uint32_t(1) << width) - 1;
            reg &= mask;
            if (reg & (uint32_t(1) << (width - 1))) reg |= ~mask;
        }
        s.status = (s.status & ~uint32_t(0xa0000000)) | (reg & 0x80000000);
        Zero(reg); Finish(next, 3);
    }
    void Rotate(uint32_t& reg, unsigned count, uint32_t next) {
        if (count > 31) throw std::runtime_error("MK2 boot rotate outside translated counts");
        const uint32_t old = reg;
        s.status &= ~uint32_t(0x60000000);
        if (count) {
            s.status |= ((old >> (32 - count)) & 1) << 30;
            reg = (old << count) | (old >> (32 - count));
        }
        Zero(reg); Finish(next, 1);
    }
    void LeadingOne(uint32_t value, uint32_t& dest, uint32_t next) {
        Zero(value); dest = 0;
        if (value) while (!(value & 0x80000000)) { ++dest; value <<= 1; }
        Finish(next, 1);
    }
    void MultiplyUnsigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {
        auto* regs = bank_b ? s.b.data() : s.a.data();
        if (dest >= 14) throw std::runtime_error("MK2 boot multiply SP alias is not translated");
        const unsigned width = FieldWidth(1);
        if (width != 32) value &= (uint32_t(1) << width) - 1;
        const uint64_t product = uint64_t(value) * regs[dest];
        Zero(product ? 1 : 0);
        regs[dest] = uint32_t(product >> 32); regs[dest | 1] = uint32_t(product);
        Finish(next, 21);
    }
    void MultiplySigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {
        auto* regs = bank_b ? s.b.data() : s.a.data();
        if (dest >= 14) throw std::runtime_error("MK2 signed multiply SP alias is not translated");
        const unsigned width = FieldWidth(1);
        if (width != 32) {
            const uint32_t mask = (uint32_t(1) << width) - 1;
            value &= mask;
            if (value & (uint32_t(1) << (width - 1))) value |= ~mask;
        }
        const int64_t product = int64_t(int32_t(value)) * int32_t(regs[dest]);
        const uint32_t high = uint32_t(uint64_t(product) >> 32);
        s.status = (s.status & ~uint32_t(0xa0000000)) | (high & 0x80000000);
        Zero(product ? 1 : 0);
        regs[dest] = high; regs[dest | 1] = uint32_t(product);
        Finish(next, 20);
    }
    void DivideUnsigned(uint32_t value, unsigned dest, bool bank_b, uint32_t next) {
        auto* regs = bank_b ? s.b.data() : s.a.data();
        if (dest >= 14 || (dest & 1))
            throw std::runtime_error("MK2 unsigned divide outside translated register pair");
        s.status &= ~uint32_t(0x30000000);
        const uint64_t dividend = (uint64_t(regs[dest]) << 32) | regs[dest + 1];
        if (!value || dividend / value > 0xffffffffu) s.status |= 0x10000000;
        else {
            regs[dest] = uint32_t(dividend / value);
            regs[dest + 1] = uint32_t(dividend % value);
            Zero(regs[dest]);
        }
        Finish(next, 37);
    }
    void MoveX(uint32_t source, uint32_t& dest, uint32_t next) {
        dest = (dest & 0xffff0000) | (source & 0xffff); Finish(next, 1);
    }
    void SetField(uint32_t field, uint32_t next, bool field1 = false) {
        const unsigned shift = field1 ? 6 : 0;
        s.status = (s.status & ~(0x3fu << shift)) | (field << shift);
        Finish(next, 1 + unsigned(field1));
    }
    void ExchangeField(uint32_t& reg, bool field1, uint32_t next) {
        const unsigned shift = field1 ? 6 : 0;
        const uint32_t previous = (s.status >> shift) & 0x3f;
        s.status = (s.status & ~(0x3fu << shift)) | ((reg & 0x3f) << shift);
        reg = previous;
        Finish(next, 1);
    }
    void Exchange(unsigned reg) {
        const auto target = s.b.at(reg); s.b[reg] = s.next_pc + 16; Finish(target, 2);
    }
    // Widescreen W1 shadow drawer (mk2_task_batch.cpp): the generated list drawer runs a second time with the clip widened
    // to the 512 VRAM columns. Its main RAM writes are journaled and undone after it, any other access only marks the pass
    // faulty (reads return 0), no cycle or instruction is counted, no hook runs, and the DMA entries it stages draw the margin columns.
    bool shadow=false,shadow_fault=false;std::vector<std::pair<size_t,uint16_t>> shadow_journal;
    void Record(char kind, uint32_t address, uint16_t value) {
        if (shadow) return;
        if (out.observe_history) out.bus.push_back({recording_interrupt_ ? last_instruction_pc_ : s.next_pc, address, value,
                           recording_interrupt_ ? last_instruction_cycles_ : s.cycles, kind});
    }
    uint16_t Read16(uint32_t address) {
        if (address & 15) throw std::runtime_error("MK2 boot word read is unaligned");
        uint16_t value;
        if (address >= 0xff800000) {
            const size_t offset = (address - 0xff800000) / 8;
            value = uint16_t(program.bytes.at(offset) | (uint16_t(program.bytes.at(offset + 1)) << 8));
        } else if (address >= 0x01000000 && address <= 0x013ffff0)
            value = out.main_ram.at((address - 0x01000000) / 16);
        else if (shadow) { shadow_fault = true; return 0; }   // the shadow reads only program ROM and main RAM
        else if (address >= 0x01400000 && address <= 0x0141fff0)
            value = s.cmos.at((address - 0x01400000) / 16);
        else if (address == 0x01480000) value = 0xffff;
        else if (address == 0x01d00000) value = machine.link.MainStatus();
        else if (address == 0x01a190e0 || address == 0x01a190f0 ||
                 (address >= 0x01a3d0c0 && address <= 0x01a3d0f0))
            value = s.board.protection.Read();
        else if (address == 0x01a191c0 || address == 0x01a191d0)
            value = s.board.protection.Shifted();
        else if (address == 0x01d9d1e0 || address == 0x01d9d1f0 ||
                 address == 0x01def920 || address == 0x01def930)
            value = s.board.protection.Constant();
        else if (address >= 0xc0000000 && address <= 0xc00001b0)
            value = s.board.io.at((address - 0xc0000000) / 16);
        else if (address == 0xc00001c0) value = s.board.HorizontalCount(s.cycles);
        else if (address == 0xc00001d0) value = s.board.vcount;
        else if (address == 0xc00001e0) value = s.board.display_address;
        else if (address >= 0x01a80000 && address <= 0x01a80110)
            value = out.video.ReadDma((address - 0x01a80000) / 16, s.board);
        else if (address >= 0x01800000 && address <= 0x0187fff0)
            value = out.video.ReadPalette(address);
        else if (address >= 0x02000000 && address <= 0x07fffff0) {
            if (program.graphics.size() != 0xc00000)
                throw std::runtime_error("MK2 graphics read requires the complete interleaved ROM region");
            const auto offset = (address - 0x02000000) / 16;
            const auto bank = (offset & 0x200000) ? 0x400000 : s.board.GfxBank0();
            const auto byte = bank + (offset & 0x1fffff) * 2;
            value = uint16_t(program.graphics.at(byte) | (uint16_t(program.graphics.at(byte + 1)) << 8));
        }
        else if (address <= 0x003ffff0) value = out.video.ReadVram(address, s.board);
        // T-unit main_map: this entire read window is unmapped with high pull-ups.
        else if (address >= 0x00400000 && address <= 0x00fffff0) value = 0xffff;
        else if (address == 0x01600000 || address == 0x01600010 || address == 0x01600020 ||
                 address == 0x01600030 || address == 0x01600040)
            value = s.board.ReadBootInput(address);
        else throw std::runtime_error("MK2 boot read outside translated memory: PC " +
                                      std::to_string(s.next_pc) + "; address " + std::to_string(address));
        Record('r', address, value); return value;
    }
    uint32_t Read8(uint32_t address) {
        const auto aligned = address & ~uint32_t(15), shift = address & 15;
        const auto word = shift > 8 ? Read32(aligned) : Read16(aligned);
        return (word >> shift) & 0xff;
    }
    uint32_t Read32(uint32_t address) {
        if (const auto shift = address & 15) {
            const auto aligned = address & ~uint32_t(15);
            const auto low = Read32(aligned), high = Read32(aligned + 32);
            return (low >> shift) | (high << (32 - shift));
        }
        const uint32_t low = Read16(address); return low | (uint32_t(Read16(address + 16)) << 16);
    }
    void Write16(uint32_t address, uint16_t value, unsigned dma_start_offset = 0) {
        if (address & 15) throw std::runtime_error("MK2 boot word write is unaligned");
        if (shadow) {
            if (address >= 0x01000000 && address <= 0x013ffff0) {
                auto& word=out.main_ram.at((address-0x01000000)/16);
                shadow_journal.emplace_back((address-0x01000000)/16,word);word=value;
            } else shadow_fault=true;
            return;
        }
        if (address >= 0xff800000 || (address >= 0x1f800000 && address <= 0x1ffffff0)) {
            if(program.bytes.size()!=0x100000)
                throw std::runtime_error("MK2 program ROM write requires the complete mapped region");
            // T-unit ROM windows have no write handler; preserve only the physical bus lane.
        } else if (address >= 0x01000000 && address <= 0x013ffff0) {
            out.main_ram.at((address - 0x01000000) / 16) = value;
            if (const auto low = s.ram.Find(address)) *low = (*low & 0xffff0000) | value;
            if (const auto high = s.ram.Find(address - 16)) *high = (*high & 0xffff) | (uint32_t(value) << 16);
        } else if (address >= 0x01400000 && address <= 0x0141fff0)
            s.WriteCmos((address - 0x01400000) / 16, value, 0xffff);
        else if (address == 0x00f20c60 || address == 0x00f20c70 ||
                 address == 0x00f42820 || address == 0x00f42830)
            s.board.protection.Write(value,0xffff);
        else if ((address >= 0x01a190e0 && address <= 0x01a190f0) ||
                 (address >= 0x01a191c0 && address <= 0x01a191d0) ||
                 (address >= 0x01a3d0c0 && address <= 0x01a3d0f0) ||
                 (address >= 0x01d9d1e0 && address <= 0x01d9d1f0) ||
                 (address >= 0x01def920 && address <= 0x01def930)) {
            // init_mk2 installs read handlers only; writes retain their bus lane without changing the latch.
        }
        else if (address == 0x01480000) s.EnableCmos();
        else if (address >= 0xc0000000 && address <= 0xc00001b0) {
            s.board.WriteIo((address - 0xc0000000) / 16, value);
            // MAME queues CLEAR_LINE on every HSTCTLH write (TMS HALT), even when already clear.
            if (address == 0xc0000100) dma_line_event_ = true;
        }
        else if (address == 0xc00001e0) s.board.WriteIo(30, value);
        else if (address >= 0x01a80000 && address <= 0x01a80110) {
            if (address == 0x01a80010 && (value & 0x8000)) {
                const bool fight_writer = machine.FirstFightAdmitted() &&
                    (s.next_pc == 0xff801590 || s.next_pc == 0xff801600 || s.next_pc == 0xff805240);
                const bool valid_writer=machine.ValidCmosDrawAdmitted() && ValidCmosDrawDmaBoundary(s) &&
                    !machine.DmaDeadlineSubticks() && !machine.DmaCompletions() && value==0xc382;
                const bool service_writer=machine.TestSwitchAdmitted() && s.board.dip_switches==kTestSwitchDsw &&
                    s.next_pc==0xff805240 && dma_start_offset==22 && (value==0x9008 || value==0xe00c);
                if ((!fight_writer && !service_writer && s.next_pc != 0xff804630) || s.cycles != machine.LastMainCycles() || (!machine.CoinPairAdmitted() && !valid_writer && !service_writer &&
                    ((s.cycles != 1259252 && s.cycles != 1373046 && s.cycles != 1487299 &&
                    s.cycles != 1601542 && s.cycles != 1715788 && s.cycles != 1830032 &&
                    s.cycles != 1944279 && s.cycles != 2058520 && s.cycles != 2172772 &&
                    s.cycles != 2287008 && s.cycles != 2401264 && s.cycles != 2515498 && s.cycles != 2629751 &&
                    s.cycles != 2743990 && s.cycles != 2858246 && s.cycles != 2972482 && s.cycles != 3086732 && s.cycles != 3200971 && s.cycles != 3314873 &&
                    s.cycles != 3429040 && s.cycles != 3543346 && s.cycles != 3657534) || machine.DmaDeadlineSubticks())))
                    throw std::runtime_error("MK2 DMA start outside qualified loaded drawing context");
                BootVideo::DrawOwner draw{s.next_pc==0xff801590 || s.next_pc==0xff801600,0};
                if(s.next_pc==0xff804630){
                    out.video.DrawQueuedBlood(s.b[14]);
                    if(out.video.BackdropEnabled())draw=out.video.TakeDraw(s.b[14]);
                }
                // Widescreen: every real DMA reaches the wide layer too, except the walk entries the shadow already drew.
                auto& walk=out.video.wide_walk_entries;
                const auto own=s.next_pc==mk2::wide::kDrain?std::find(walk.begin(),walk.end(),s.b[14]):walk.end();
                if(own!=walk.end() && out.video.wide_post_keyed && s.b[14]==out.video.wide_post_key)out.video.FlushWide(program.graphics);
                // A walk entry the shadow drew is not mirrored; one it did not draw (no shadow this frame) is scenery. Only the
                // drain's entries outside the walk (the HUD bars, wins and medallions) keep their place; every other real DMA
                // draws scenery in the arcade's screen and moves with its page's offset.
                out.video.wide_mirror=out.video.WideEnabled() && (own==walk.end() || !out.video.wide_walk_drawn);
                out.video.wide_mirror_world=s.next_pc!=mk2::wide::kDrain || own!=walk.end();
                if(own!=walk.end())walk.erase(own);
                const auto pixels = out.video.StartDma(value, s.board, program.graphics,draw.scenery,draw.fighter);
                out.video.wide_mirror=false;
                machine.ScheduleDma(pixels, dma_start_offset);
                dma_line_event_ = true;
            } else {
                out.video.WriteDma((address - 0x01a80000) / 16, value, s.board);
                // A DMA command write without GO clears INT1; MAME queues CLEAR_LINE even when already clear.
                if (address == 0x01a80010) dma_line_event_ = true;
            }
        }
        else if (address >= 0x01800000 && address <= 0x0187fff0)
            out.video.WritePalette(address, value);
        else if (address == 0x01b00000) s.board.WriteVideoControl(value);
        else if (address == 0x01d01010) {
            // The MK2 map starts the DCS window at20h; this preceding word is unconnected.
        }
        else if (address == 0x01d01020) machine.link.HostWrite(address, value);
        else if (address == 0x01d01070) {
            // The pinned MK2 map leaves this write unmapped; retain its physical bus lane.
        }
        else if (address <= 0x003ffff0) out.video.WriteVram(address, value, s.board);
        else throw std::runtime_error("MK2 boot write outside translated memory: PC " +
                                      std::to_string(s.next_pc) + "; address " + std::to_string(address));
        Record('w', address, value);
    }
    void WriteByte(uint32_t address, uint32_t value) {
        const uint32_t aligned = address & ~uint32_t(15), shift = address & 15;
        if (shift > 8) {
            const auto old = Read32(aligned), mask = 0xffu << shift;
            Write32(aligned,(old & ~mask) | ((value << shift) & mask));
            return;
        }
        const uint16_t old = Read16(aligned), mask = uint16_t(0xffu << shift);
        Write16(aligned, uint16_t((old & uint16_t(~mask)) | ((value & 0xff) << shift)));
    }
    void Write32(uint32_t address, uint32_t value, unsigned dma_start_offset = 0) {
        if (const auto shift = address & 15) {
            const auto aligned = address & ~uint32_t(15);
            const auto low = Read32(aligned) & (0xffffffffu >> (32-shift));
            const auto high = Read32(aligned+32) & (0xffffffffu << shift);
            Write32(aligned,(value << shift) | low,dma_start_offset);
            Write32(aligned+32,(value >> (32-shift)) | high,dma_start_offset);
            return;
        }
        Write16(address, uint16_t(value),dma_start_offset); Write16(address + 16, uint16_t(value >> 16),dma_start_offset);
        if (shadow) return;
        if (address >= 0x01000000 && address <= 0x013fffe0) s.ram[address] = value;
        if(address==0x01060220 || address==0x01060df0)out.video.BindFighter(value);
        else if(address==0x01060130 && out.video.FightersEnabled()){
            switch(s.next_pc){
            case 0xff809120:out.video.ForgetFighter(s.a[0]);break;
            case 0xff8097a0:case 0xff8403f0:case 0xff809990:case 0xff8408e0:case 0xff80c5b0:
                out.video.ForgetFighter(value);break;
            case 0xff808990:case 0xff83f160:out.video.ResetFighters();break;
            }
        }
    }
    unsigned FieldWidth(unsigned field) const {
        if (field > 1) throw std::runtime_error("MK2 boot field selector is invalid");
        const unsigned width = (s.status >> (field * 6)) & 31;
        if (width != 0 && width != 1 && width != 6 && width != 8 && width != 16)
            throw std::runtime_error("MK2 boot field width outside translated scope");
        return width ? width : 32;
    }
    uint32_t ReadField(uint32_t address, unsigned field) {
        const unsigned width = FieldWidth(field);
        if (width == 32) return Read32(address);
        const auto aligned = address & ~uint32_t(15);
        const auto word = (address & 15) + width > 16 ? Read32(aligned) : Read16(aligned);
        uint32_t value = (word >> (address & 15)) &
                         ((uint32_t(1) << width) - 1);
        if ((s.status & (0x20u << (field * 6))) && (value & (uint32_t(1) << (width - 1))))
            value |= ~((uint32_t(1) << width) - 1);
        return value;
    }
    void WriteField(uint32_t address, uint32_t value, unsigned field) {
        const unsigned width = FieldWidth(field);
        if (width == 32) { Write32(address, value); return; }
        if (width == 16 && !(address & 15)) { Write16(address, uint16_t(value)); return; }
        const uint32_t aligned = address & ~uint32_t(15), shift = address & 15;
        if (shift + width > 16) {
            const auto mask = ((uint32_t(1) << width)-1) << shift;
            const auto old = Read32(aligned);
            Write32(aligned,(old & ~mask) | ((value << shift) & mask));
            return;
        }
        const uint16_t mask = uint16_t(((uint32_t(1) << width) - 1) << shift);
        const uint16_t old = Read16(aligned);
        Write16(aligned, uint16_t((old & uint16_t(~mask)) | ((value << shift) & mask)));
    }
    void Push(uint32_t value) { s.a[15] -= 32; Write32(s.a[15], value); }
    uint32_t Pop() { const auto value = Read32(s.a[15]); s.a[15] += 32; return value; }
    void Call(uint32_t target, uint32_t return_pc, unsigned cost) {
        if(shadow){Push(return_pc);Finish(target & ~uint32_t(15), cost);return;}
        if(target==0xff808a40 && out.video.BackdropEnabled())out.video.ResetDrawQueue();
        uint32_t child=0,parent=0;
        if(out.video.FightersEnabled()){
            // Object insertion calls in the finisher tasks. Ordinary projectiles
            // and blood may use the same insertion routines from other PCs.
            switch(s.next_pc){
            case 0xff85bdd0:
                // The Pit II scene rebuild creates a replacement after clearing both roots.
                out.video.BindFighter(s.a[8]);break;
            case 0xff87ecc0:case 0xffa174e0:case 0xff887a20:
            case 0xff8bb500:case 0xff8bcd00:case 0xff8ca410:case 0xffa0e420:
                child=s.a[8];parent=s.a[10];break;
            case 0xff882b50:case 0xff884730:case 0xff884ae0:case 0xff9eb700:case 0xff9e83d0:
            case 0xff9f31b0:case 0xff9df4b0:
                child=s.a[8];parent=s.a[11];break;
            case 0xff8832b0:case 0xff883b80:case 0xff885250:
            case 0xff8c8830:case 0xff8c84f0:case 0xff8c69e0:
            case 0xff8c62d0:case 0xff8bc130:case 0xff8bd480:case 0xff8ae140:
                child=s.a[10];parent=s.a[8];break;
            case 0xff8be790:case 0xff8be820:case 0xff8ae560:
                child=s.a[0];parent=s.a[8];break;
            case 0xff8803a0:case 0xff880490:
            case 0xff9e3150:case 0xff9e38e0:case 0xff9e3af0:
                child=s.a[8];parent=FighterPointer(s.a[13]+0xc0);break;
            case 0xff8c9090:
                child=s.a[8];parent=FighterPointer(s.a[13]+0x140);break;
            case 0xff885dc0:case 0xff8cef80:
                child=s.a[10];parent=FighterPointer(s.a[15]+0x20);break;
            case 0xff8bd6e0:case 0xff8ae390:
                out.video.ForgetFighter(s.a[10]);break;
            }
            if(child && out.video.FighterGeneration(parent))out.video.BindFighter(child);
        }
        Push(return_pc);Finish(target & ~uint32_t(15), cost);
    }
    void Rets(uint32_t return_pc) {
        if ((Pop() & ~uint32_t(15)) != return_pc) throw std::runtime_error("MK2 boot return outside translated call graph");
        Finish(return_pc, 7);
    }
    void ReturnSubroutine(unsigned discard = 0) {
        const auto target = PeekStack() & ~uint32_t(15);
        Pop();
        s.a[15] += discard << 4;
        Finish(target, 7);
    }
    uint32_t PeekStack(unsigned offset = 0) const {
        const uint32_t address = s.a[15] + offset;
        if (address < 0x01000000 || address > 0x013fffe0 || (address & 15))
            throw std::runtime_error("MK2 boot stack outside translated memory");
        const auto n = (address - 0x01000000) / 16;
        return out.main_ram.at(n) | (uint32_t(out.main_ram.at(n + 1)) << 16);
    }
    void RetsAllowed(std::initializer_list<uint32_t> returns) {
        const auto pc = PeekStack() & ~uint32_t(15);
        for (auto allowed : returns) if (pc == allowed) { Rets(pc); return; }
        throw std::runtime_error("MK2 boot return outside translated call graph");
    }
    void PushStatus(uint32_t next) { Push(s.status); Finish(next, 2); }
    uint32_t InterruptVector(uint32_t status, uint32_t stack) const {
        const unsigned pending = s.board.io[17] & s.board.io[18];
        if (!(status & 0x00200000) || !pending) return 0;
        if ((pending & 0x0200) || (!(pending & 0x0400) && (pending & 0x0800)))
            throw std::runtime_error("MK2 host/window interrupt outside translated scope");
        const uint32_t vector = pending & 0x0400 ? 0xfffffea0u : 0xffffffc0u;
        if (!(pending & 0x0402))
            throw std::runtime_error("MK2 external INT2 outside translated scope");
        const uint32_t handler = pending & 0x0400 ? 0xff816370u : 0xff8045d0u;
        const size_t offset = (vector - 0xff800000u) / 8;
        if (program.bytes.size() < offset + 4 ||
            (uint32_t(program.bytes[offset]) | (uint32_t(program.bytes[offset+1]) << 8) |
             (uint32_t(program.bytes[offset+2]) << 16) | (uint32_t(program.bytes[offset+3]) << 24)) != handler)
            throw std::runtime_error("MK2 interrupt vector is not authenticated");
        if (stack < 0x01000040 || stack > 0x01400000 || (stack & 15) ||
            size_t((stack - 0x01000000) / 16) > out.main_ram.size())
            throw std::runtime_error("MK2 IRQ stack outside translated memory");
        return vector;
    }
    uint32_t EnterInterrupt(uint32_t vector, uint32_t return_pc) {
        Push(return_pc); Push(s.status); s.status = 0x10;
        return Read32(vector);
    }
    void PopStatus(uint32_t next) {
        const auto status = PeekStack();
        const auto vector = InterruptVector(status, s.a[15] + 32);
        s.status = Pop();
        Finish(vector ? EnterInterrupt(vector, next) : next, 8 + (vector ? 16 : 0));
    }
    void PutStatus(uint32_t status, uint32_t next) {
        const auto vector = InterruptVector(status, s.a[15]);
        s.status = status;
        Finish(vector ? EnterInterrupt(vector, next) : next, 3 + (vector ? 16 : 0));
    }
    void InterruptEnable(bool enabled, uint32_t next) {
        const auto status = enabled ? s.status | 0x00200000 : s.status & ~uint32_t(0x00200000);
        const auto vector = InterruptVector(status, s.a[15]);
        s.status = status;
        Finish(vector ? EnterInterrupt(vector, next) : next, 3 + (vector ? 16 : 0));
    }
    void ReturnInterrupt() {
        const auto status = PeekStack(), pc = PeekStack(32) & ~uint32_t(15);
        if (pc < 0xff800000)
            throw std::runtime_error("MK2 RETI target outside translated program");
        const auto vector = InterruptVector(status, s.a[15] + 64);
        Pop(); Pop(); s.status = status;
        Finish(vector ? EnterInterrupt(vector, pc) : pc, 11 + (vector ? 16 : 0));
    }
    void TrapOne(uint32_t handler, uint32_t next) {
        if (machine.CoinPairAdmitted()) {
            const bool fight_trap = machine.FirstFightAdmitted() &&
                ((s.next_pc == 0xff804940 && next == 0xff804950) ||
                 (s.next_pc == 0xff805470 && next == 0xff805480));
            if ((!fight_trap && (s.next_pc != 0xff8044f0 || next != 0xff804500)) || handler != 0xff8045d0 ||
                s.a[15] < 0x01000040 || s.a[15] > 0x01400000 || (s.a[15] & 15) ||
                size_t((s.a[15]-0x01000000)/16) > out.main_ram.size())
                throw std::runtime_error("MK2 TRAP outside translated drawing/stack context");
        } else if(machine.ValidCmosDrawAdmitted()) {
            if(!ValidCmosDrawTrapBoundary(s) || next!=0xff804500 || handler!=0xff8045d0 ||
               WordsDigest({s.board.io[17],s.board.io[18]})!="5e9a5de4a59a7c3c9ba8846060ba77ab402e3088658aa705e1a64e796f6cd5cf")
                throw std::runtime_error("MK2 valid-CMOS TRAP differs from original first sprite context");
        } else {
        // Cold drawing TRAPs of the Original: SHA-256 of (cycle, ST, SP, IO17, IO18) (mk2_oracle_digest.hpp).
        static constexpr const char* traps[]={
            "b37dfed3e21b50f15e467eb5ee20c1b9bd9d53fdbc6355966aafc05892f6c50b",
            "a4ce4b4cc31d9f462c46f1699c681f14fb8cd6fa9d79273260aa41cb0e1e0d81",
            "7f1175114cc734e04710fdd8f4d5f7bf58153c86f8bb5246eb4f0eb86a4fb730",
            "715c11e1bbfa499f2519ba50df224bffc9e69ee8275bcbd635a4ae67efa44613",
            "18c17853b60c73203d47347ea2ff02e134a5bfbc61091e92bbeaf901c9eca9d2",
            "1a0c27120d1c70fea9d6ed5d62abe769e7fa065da6c6f649b78b2a3c84edcd4a",
            "bc387f59f40e5d3634557fd08543ffc13209225e3ca454841e6f4f473345fb1c",
            "5f04e913a85a228215424b9fe4d625b1eb6d322ab2bccfc5855bc781e68a1cd5",
            "453c4ddb1b37b59c41cb4e0f85a116ba9d35b182cb7c13629d6e24689f337627",
            "a1e74382725d9451b43e30f3ae2402aaf61ab2909676acbe3350e855a863b55c",
            "9bc867115e0db60e14bbfbb6a775806744c27ef49811d5f26081e037aff74fcf",
            "69e26ddba98c6a27918d08920b4c9e17a395d4094e12ce14bb64977759ee4518",
            "6d182450e8f654df94aba1ce3623a30702c34163cb8d60d9e94499e9531ab7ce",
            "32d5a849463927fdf66cc65d22368eb52ecb7be5c468e1fea04730e7607ab34b",
            "b5182d13e0cc0ee635dbc7800ba6a111f90b043b48921996a9ea8e20252eecca",
            "64438e6a1012ae0156aead71eaa07c1686d20a872cf6b982383c9605ddabd714",
            "48a3440c78b107e6d17392e33df74d9d93df4d95a76944f8345e496343ccb41d",
            "8df5ae95904dad1293bdd4380f45d86bbe44c3a1cb2848467ae7b22d483c1973",
            "b566f7c58bd904445eec03575afa529e587fa9bba848981851d7088fd8d22d21",
            "9687636c8f19b92c174ee09af1aea5dc8b33f981066d21f715cacad7b5081117",
            "bc6733e7947b644336ad50d904b950f363c5ee127a8e812f5dbc7c9f40a09213",
            "b89f2a9492eb5e4e0418a06db2ea0548bef044c2f54829245bfde719db5bd69d",
        };
        if (s.next_pc != 0xff8044f0 || next != 0xff804500 || handler != 0xff8045d0 ||
            !DigestIn(WordsDigest({s.cycles,s.status,s.a[15],s.board.io[17],s.board.io[18]}),traps))
            throw std::runtime_error("MK2 TRAP outside qualified drawing context");
        }
        const size_t vector = (0xffffffc0u - 0xff800000u) / 8;
        if (program.bytes.size() < vector + 4 || program.bytes[vector] != 0xd0 ||
            program.bytes[vector + 1] != 0x45 || program.bytes[vector + 2] != 0x80 || program.bytes[vector + 3] != 0xff)
            throw std::runtime_error("MK2 drawing TRAP vector is not authenticated");
        Push(next); Push(s.status); s.status = 0x10;
        Finish(Read32(0xffffffc0), 16);
    }
    bool PendingInterrupt() {
        const auto vector = InterruptVector(s.status, s.a[15]);
        if (!vector) return false;
        recording_interrupt_ = true;
        s.next_pc = EnterInterrupt(vector, s.next_pc);
        recording_interrupt_ = false; s.cycles += 16;
        const auto completed = machine.DmaCompletions();
        machine.MainInstruction(s.cycles);
        if (machine.DmaCompletions() != completed) {
            s.board.dma_command &= 0x7fff;
            s.board.io[18] |= 2;
        }
        machine.AdvanceVideo(s.board, out.video);
        return true;
    }
    void DisplayInterrupt() {
        // Polling DI contexts of the Original: SHA-256 of (cycle, PC, ST, SP), of (PC, ST) and of (IO17, IO18).
        static constexpr const char* batches[]={
            "3a939f1fed6225fef9f8163de99977b01a68e8d2b3f5e5ec957a45d0c1c36610",
            "da2c07452ae9e5fd9ced313a57696cd460de8ce3766deaad4ba6d7ea5a660ada",
            "8cd59da4120dc8f42c9203ee920486d46ece3f9644209630e8f501a45ed871a8",
            "749e4b824547e0c7587996201f62ba858ab1042871c412ab694927ad5dd0e1be",
            "feceb2e1c6b0ed152084d827779da41092b9f8a8d064289da71129c898a9c20e",
            "98bfbb10bd038854fc63b560e7bada4427c4fc121921129818a43444a9d5d995",
            "c0fc215b740902e47c753a8e9995ab7c23f1fbf48f09a0ffcf1b0ddc8000884f",
            "bdfbcdf1b90ed591c5591ff1708eb74b6eea0ac05b71ae3f0260fe6922d763a8",
            "9f7902496930250fe3bc137bb5fe7646e064301ec0e6d334089932c8ca4579d6",
            "c354ebfe720acca2405f433c786fe4fa29a4d7ec7ce76ca4578018be2ce02b78",
            "f3965f395792d456f0668cc62921f81947240efada16383d9ac84f49f69bc657",
            "c9341f8248ab2f78790bef8f34ce6ffb29c67e3c49df27de860edc42d4fbe510",
            "9819c61269a545fc60bfc185184babb2ac365081a6b9ee075079167e336076b5",
            "56bd64ebf6680f19caaf4430c77aefeaaca46bbe9cc1090703e66ffe8fccd4a2",
            "8c53a3c486653f7aeff21024d6b49fd55d9a5c9fd18b642cf1beb924537147e0",
            "0dc9b56bc602f22282acebb3d396a9651daf74c13600a9e02ae4c7aed8033626",
            "bc0aa56e7e979d4ec795c4443f3ae95e7b7223b36b84b3e974e2cc42e39a3be6",
            "8ad7650f6ba528bfd302044168c12f681a7c039604206484a252364b8e161a06",
            "617959ce9a78b85fc75a55460a8143d193147338c92dbf4bdf6e403df3b99098",
            "fa3c445488ac5e37aad0f5978933adeb388f6db5cb759a6f7d8de2c6e3b8b78e",
            "9ec2bda35db7c2fc96eedfa197607adeecaac6491ac85f91b573dc872f261909",
        };
        static constexpr const char* polls[]={
            "5f3127ad213b292800992c0178474f7ddd54ea0e526b779e08a138328f3e500d",
            "67ca359c49c3b857ace4f20551d9a7dfb739b9d981c7cd8e84c658eed8e0e5cc",
        };
        static constexpr char cold[]="677b0b0cd9ba1ef473272270753b692d8ba4b658dab7eee1e32e503b6935e24a", io_idle[]="d3527ea35fa9a3538188aba60f3053d51860ba10d4bacb43ebf49f0788a09f60", io_batch[]="5fea1dad4c99433ca93586a2639711ca160bf4ee9c1607c3c0587605ed0b34ad";
        const auto context=WordsDigest({s.cycles,s.next_pc,s.status,s.a[15]});
        const bool batch = DigestIn(context,batches);
        const bool valid_startup=machine.ValidCmosStartupAdmitted() && ValidCmosStartupBoundary(s,6);
        const auto io=WordsDigest({s.board.io[17],s.board.io[18]});
        if (!(DigestIn(WordsDigest({s.next_pc,s.status}),polls) || context==cold || batch || valid_startup) ||
            (io!=io_idle && !(io==io_batch && batch)))
            throw std::runtime_error("MK2 asynchronous DI outside qualified polling contexts");
        if (s.a[15] < 0x01000040 || s.a[15] > 0x01400000 || (s.a[15] & 15) ||
            size_t((s.a[15] - 0x01000000) / 16) > out.main_ram.size())
            throw std::runtime_error("MK2 IRQ stack outside translated memory");
        const size_t vector = (0xfffffea0u - 0xff800000u) / 8;
        if (program.bytes.size() < vector + 4 || program.bytes[vector] != 0x70 ||
            program.bytes[vector + 1] != 0x63 || program.bytes[vector + 2] != 0x81 ||
            program.bytes[vector + 3] != 0xff)
            throw std::runtime_error("MK2 first DI vector is not authenticated");
        // The debugger's CURPC/totalcycles cache still belongs to the preceding instruction hook.
        recording_interrupt_ = true;
        Push(s.next_pc); Push(s.status); s.status = 0x10;
        s.next_pc = Read32(0xfffffea0); recording_interrupt_ = false; s.cycles += 16;
        machine.MainInstruction(s.cycles); machine.AdvanceVideo(s.board, out.video);
    }
    uint32_t LinearPixelAddress(uint32_t xy) const {
        if (s.board.io[20] != 19 || s.board.io[21] != 8)
            throw std::runtime_error("MK2 XY conversion outside qualified service mode");
        const uint32_t x = ((xy & 0xffffu) ^ 0x8000u) - 0x8000u;
        const uint32_t y = ((xy >> 16) ^ 0x8000u) - 0x8000u;
        return y * s.board.DestinationPitch() + x * 8 + s.b[4];
    }
    void ConvertXY(uint32_t source, uint32_t& destination, uint32_t next) {
        destination = LinearPixelAddress(source);
        Finish(next, 3);
    }
    void StoreXYPixel(uint32_t xy, uint32_t value) {
        if (s.board.io[11] != 0x2c || s.board.io[22] ||
            (s.board.io[8] & 0x0800) || s.board.VideoBank() != 1)
            throw std::runtime_error("MK2 XY pixel write outside transparent replace mode");
        const uint32_t address = LinearPixelAddress(xy);
        const uint16_t pixel = uint8_t(value);
        if (!pixel) return;
        const uint32_t aligned = address & ~uint32_t(15);
        if (aligned > 0x003ffff0)
            throw std::runtime_error("MK2 XY pixel write outside VRAM");
        const unsigned shift = address & 8;
        const uint16_t old = Read16(aligned);
        Write16(aligned, uint16_t((old & ~(0xffu << shift)) | (pixel << shift)));
    }
    void WriteXYPixel(uint32_t xy, uint32_t value, uint32_t next) {
        StoreXYPixel(xy, value);
        Finish(next, 4);
    }
    void DrawAdvance(uint32_t& destination, uint32_t delta, uint32_t next) {
        StoreXYPixel(destination, s.b[9]);
        const uint16_t x = uint16_t(destination) + uint16_t(delta);
        const uint16_t y = uint16_t(destination >> 16) + uint16_t(delta >> 16);
        destination = x | (uint32_t(y) << 16);
        Finish(next, 4);
    }
    void DrawBitmapText() {
        const uint32_t pc = s.next_pc;
        const auto step = out.execution.bitmap.Step(program, out.video, s, machine.MainBudget(s.cycles));
        const uint32_t next = s.next_pc;
        s.next_pc = pc;
        if (out.observe_history)
            out.bus.insert(out.bus.end(), step.accesses.begin(), step.accesses.end());
        Finish(next, step.cycles);
    }
    void PixelRead(uint32_t address, uint32_t& destination, uint32_t next) {
        if (s.next_pc != 0xff817080 || next != 0xff817090 || s.board.io[21] != 8 ||
            !(s.board.io[8] & 0x0800) || address != 0x001fe000 ||
            (address >> 3) + 1024 > out.video.vram.size())
            throw std::runtime_error("MK2 PIXT outside qualified shift-register load");
        std::copy_n(out.video.vram.begin() + (address >> 3), 1024, out.video.shift_register.begin());
        shift_loaded_ = true;
        const auto value = out.video.shift_register[0];
        destination = value;
        s.status = (s.status & ~uint32_t(0x10000000)) | (value ? 0x10000000 : 0);
        Finish(next, 4);
    }
    void FillXY(uint32_t next) {
        constexpr uint32_t progress = 1u << 25;
        if (s.next_pc != 0xff817130 || next != 0xff817140 ||
            s.board.io[21] != 8 || s.board.io[11] != 0x2c || s.board.io[22] ||
            !(s.board.io[8] & 0x0800) || !shift_loaded_ || s.board.io[20] != 18 || s.board.VideoBank() != 1 ||
            s.b[3] != 0x2000 || (s.b[4] != 3 && s.b[4] != 0x00100003) ||
            s.b[7] != 0x007f0001 || s.b[9] != 0x12 || bool(s.status & progress) != bool(fill_remaining_) ||
            (!fill_remaining_ && s.b[2]) ||
            (fill_remaining_ && !std::equal(fill_b_.begin(), fill_b_.end(), s.b.begin())) ||
            ((s.b[4] & ~uint32_t(7)) + 126 * s.b[3]) / 8 + 1024 > out.video.vram.size())
            throw std::runtime_error("MK2 FILL outside qualified display-column geometry");
        const unsigned budget = machine.MainBudget(s.cycles);
        if (!fill_remaining_) {
            std::copy_n(s.b.begin(), fill_b_.size(), fill_b_.begin());
            // Window mode zero and transparent replace: setup 8, one four-cycle callback per row.
            const unsigned height = s.b[7] >> 16;
            fill_remaining_ = 8 + height * 4;
            s.status |= progress;
            const bool presentation=out.video.BackdropEnabled() || out.video.FighterVideo().enabled();
            const bool scenery=presentation && out.main_ram[(0x010601d0-0x01000000)/16]!=0;
            for (unsigned row = 0; row < height; ++row) {
                const uint32_t address = (s.b[4] & ~uint32_t(7)) + row * s.b[3];
                // The board callback ignores COLOR1/data and directly copies the entire physical shift register.
                std::copy_n(out.video.shift_register.begin(), 1024, out.video.vram.begin() + (address >> 3));
                out.video.MirrorWide(address >> 3, 1024);out.video.MarkCleared(address >> 3);
                out.video.MaskBlood(address >> 3,1024);
                if(presentation)out.video.CopyBackdrop(address >> 3,1024,scenery);
            }
        }
        const unsigned cycles = std::min(fill_remaining_, budget);
        fill_remaining_ -= cycles;
        if (!fill_remaining_) {
            s.status &= ~progress;
            s.b[2] = (s.b[2] & 0xffff) | (uint32_t(uint16_t((s.b[2] >> 16) + (s.b[7] >> 16))) << 16);
        }
        Finish(fill_remaining_ ? s.next_pc : next, cycles);
    }
    // A shadow entry in a palette slot that the pass itself booked for another palette (BootVideo::wide_palettes): its colours
    // come from the ROM (mk5wide::Layer::Book); true (left out) when the slot holds no ROM palette.
    bool WideBooked(const std::array<uint16_t,12>& entry){
        const unsigned slot=(entry[8]>>8)&0x7f;if(slot>=mk2::wide::kPaletteSlots)return false;
        const size_t i=(mk2::wide::kPaletteTable+slot*mk2::wide::kPaletteEntry-0x01000000)/16;
        const uint32_t pointer=out.main_ram[i]|uint32_t(out.main_ram[i+1])<<16;
        if(pointer==out.video.wide_palettes[slot]||out.video.wide.booked[slot])return false;
        if(pointer<0xff800000u||size_t(pointer-0xff800000u)/8+2*257>program.bytes.size())return true;
        out.video.wide.Book(slot,pointer,[&](uint32_t bit){const size_t at=(bit-0xff800000u)/8;return uint16_t(program.bytes[at]|program.bytes[at+1]<<8);});
        return false;
    }
    void Mmtm(uint16_t mask, uint32_t next, bool bank_b = false, unsigned base = 15) {
        if (base > 15)
            throw std::runtime_error("MK2 boot MMTM address register is not translated");
        // The shadow drawer's staged entry: A12 r0/r1, A11 r2/r3, A10 r4/r5, A9 r6/r7, A8 r8/r9, A6 r10/r11.
        if(shadow && s.next_pc==mk2::wide::kDrawerStage){
            std::array<uint16_t,12> entry{};const unsigned regs[6]={12,11,10,9,8,6};
            for(unsigned k=0;k<6;++k){entry[2*k]=uint16_t(s.a[regs[k]]);entry[2*k+1]=uint16_t(s.a[regs[k]]>>16);}
            // A0: the object. A far plane's object the game parked off the arcade's screen goes on flying in the margins
            // (BootVideo::WideMove): its x velocity and x position longs, and its x on the plane's screen.
            const uint32_t object=s.a[0];int32_t velocity=0,x=0;
            const auto ram_long=[&](uint32_t bit){const size_t i=(bit-0x01000000u)/16;return i+1<out.main_ram.size()?int32_t(out.main_ram[i]|uint32_t(out.main_ram[i+1])<<16):0;};
            if(out.video.wide_slot>=0&&object>=0x01000000u&&object<0x01400000u&&!(object&15)){velocity=ram_long(object+mk2::wide::kObjectXVelocity);x=ram_long(object+mk2::wide::kObjectX);}
            for(const auto& shown:out.video.WideMove(object,velocity,x,(x>>16)-out.video.wide_camera,entry))
                if(!WideBooked(shown)&&!out.video.WideEntry(shown,program.graphics,out.video.WideFresh(object)))shadow_fault=true;
        } else if(shadow && s.next_pc==mk2::wide::kFloorStage){   // the floor producer: A9 r0/r1, A8 r2/r3, A7 r4/r5, A6 r6/r7, A3 r8/r9, A2 r10/r11
            std::array<uint16_t,12> entry{};const unsigned regs[6]={9,8,7,6,3,2};
            for(unsigned k=0;k<6;++k){entry[2*k]=uint16_t(s.a[regs[k]]);entry[2*k+1]=uint16_t(s.a[regs[k]]>>16);}
            // Each floor line is one unscaled 400-pixel row (at the virtual camera): the same row of the floor art from the
            // wide window's left edge (67 pixels earlier) and as wide as the layer.
            if(entry[6]==400 && entry[7]==1 && !(entry[1]&0x10)){
                const unsigned code=(entry[1]>>12)&7,bpp=code?code:8;constexpr int lead=mk5wide::kWideMargin;
                const uint32_t source=(entry[2]|uint32_t(entry[3])<<16)-uint32_t(lead)*bpp;
                entry[2]=uint16_t(source);entry[3]=uint16_t(source>>16);entry[4]=uint16_t(entry[4]-lead);entry[6]=mk5wide::kLayerWidth;
            }
            if(!WideBooked(entry)&&!out.video.WideEntry(entry,program.graphics))shadow_fault=true;
        }
        // Widescreen: the real walk's entries (their queue cursors); the shadow's own copies replace them when it drew this frame.
        if(!shadow && out.video.WideEnabled() && (s.next_pc==mk2::wide::kDrawerStage || s.next_pc==mk2::wide::kFloorStage)){
            out.video.wide_walk_entries.push_back(s.a[1]-0xc0);
            if(out.video.wide_walk_drawn && out.video.wide_real_post && !out.video.wide_post_keyed){out.video.wide_post_key=s.a[1]-0xc0;out.video.wide_post_keyed=true;}
        }
        // The IRQ drains six dwords per object and reaches the command at this cursor.
        if(!shadow && out.video.BackdropEnabled() &&
           (s.next_pc==0xff804410 || s.next_pc==0xff805390 || s.next_pc==0xff800490)){
            const auto owner=s.next_pc==0xff804410?FighterOwner():0;
            out.video.StageDraw(s.a[1]-0xc0,{s.next_pc==0xff804410 && out.video.backdrop_list && !owner,owner});
        }
        unsigned cost = 2;
        for (unsigned reg = 0; reg < 16; ++reg)
            if (mask & (0x8000u >> reg)) {
                auto& address = bank_b && base != 15 ? s.b[base] : s.a[base];
                // COUNT_CYCLES precedes each write; timer.adjust uses CPU local_time, not debugger CURPC/totalcycles.
                address -= 32; Write32(address, reg == 15 ? s.a[15] : bank_b ? s.b[reg] : s.a[reg],cost); cost += 4;
            }
        Finish(next, cost);
    }
    void Absolute(uint32_t& reg, uint32_t next) {
        const uint32_t negated=0u-reg;
        // ABS exports flags from the negation even when it keeps a positive operand.
        s.status=(s.status&~0xb0000000u)|(negated&0x80000000u)|
                 (negated==0?0x20000000u:0)|(negated==0x80000000u?0x10000000u:0);
        if(negated && !(negated&0x80000000u)) reg=negated;
        Finish(next,1);
    }
    void DivideSignedSingle(uint32_t& reg, uint32_t value, uint32_t next) {
        if(reg==0x80000000u && value==0xffffffffu)
            throw std::runtime_error("MK2 odd DIVS overflow outside measured reference context");
        s.status&=~0xb0000000u;
        if(!value) s.status|=0x10000000u;
        else {
            const int64_t numerator=(reg&0x80000000u)?int64_t(reg)-0x100000000ll:reg;
            const int64_t divisor=(value&0x80000000u)?int64_t(value)-0x100000000ll:value;
            reg=uint32_t(numerator/divisor);
            s.status|=(reg&0x80000000u)|(reg==0?0x20000000u:0);
        }
        Finish(next,39);
    }
    void Mmfm(uint16_t mask, uint32_t next, bool bank_b = false, unsigned base = 15) {
        if (base > 15)
            throw std::runtime_error("MK2 boot MMFM address register is not translated");
        unsigned cost = 3;
        for (int reg = 15; reg >= 0; --reg)
            if (mask & (0x8000u >> reg)) {
                auto& address = bank_b && base != 15 ? s.b[base] : s.a[base];
                const auto value = Read32(address);
                (reg == 15 ? s.a[15] : bank_b ? s.b[reg] : s.a[reg]) = value;
                address += 32; cost += 4;
            }
        Finish(next, cost);
    }
};
}
