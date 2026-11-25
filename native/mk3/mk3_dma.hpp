// MK3 2.1 (Wolf-Unit) DMA blitter, isolated from the CPU executor and the video timing.
// Contract: MAME 0.280 as archived in docs/investigation (V = sources/midtunit_v.cpp, H = decoder-research/upstream/
// midtunit_v.h, W = sources/midwunit.cpp) and the draw tables of midtunit_v.ipp at tag mame0280 (I; not archived yet:
// the mame0280 tag of mamedev/mame, file midtunit_v.ipp). midwunit_video_device inherits
// dma_r/dma_w/dma_done/dma_draw from the T-Unit device with m_gfx_rom_large set (V:79-83); the DMA reads the "video"
// region from offset 0, without the CPU read bank of midwunit_gfxrom_r (V:211, 438-439).
// Translated: every dma_w path - the 16 pixel modes x xflip x yflip x skip (pre/post sizes, LRSKIP in both layouts)
// x clipping, all 8 depths, the unscaled and the scaled dma_draw branches and skipdma (source out of range).
// Verified against the Original from reset (tools/mk3/reference_dma.py, tools/mk3/test_dma.py): the paths of the
// commands 0xc002, 0xc082 and 0x800c, unscaled, source in range. Profile (mk3_profile.hpp): in Strict every other
// path rejects with the command, as the measured-only build did; in Game it runs and `first` records DmaPath with
// id = PathId(). A read past the loaded ROM bytes (MAME fills the rest of the region with random data under the
// debugger) and a done time outside the int arithmetic of dma_w reject in both profiles.
// Timing and the INT1 line stay in VideoTiming: Write returns the pixel count for VideoTiming::DmaCommand and the
// owner calls Done() whenever VideoTiming::int1_count advances (dma_done, V:628-632).
#pragma once
#include "mk3_dma_observer.hpp"
#include "mk3_profile.hpp"
#include "mk3_program.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace mk3 {

struct Dma {
    enum Reg { LRSKIP, COMMAND, OFFSETLO, OFFSETHI, XSTART, YSTART, WIDTH, HEIGHT, PALETTE, COLOR, SCALE_X, SCALE_Y,
               TOPCLIP, BOTCLIP, UNKNOWN_E, CONFIG, LEFTCLIP, RIGHTCLIP };  // H:87-107
    enum Op { SKIP, COLOR_OP, COPY };                                       // H:54-59 op_type_t
    static constexpr uint32_t kXMask = 0x3ff, kYMask = 0x1ff;              // H:75-76
    static constexpr uint32_t kRegionBits = 0x2000000u * 8;                // W:725 ROM_REGION( 0x2000000, "video" ); V:761

    std::array<uint16_t, 18> reg{};                         // m_dma_register, zero (V:63)
    std::vector<uint16_t> vram = std::vector<uint16_t>(0x80000);  // m_local_videoram, 0x100000 bytes (V:198)
    const std::vector<uint8_t>& gfx;                        // the loaded bytes of the "video" region (W:725-751)
    Profile profile = Profile::Strict;
    const RouteVerified* routes = nullptr;   // Strict: route-verified paths also run (mk3_profile.hpp)
    FirstExecution* first = nullptr;
    DmaObserver* observer = nullptr;   // read-only draw/pixel observation (mk3_dma_observer.hpp)
    uint64_t draws = 0;

    explicit Dma(const std::vector<uint8_t>& graphics) : gfx(graphics) {}

    // dma_r (V:642-650): register 0 reads as the command register; no register_map on reads.
    uint16_t Read(unsigned offset) const { offset &= 15; return reg[offset ? offset : unsigned(COMMAND)]; }

    // dma_done (V:628-632) without the INT1 line, which VideoTiming raises.
    void Done() { reg[COMMAND] &= 0x7fff; }

    static bool Verified(uint16_t command) { return command == 0xc002 || command == 0xc082 || command == 0x800c; }
    // DmaPath key: the command bits that select the dma_draw instance and its state (15..0), the scaled branch (16)
    // and skipdma (17).
    static uint32_t PathId(uint16_t command, bool scaled, bool in_range) {
        return uint32_t(command) | (scaled ? 0x10000u : 0) | (in_range ? 0 : 0x20000u);
    }

    // dma_w (V:699-840), offset = word offset in 0x01a00000-0x01a000ff (mirror 0x80000, W:123). Returns -1 for a
    // register other than DMA_COMMAND, else the pixels for the done timer (0 without bit 15, which arms nothing).
    // cycle: only for the first-execution record.
    int Write(unsigned offset, uint16_t data, uint16_t mask = 0xffff, uint64_t cycle = 0) {
        static const uint8_t map[2][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 14, 15},
                                           {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
        const unsigned number = map[(reg[CONFIG] >> 5) & 1][offset & 15];
        reg[number] = uint16_t((reg[number] & ~mask) | (data & mask));
        if (number != COMMAND) return -1;
        const uint16_t command = reg[COMMAND];
        if (!(command & 0x8000)) return 0;
        const unsigned xstep = reg[SCALE_X] ? reg[SCALE_X] : 0x100, ystep = reg[SCALE_Y] ? reg[SCALE_Y] : 0x100;  // V:740-741
        const bool scaled = xstep != 0x100 || ystep != 0x100;
        uint32_t source = (command & 0x0f) == 0x0c ? 0 : reg[OFFSETLO] | uint32_t(reg[OFFSETHI]) << 16;  // V:750-754
        if (source >= 0xf8000000u) source -= 0xf8000000u;                                                 // V:759-760
        const bool in_range = source < kRegionBits;                                                       // V:761-767
        if (!Verified(command) || scaled || !in_range) {
            if (profile == Profile::Strict && !Accepted(routes, Unverified::DmaPath, PathId(command, scaled, in_range))) {
                char text[96];
                if (!in_range && Verified(command) && !scaled) std::snprintf(text, sizeof text, "MK3 DMA source outside the graphics region is not verified");
                else std::snprintf(text, sizeof text, "MK3 DMA command 0x%04x scale 0x%x,0x%x is not verified", command, xstep, ystep);
                throw std::runtime_error(text);
            }
            if (first) first->Note(Unverified::DmaPath, PathId(command, scaled, in_range), cycle);
        }
        if (!in_range) return 0;   // skipdma: m_dma_timer->adjust(from_nsec(41 * 0))
        Draw(command, source, xstep, ystep, false);  // all ROM reads checked before any video RAM write
        ++draws;
        if (observer) {
            const uint16_t pal = reg[PALETTE] & 0x7f00;
            observer->Begin({draws, cycle, command, source, int(reg[XSTART] & kXMask), int(reg[YSTART] & kYMask), reg[WIDTH] & 0x3ff,
                             reg[HEIGHT] & 0x3ff, pal, uint16_t(pal | (reg[COLOR] & 0xff)), reg[LEFTCLIP] & 0x3ff, reg[RIGHTCLIP] & 0x3ff,
                             reg[TOPCLIP] & 0x1ff, reg[BOTCLIP] & 0x1ff, uint16_t(xstep), uint16_t(ystep)});
        }
        Draw(command, source, xstep, ystep, true);
        const int w = reg[WIDTH] & 0x3ff, h = reg[HEIGHT] & 0x3ff;  // V:729-732
        if (!scaled) return w * h;                                  // V:822
        // V:831-834 in int: ((w << 8) / xstep) * ((h << 8) / ystep), then from_nsec(41 * pixels).
        const int64_t pixels = int64_t((w << 8) / int(xstep)) * ((h << 8) / int(ystep));
        if (pixels * 41 > INT32_MAX) throw std::runtime_error("MK3 DMA done time overflows the int arithmetic of dma_w");
        return int(pixels);
    }

    // A staged entry's source as dma_w folds it; false: outside the graphics region (skipdma, no draw).
    static bool Source(uint16_t command, uint16_t low, uint16_t high, uint32_t& source) {
        source = (command & 0x0f) == 0x0c ? 0 : low | uint32_t(high) << 16;
        if (source >= 0xf8000000u) source -= 0xf8000000u;
        return source < kRegionBits;
    }
private:
    // EXTRACTGEN (V:426-432): two bytes from the bit offset.
    unsigned Extract(uint32_t bit, unsigned mask) const {
        const size_t at = bit >> 3;
        if (at + 1 >= gfx.size()) throw std::runtime_error("MK3 DMA read past the loaded graphics ROM is not verified");
        return ((unsigned(gfx[at]) | unsigned(gfx[at + 1]) << 8) >> (bit & 7)) & mask;
    }

    // dma_draw<BitsPerPixel, XFlip, Skip, Scale, Zero, NonZero> (V:435-614) selected through the I tables:
    // index (command & 0x1f) * 8 + bpp, bpp 0 = 8 bits, mode 0 = dma_draw_none, bit 4 = XFlip; Scale is the
    // xstep/ystep != 0x100 branch (V:815-830). write = false makes only the ROM reads of the write pass.
    void Draw(uint16_t command, uint32_t source, unsigned xstep_reg, unsigned ystep_reg, bool write) {
        Blit(reg, command, source, xstep_reg, ystep_reg, write, 512, [this](size_t index, uint16_t value) {
            vram[index] = value;
            if (observer) observer->Pixel(index, value);
        });
    }
public:
    // The same blitter over any registers into a destination of `stride` words per line (the widescreen layer,
    // mk3_wide.hpp): store(index, value) for each word written; write = false makes only the ROM reads.
    template <class Store>
    void Blit(const std::array<uint16_t, 18>& reg, uint16_t command, uint32_t source, unsigned xstep_reg, unsigned ystep_reg, bool write,
              size_t stride, Store&& store_word) const {
        static const Op zero_op[16] = {SKIP, COPY, SKIP, COPY, COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP,
                                       SKIP, COPY, SKIP, COPY, COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP};
        static const Op nonzero_op[16] = {SKIP, SKIP, COPY, COPY, SKIP, SKIP, COPY, COPY,
                                          COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP, COLOR_OP};
        if ((command & 15) == 0) return;  // dma_draw_none
        const Op zero = zero_op[command & 15], nonzero = nonzero_op[command & 15];
        const bool scale = xstep_reg != 0x100 || ystep_reg != 0x100;
        const int bpp = (command >> 12) & 7 ? (command >> 12) & 7 : 8;
        const unsigned mask = (1u << bpp) - 1;
        const bool xflip = command & 0x10, skip = command & 0x80, yflip = command & 0x20;  // V:737
        const int preskip = (command >> 8) & 3, postskip = (command >> 10) & 3;           // V:738-739
        const int xpos = reg[XSTART] & kXMask, width_px = reg[WIDTH] & 0x3ff, height = (reg[HEIGHT] & 0x3ff) << 8;  // V:729-732
        const uint16_t pal = reg[PALETTE] & 0x7f00, color = uint16_t(pal | (reg[COLOR] & 0xff)); // V:733-734, 442
        const int top = reg[TOPCLIP] & 0x1ff, bottom = reg[BOTCLIP] & 0x1ff;               // V:744-747
        const int left = reg[LEFTCLIP] & 0x3ff, right = reg[RIGHTCLIP] & 0x3ff;
        const int startskip_px = command & 0x40 ? reg[LRSKIP] & 0xff : 0;                  // V:791-800
        const int endskip_px = command & 0x40 ? reg[LRSKIP] >> 8 : reg[LRSKIP];
        const int xstep = scale ? int(xstep_reg) : 0x100, ystep = int(ystep_reg);           // V:447
        uint32_t offset = source;
        int sy = reg[YSTART] & kYMask, iy = 0, pre = 0, post = 0;
        while (iy < height) {
            const int startskip = startskip_px << 8;
            int width = width_px << 8, sx = xpos, ix = 0, tx;
            uint32_t o = offset;
            if (skip) {  // V:461-480
                const unsigned value = Extract(o, 0xff);
                o += 8;
                pre = int(value & 0x0f) << (preskip + 8);
                tx = pre / xstep;
                sx = int((xflip ? sx - tx : sx + tx) & kXMask);
                ix += tx * xstep;
                post = int((value >> 4) & 0x0f) << (postskip + 8);
                width -= post;
            }
            if (sy >= top && sy <= bottom) {  // V:483-485
                if (ix < startskip) {  // V:488-493
                    tx = ((startskip - ix) / xstep) * xstep;
                    ix += tx;
                    o += uint32_t((tx >> 8) * bpp);
                }
                if ((width >> 8) > width_px - endskip_px) width = (width_px - endskip_px) << 8;  // V:496-497
                const size_t row = size_t(sy) * stride;
                const auto store = [&](int x, uint16_t value) { store_word(row + size_t(x), value); };
                while (ix < width) {  // V:507-563
                    if (sx >= left && sx <= right) {
                        if (!write) { if (zero != COLOR_OP || nonzero != COLOR_OP) Extract(o, mask); }  // the write pass's reads
                        else if (zero == nonzero) {
                            if (zero == COLOR_OP) store(sx, color);
                            else if (zero == COPY) store(sx, uint16_t(Extract(o, mask) | pal));
                        } else {
                            const unsigned pixel = Extract(o, mask);
                            const Op op = pixel ? nonzero : zero;
                            if (op == COLOR_OP) store(sx, color);
                            else if (op == COPY) store(sx, uint16_t(pixel | pal));
                        }
                    }
                    sx = int((xflip ? sx - 1 : sx + 1) & kXMask);
                    if (!scale) { ix += 0x100; o += uint32_t(bpp); }
                    else { tx = ix >> 8; ix += xstep; tx = (ix >> 8) - tx; o += uint32_t(bpp * tx); }
                }
            }
            sy = int((yflip ? sy - 1 : sy + 1) & kYMask);  // V:566-571
            if (!scale) {  // V:572-586
                iy += 0x100;
                width = width_px;
                if (skip) {
                    offset += 8;
                    width -= (pre + post) >> 8;
                    if (width > 0) offset += uint32_t(width * bpp);
                } else offset += uint32_t(width * bpp);
            } else {       // V:588-612
                int ty = iy >> 8;
                iy += ystep;
                ty = (iy >> 8) - ty;
                if (!skip) offset += uint32_t(ty * width_px * bpp);
                else if (ty--) {
                    o = offset + 8;
                    width = width_px - ((pre + post) >> 8);
                    if (width > 0) o += uint32_t(width * bpp);
                    while (ty--) {
                        const unsigned value = Extract(o, 0xff);
                        o += 8;
                        pre = int(value & 0x0f) << preskip;    // V:604-605: no "+ 8" in the skipped rows
                        post = int((value >> 4) & 0x0f) << postskip;
                        width = width_px - pre - post;
                        if (width > 0) o += uint32_t(width * bpp);
                    }
                    offset = o;
                }
            }
        }
    }
};

// Remastered floor blood (mk3_blood.hpp): the splash frame's palette indices, drawn by this blitter from the frame header
// (width, height, anchor, source, command bits at words 0-6, as the object walker reads it) into a scratch video RAM.
inline void EnableFloorBlood(FloorBlood& blood, const Program& program, const std::vector<uint8_t>& graphics) {
    const auto word = [&](unsigned k) { return program.Word(FloorBlood::kFrame + 16 * k); };
    if (word(0) != FloorBlood::kWidth || word(1) != FloorBlood::kHeight) throw std::runtime_error("MK3 invalid original floor splash");
    Dma dma{graphics};
    dma.profile = Profile::Game;
    dma.reg[Dma::WIDTH] = word(0); dma.reg[Dma::HEIGHT] = word(1);
    dma.reg[Dma::OFFSETLO] = word(4); dma.reg[Dma::OFFSETHI] = word(5);
    dma.reg[Dma::BOTCLIP] = 511; dma.reg[Dma::RIGHTCLIP] = 1023;
    dma.Write(Dma::COMMAND, uint16_t(0x8002 | (word(6) & ~0x30u)));   // unmirrored; the stain mirrors itself
    blood.shape.assign(size_t(FloorBlood::kWidth) * FloorBlood::kHeight, 0);
    for (int y = 0; y < FloorBlood::kHeight; ++y)
        for (int x = 0; x < FloorBlood::kWidth; ++x) {
            const uint16_t pixel = dma.vram[size_t(y) * 512 + size_t(x)];
            if (pixel >= FloorBlood::Colors{}.size()) throw std::runtime_error("MK3 invalid original floor splash palette index");
            blood.shape[size_t(y) * FloorBlood::kWidth + size_t(x)] = uint8_t(pixel);
        }
    blood.layer.assign(dma.vram.size(), 0);
}

} // namespace mk3
