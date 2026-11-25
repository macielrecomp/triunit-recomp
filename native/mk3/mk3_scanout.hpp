// MK3 2.1 (Wolf-Unit) scanout: the screen bitmap as MAME 0.280 builds it, line by line, from live video RAM.
// Contract (S = decoder-research/upstream/tms34010.cpp, SCR = decoder-research/upstream/screen.cpp, V = sources/
// midtunit_v.cpp, W = sources/midwunit.cpp): the scanline timer calls screen().update_partial(vcount) inside the
// visible area before DPYADR's DUDATE step (S:1094-1097, VideoTiming::update_partial); update_partial renders
// [last_partial_scan, vcount] clipped to the visible area (SCR:628-700) through tms340x0_ind16, which draws only the
// clip's first row (S:1168-1196) with get_display_params (S:1137-1155) and midtunit scanline_update (V:845-855):
// pens = VRAM & 0x7fff from HEBLNK to HSBLNK - 1, black pen elsewhere in the clip and on a disabled display. The
// scanline-0 timer resets last_partial_scan (SCR:902). The ind16 bitmap goes through the xRGB_555 palette (W:643) at
// the primary screen's frame_update in vblank_begin (SCR:1153-1162): Present() takes the palette RAM at that point.
// Geometry: 506 x 289 raw, visible 101..500 x 20..273, 1 pixel per 8 MHz clock (W:625-647).
// Not modelled (reject): an HSBLNK past the bitmap width. Frame skipping and render liveness (SCR:633-647) are
// assumed off/live, as for the reference captures.
// Compared with the Original (tools/mk3/test_scanout_original.py): every VBLANK from reset to the native `jogo`
// frontier, visible RGB equal frame by frame (as screen_device::pixels sees it) and palette RAM equal at each VBLANK.
// Remastered layer 2 (presentation only): with `layers` attached, each rendered row also keeps the backdrop pen of
// every pixel (DmaLayers::Backdrop) and Present() resolves it through the same palette into `backdrop`; `pens`,
// `frame` and the hashes are computed exactly as without it. Each row also samples layers->fighters through
// mk5presentation::FighterCapture (as MK2's scanout, with pens instead of colours); Present() completes `fighters`
// through the same palette (serial 0 = no fighter layer this frame or a row the capture cannot follow).
// Floor blood (mk3_blood.hpp): a visible word under a stain of layers->blood is the stain's colour in `presentation`
// (the frame the Remastered window shows), unflagged in `backdrop` and in the fighters' underlay; `frame` stays canonical.
// Present dims the frame's stains by their FloorBlood floor pens through the VBLANK palette against those pens'
// brightness then (mk5blood::Shade, as MK1/MK2), so the program's palette fades take them along and nothing drawn
// under a stain later (a shadow) does.
#pragma once
#include "mk3_dma_observer.hpp"
#include "mk3_video.hpp"
#include "../include/mk5_wide.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace mk3 {

struct Scanout {
    using V = VideoTiming;
    static constexpr int kWidth = V::kWidth, kHeight = V::kHeight, kLeft = V::kVisLeft, kRight = V::kVisRight;
    static constexpr int kTop = V::kVisTop, kBottom = V::kVisBottom;
    static constexpr int kFrameWidth = kRight - kLeft + 1, kFrameHeight = kBottom - kTop + 1;   // 400 x 254
    static constexpr uint16_t kBlackPen = 0x8000;   // palette black_pen(): outside the 32768 palette entries

    std::vector<uint16_t> pens = std::vector<uint16_t>(size_t(kWidth) * kHeight, kBlackPen);   // the ind16 bitmap
    std::vector<uint32_t> frame = std::vector<uint32_t>(size_t(kFrameWidth) * kFrameHeight);  // 0x00RRGGBB, visible area
    int last_partial_scan = 0;
    uint64_t frames = 0, lines = 0;
    // Host observer after every Present (window, audio pump, hashes); it reads the frame and never feeds back.
    std::function<void(const Scanout&)> presented;
    DmaLayers* layers = nullptr;
    mk5presentation::FighterCapture fighter_work;
    mk5presentation::FighterFrame fighters;
    std::vector<uint32_t> row_pens = std::vector<uint32_t>(kFrameWidth), row_back = std::vector<uint32_t>(kFrameWidth);
    std::vector<uint32_t> backdrop_pens, backdrop;   // pen | DmaLayers::kScenery; 0x00RRGGBB | kSceneryRgb (visible area)
    std::vector<uint32_t> presentation_pens, presentation;   // visible area: pen or FloorBlood::kMark | RGB555; 0x00RRGGBB
    std::vector<uint16_t> presentation_floor_pen;   // visible area: FloorBlood floor pen and brightness of a stained pixel
    std::vector<uint8_t> presentation_floor;
    std::vector<uint16_t> row_blood = std::vector<uint16_t>(kWidth);
    std::vector<uint16_t> row_floor_pen = std::vector<uint16_t>(kWidth);
    std::vector<uint8_t> row_floor = std::vector<uint8_t>(kWidth);
    static constexpr uint32_t kSceneryRgb = 0x80000000u;   // mk5stage::scenery

    // Widescreen (mk3_wide.hpp, Remastered presentation): each visible line also takes its line of the 16:9 layer, the
    // arcade's 400 columns at its centre, as pen codes resolved through the VBLANK palette like the frame. A line whose
    // raster is not the arcade's (blanked, another tap) shows black sides.
    // wide_source: each wide pixel's arcade column where its pen is the arcade's own at that column (the world's column
    // with the window's offset, the HUD's without), so the Remastered layers of that column apply; kRaw where only the
    // layer has it (margins, an offset window, a block the arcade has not drawn yet); kBlank on a blanked line.
    static constexpr int kWideWidth = mk5wide::kLayerWidth;
    static constexpr uint32_t kWideColour = 0x10000, kWideMasked = 0x20000, kWideSentinel = 0x30000, kWideBlank = 0x40000;   // pen codes above the palette's
    static constexpr int16_t kRaw = -1, kBlank = -2;
    static constexpr uint8_t kWideFresh = 32, kWideSoft = 64;   // mk3::Wide::kFresh, kSoft
    static constexpr uint32_t kWideSoftFlag = 0x100000;        // on a pen code: a pixel past a far backdrop's end, shown soft
    const mk5wide::Layer<uint16_t>* wide = nullptr;
    bool wide_present = false;
    std::function<unsigned()> wide_dim;   // the game's fade at the VBLANK, of 256 (mk3::Wide::Dim), for the colours the layer took from the ROM
    std::vector<uint32_t> wide_pens, wide_frame;   // kWideWidth x kFrameHeight: pen codes; 0xffRRGGBB (raw colours, soft sides)
    std::vector<int16_t> wide_source;
    std::vector<uint16_t> wide_blood;              // the floor stain on a kRaw pixel (RGB555 | 8000h; 0: none)
    int wide_shift_work = 0, wide_shift = 0;       // the frame's window offset: arcade column x is wide column x + 67 - shift
    bool wide_whole_work = false, wide_whole = false, wide_soft_work = false, wide_soft = false;
    // Diagnostics of the last presented frame: arcade-column pixels that differ from VRAM (0 when the wide camera is the
    // arcade's), of them the ones a fresh draw filled, and the margin audit (mk5wide::Layer::debug).
    uint64_t wide_centre_work = 0, wide_centre = 0, wide_filled_work = 0, wide_filled = 0;
    mk5wide::Audit wide_audit_work, wide_audit;
    std::vector<uint32_t> wide_presentation, wide_backdrop;   // 0xffRRGGBB; 0x00RRGGBB | kSceneryRgb
    mk5presentation::FighterFrame wide_fighters;
    // Save-state fields: the lines taken so far (a state is saved inside a frame); a state saved with widescreen and read
    // without it is passed over, one saved without it leaves the wide frame as it stands until the next one.
    template<class A> void WideFields(A& a) {
        bool saved = wide != nullptr;
        a(saved);
        if (!saved) return;
        if (wide) { a(wide_pens, wide_source, wide_blood, wide_shift_work, wide_whole_work, wide_soft_work, wide_centre_work, wide_filled_work); return; }
        std::vector<uint32_t> pens_; std::vector<int16_t> source_; std::vector<uint16_t> blood_;
        int shift_ = 0; bool whole_ = false, soft_ = false; uint64_t centre_ = 0, filled_ = 0;
        a(pens_, source_, blood_, shift_, whole_, soft_, centre_, filled_);
    }
    void EnableWide(const mk5wide::Layer<uint16_t>* layer, bool present) {
        wide = layer; wide_present = present;
        const size_t n = size_t(kWideWidth) * kFrameHeight;
        wide_pens.assign(n, kWideBlank); wide_source.assign(n, kBlank); wide_blood.assign(n, 0); wide_frame.assign(n, 0xff000000u);
    }
    void WideLine(int y, bool on, size_t row_base, unsigned col, const std::vector<uint16_t>& vram) {
        const auto& layer = *wide;
        const bool have = on && layer.Enabled() && (col & 0x1ff) == unsigned(mk5wide::kMargin);   // the row's low bits ride above the tap
        const unsigned page = unsigned(row_base >> 17) & 1;
        if (!y) {
            wide_shift_work = have ? layer.page_shift[page] : 0; wide_whole_work = have && layer.page_whole[page];
            wide_soft_work = have && layer.page_soft[page];
            wide_centre_work = wide_filled_work = 0; wide_audit_work = {};
        }
        const int shift = wide_shift_work;
        const size_t base = (row_base >> 9) * size_t(kWideWidth), line = size_t(y) * kWideWidth;
        const uint16_t* row = vram.data() + row_base;
        const uint16_t* pens = have ? layer.pens.data() + base : nullptr;
        const uint8_t* marks = have ? layer.fixed.data() + base : nullptr;
        const uint16_t* stains = have && !layer.blood.empty() ? layer.blood.data() + base : nullptr;
        uint32_t* out = wide_pens.data() + line;
        int16_t* sources = wide_source.data() + line;
        uint16_t* bloods = wide_blood.data() + line;
        for (int x = 0; x < kWideWidth; ++x) {
            const int hud = x - mk5wide::kWideMargin;
            if (!have) {   // the arcade's line between black sides
                const bool arcade = on && hud >= 0 && hud < kFrameWidth;
                out[x] = arcade ? uint32_t(row[(col + unsigned(hud)) & 0x1ff] & 0x7fff) : kWideBlank;
                sources[x] = arcade ? int16_t(hud) : kBlank; bloods[x] = 0;
                continue;
            }
            const uint16_t pen = pens[x];
            const bool shown = layer.Shown(base + size_t(x));
            const int at = !shift || (marks[x] & mk5wide::Layer<uint16_t>::kFixed) ? hud : hud + shift;
            const bool same = at >= 0 && at < kFrameWidth && (pen & 0x7fff) == (row[(col + unsigned(at)) & 0x1ff] & 0x7fff) && !(pen & 0x8000);
            if (hud >= 0 && hud < kFrameWidth && !wide_whole_work && (pen & 0x7fff) != (row[(col + unsigned(hud)) & 0x1ff] & 0x7fff))
                ++(marks[x] & kWideFresh ? wide_filled_work : wide_centre_work);
            const bool soft = shown && (marks[x] & kWideSoft) && (hud < 0 || hud >= kFrameWidth);
            out[x] = !shown ? (layer.debug && !layer.written[base + size_t(x)] ? kWideSentinel : kWideMasked)
                            : (pen & 0x8000 && !layer.rom_palette[page].empty() ? kWideColour | layer.rom_palette[page][pen & 0x7fff] : uint32_t(pen & 0x7fff)) |
                              (soft ? kWideSoftFlag : 0);
            sources[x] = same && shown && !soft ? int16_t(at) : kRaw;
            bloods[x] = !same && shown && !soft && stains ? stains[x] : uint16_t(0);
        }
        if (have) layer.AuditRow(wide_audit_work, unsigned(row_base >> 9));
    }
    void PresentWide(const std::vector<uint16_t>& palette, const mk5blood::Shade& shade) {
        bool soft = false;
        unsigned dim = 0;   // 0: not measured yet
        const auto faded = [&](unsigned c) {
            if (!dim) dim = wide_dim ? std::max(1u, wide_dim()) : 256u;
            return dim >= 256 ? uint16_t(c) : uint16_t(((c >> 10 & 31) * dim >> 8) << 10 | ((c >> 5 & 31) * dim >> 8) << 5 | ((c & 31) * dim >> 8));
        };
        for (size_t i = 0; i < wide_pens.size(); ++i) {
            const uint32_t code = wide_pens[i] & ~kWideSoftFlag;
            soft |= code != wide_pens[i];
            wide_frame[i] = 0xff000000u | (code < 0x8000 ? Rgb(palette[code]) : (code & 0xf0000) == kWideColour ? Rgb(faded(code & 0x7fff)) : code == kWideSentinel ? 0xff00ffu : 0u);
        }
        // A far backdrop's soft sides (mk3::Wide::SoftBackdrop): per margin and line the average colour of its soft
        // pixels (the line's last columns), blurred over kSoftRows lines up and down among the lines that have them, and
        // dimmed from the line's innermost soft pixel (all of it) to 45% a margin's width further out.
        if (soft) for (int side = 0; side < 2; ++side) {
            const int first = side ? mk5wide::kWideMargin + mk5wide::kVisible : 0, last = first + mk5wide::kWideMargin;
            std::vector<std::array<unsigned, 4>> line(size_t(kFrameHeight), std::array<unsigned, 4>{});   // r, g, b sums and count
            std::vector<int> inner(size_t(kFrameHeight), -1);
            for (int y = 0; y < kFrameHeight; ++y)
                for (int x = first; x < last; ++x) if (const size_t i = size_t(y) * kWideWidth + size_t(x); wide_pens[i] & kWideSoftFlag) {
                    const uint32_t p = wide_frame[i];
                    line[size_t(y)][0] += p >> 16 & 255; line[size_t(y)][1] += p >> 8 & 255; line[size_t(y)][2] += p & 255; ++line[size_t(y)][3];
                    if (side ? inner[size_t(y)] < 0 : true) inner[size_t(y)] = x;   // nearest the arcade's columns
                }
            for (int y = 0; y < kFrameHeight; ++y) {
                if (!line[size_t(y)][3]) continue;
                std::array<unsigned, 3> sum{};
                unsigned n = 0;
                for (int dy = -mk5wide::kSoftRows; dy <= mk5wide::kSoftRows; ++dy)
                    if (const int at = y + dy; at >= 0 && at < kFrameHeight && line[size_t(at)][3]) {
                        for (size_t c = 0; c < 3; ++c) sum[c] += line[size_t(at)][c] / line[size_t(at)][3];
                        ++n;
                    }
                for (int x = first; x < last; ++x) if (const size_t i = size_t(y) * kWideWidth + size_t(x); wide_pens[i] & kWideSoftFlag) {
                    const unsigned away = unsigned(std::abs(x - inner[size_t(y)])), light = 256 - 141 * std::min(away, unsigned(mk5wide::kWideMargin)) / unsigned(mk5wide::kWideMargin);
                    wide_frame[i] = 0xff000000u | (sum[0] / n * light >> 8) << 16 | (sum[1] / n * light >> 8) << 8 | (sum[2] / n * light >> 8);
                }
            }
        }
        // A page with soft sides whose margins already hold what each arcade line ends in (the game filled it whole: a
        // finisher's white flash, a wipe, black) is shown as it is.
        bool filled = wide_soft_work;
        for (int y = 0; y < kFrameHeight && filled; ++y) {
            const uint32_t* line = wide_frame.data() + size_t(y) * kWideWidth;
            for (int x = 0; x < mk5wide::kWideMargin && filled; ++x)
                filled = line[x] == line[mk5wide::kWideMargin] && line[mk5wide::kWideMargin + mk5wide::kVisible + x] == line[mk5wide::kWideMargin + mk5wide::kVisible - 1];
        }
        if (wide_soft_work && !filled) mk5wide::SoftMargins(wide_frame.data(), unsigned(kFrameHeight), false, [&](size_t i) { wide_source[i] = kRaw; wide_blood[i] = 0; });
        wide_shift = wide_shift_work; wide_whole = wide_whole_work; wide_soft = wide_soft_work;
        wide_centre = wide_centre_work; wide_filled = wide_filled_work; wide_audit = wide_audit_work;
        if (!wide_present || presentation.size() != frame.size()) return;
        const size_t n = wide_frame.size();
        const int move = mk5wide::kWideMargin - wide_shift;
        const bool scenery = backdrop.size() == frame.size(), actors = fighters.serial != 0, actor_backdrop = actors && !fighters.backdrop.empty();
        auto& f = wide_fighters;
        wide_presentation.resize(n); wide_backdrop.resize(scenery ? n : 0);
        f.serial = fighters.serial; f.layers.clear();
        f.underlay.resize(actors ? n : 0); f.backdrop.resize(actor_backdrop ? n : 0); f.opaque_order.resize(actors ? n : 0); f.covered.resize(actors ? n : 0);
        for (size_t i = 0, y = 0; y < size_t(kFrameHeight); ++y)
            for (size_t x = 0, arcade = y * kFrameWidth; x < size_t(kWideWidth); ++x, ++i) {
                const int source = wide_source[i];
                if (source >= 0) {
                    const size_t a = arcade + size_t(source);
                    wide_presentation[i] = 0xff000000u | presentation[a];
                    if (scenery) wide_backdrop[i] = backdrop[a];
                    if (actors) {
                        f.underlay[i] = fighters.underlay[a]; f.opaque_order[i] = fighters.opaque_order[a]; f.covered[i] = fighters.covered[a];
                        if (actor_backdrop) f.backdrop[i] = fighters.backdrop[a];
                    }
                    continue;
                }
                const uint32_t raw = wide_frame[i];
                const uint32_t stain = wide_blood[i] ? 0xff000000u | Rgb(mk5blood::Dim(uint16_t(wide_blood[i] & 0x7fff), shade.now, shade.then)) : 0;
                const uint32_t shown = stain ? stain : raw, back = source == kRaw && !stain ? (raw & 0xffffff) | kSceneryRgb : 0;
                wide_presentation[i] = shown;
                if (scenery) wide_backdrop[i] = back;
                if (actors) {
                    f.underlay[i] = shown & 0xffffff; f.opaque_order[i] = UINT64_MAX; f.covered[i] = 0;
                    if (actor_backdrop) f.backdrop[i] = back;
                }
            }
        if (actors) for (auto layer : fighters.layers) { layer.x += move; f.layers.push_back(std::move(layer)); }   // the fighters are world
    }
    void ResetPartial() { last_partial_scan = 0; }   // scanline-0 timer: reset_partial_updates

    // update_partial(vcount) with the TMS registers and video RAM at that instant.
    void Line(int vcount, const std::array<uint16_t, 32>& io, const std::vector<uint16_t>& vram) {
        if (vcount < last_partial_scan) return;   // already rendered
        const int top = std::max(kTop, last_partial_scan), bottom = std::min(kBottom, vcount);
        if (top > bottom) return;
        last_partial_scan = bottom + 1;
        ++lines;
        uint16_t* dest = &pens[size_t(top) * kWidth];
        int heblnk = io[V::HEBLNK], hsblnk = io[V::HSBLNK];   // m_pixperclock = 1
        size_t row_base = 0; unsigned row_col = 0;
        const bool enabled = io[V::DPYCTL] & 0x8000;
        if (enabled) {   // enabled: midtunit_video_device::scanline_update
            if (hsblnk > kWidth) throw std::runtime_error("MK3 scanout: HSBLNK past the screen bitmap is not modelled");
            uint16_t dpyadr = io[V::DPYADR];
            if (!(io[V::DPYCTL] & 0x0400)) dpyadr ^= 0xfffc;
            const unsigned rowaddr = dpyadr >> 4, coladdr = ((dpyadr & 0x007c) << 4) | (io[V::DPYTAP] & 0x3fff);
            const uint16_t* src = &vram[(rowaddr << 9) & 0x3fe00];
            unsigned col = coladdr << 1;
            row_base = (rowaddr << 9) & 0x3fe00; row_col = col;
            if (layers) {
                if (backdrop_pens.empty()) backdrop_pens.assign(pens.size(), kBlackPen);
                uint32_t* back = &backdrop_pens[size_t(top) * kWidth];
                const size_t base = (rowaddr << 9) & 0x3fe00;
                std::fill(row_blood.begin(), row_blood.end(), uint16_t(0));
                for (int x = heblnk, c = int(col); x < hsblnk; ++x, ++c) {
                    back[x] = layers->Backdrop(base + (c & 0x1ff), src[c & 0x1ff]);
                    row_blood[x] = layers->blood.At(base + (c & 0x1ff)); row_floor_pen[x] = layers->blood.Pen(base + (c & 0x1ff));
                    row_floor[x] = layers->blood.Floor(base + (c & 0x1ff));
                    if (row_blood[x]) back[x] = FloorBlood::kMark | (row_blood[x] & 0x7fffu);
                }
            }
            for (int x = heblnk; x < hsblnk; ++x) dest[x] = src[col++ & 0x1ff] & 0x7fff;
        } else heblnk = hsblnk = kRight + 1;
        for (int x = kLeft; x < heblnk && x <= kRight; ++x) dest[x] = kBlackPen;
        for (int x = std::max(hsblnk, kLeft); x <= kRight; ++x) dest[x] = kBlackPen;
        if (layers) {
            if (backdrop_pens.empty()) backdrop_pens.assign(pens.size(), kBlackPen);
            uint32_t* back = &backdrop_pens[size_t(top) * kWidth];
            for (int x = kLeft; x <= kRight; ++x) if (x < heblnk || x >= hsblnk) back[x] = kBlackPen;
            if (top == kTop) fighter_work.Begin(kFrameWidth, kFrameHeight);
            if (!enabled) std::fill(row_blood.begin(), row_blood.end(), uint16_t(0));
            if (presentation_pens.empty()) presentation_pens.assign(frame.size(), kBlackPen);
            if (presentation_floor.empty()) { presentation_floor_pen.assign(frame.size(), 0); presentation_floor.assign(frame.size(), 0); }
            for (int x = 0; x < kFrameWidth; ++x) {
                const uint16_t stain = row_blood[x + kLeft];
                row_pens[x] = stain ? FloorBlood::kMark | (stain & 0x7fffu) : dest[x + kLeft];
                row_back[x] = stain ? row_pens[x] : (back[x + kLeft] & 0xffff) | (back[x + kLeft] & DmaLayers::kScenery ? kSceneryRgb : 0);
                presentation_pens[size_t(top - kTop) * kFrameWidth + x] = row_pens[x];
                presentation_floor_pen[size_t(top - kTop) * kFrameWidth + x] = stain ? row_floor_pen[x + kLeft] : uint16_t(0);
                presentation_floor[size_t(top - kTop) * kFrameWidth + x] = stain ? row_floor[x + kLeft] : uint8_t(0);
            }
            layers->Flush();
            fighter_work.Line(layers->fighters, unsigned(top - kTop), row_base, row_col, row_pens.data(), row_back.data(),
                              [](uint16_t pen) { return uint32_t(pen); }, enabled && heblnk == kLeft && hsblnk == kRight + 1);
        }
        if (wide) WideLine(top - kTop, enabled && heblnk == kLeft && hsblnk == kRight + 1, row_base, row_col, vram);
    }

    // vblank_begin -> frame_update: the visible area through the palette (xRGB_555, pal5bit expansion).
    void Present(const std::vector<uint16_t>& palette) {
        for (int y = 0; y < kFrameHeight; ++y)
            for (int x = 0; x < kFrameWidth; ++x) {
                const uint16_t pen = pens[size_t(y + kTop) * kWidth + size_t(x + kLeft)];
                frame[size_t(y) * kFrameWidth + x] = pen == kBlackPen ? 0 : Rgb(palette[pen]);
            }
        mk5blood::Shade shade;
        if (layers) {
            if (backdrop_pens.empty()) backdrop_pens.assign(pens.size(), kBlackPen);
            if (presentation_pens.empty()) presentation_pens.assign(frame.size(), kBlackPen);
            if (presentation_floor.empty()) { presentation_floor_pen.assign(frame.size(), 0); presentation_floor.assign(frame.size(), 0); }
            // this frame's stains: their floor pens through the VBLANK palette and then
            for (size_t i = 0; i < frame.size(); ++i)
                if (presentation_pens[i] & FloorBlood::kMark) shade.Cover(presentation_floor_pen[i], presentation_floor[i], palette.data());
            const auto rgb = [&](uint32_t pen) {
                return pen & FloorBlood::kMark ? Rgb(mk5blood::Dim(uint16_t(pen & 0x7fff), shade.now, shade.then))
                                               : (pen & 0xffff) == kBlackPen ? 0 : Rgb(palette[pen & 0x7fff]);
            };
            presentation.resize(frame.size());
            for (size_t i = 0; i < frame.size(); ++i) presentation[i] = rgb(presentation_pens[i]);
            backdrop.resize(frame.size());
            for (int y = 0; y < kFrameHeight; ++y)
                for (int x = 0; x < kFrameWidth; ++x) {
                    const uint32_t pen = backdrop_pens[size_t(y + kTop) * kWidth + size_t(x + kLeft)];
                    backdrop[size_t(y) * kFrameWidth + x] = rgb(pen) | (pen & DmaLayers::kScenery ? kSceneryRgb : 0);
                }
            fighter_work.Complete(frames + 1, fighters);
            if (fighters.serial) {
                for (auto& v : fighters.underlay) v = rgb(v);
                for (auto& v : fighters.backdrop) v = rgb(v) | (v & kSceneryRgb);
                for (auto& l : fighters.layers) for (auto& v : l.pixels) if (v >> 24) v = 0xff000000u | rgb(v);
            }
        }
        if (wide) PresentWide(palette, shade);
        ++frames;
        if (presented) presented(*this);
    }
    // Self-check of `fighters`: its layers over the underlay at 1x (opaque order kept) must give `presentation`; mismatched pixels.
    uint64_t FighterMismatched() const {
        if (!fighters.serial) return 0;
        std::vector<uint32_t> img = fighters.underlay;
        for (const auto& l : fighters.layers)
            for (unsigned y = 0; y < l.height; ++y) for (unsigned x = 0; x < l.width; ++x) {
                const int fx = l.x + int(x), fy = l.y + int(y);
                const uint32_t v = l.pixels[size_t(y) * l.width + x];
                if (!(v >> 24) || fx < 0 || fy < 0 || fx >= kFrameWidth || fy >= kFrameHeight) continue;
                const size_t i = size_t(fy) * kFrameWidth + size_t(fx);
                if (fighters.opaque_order[i] < l.order) img[i] = v;
            }
        uint64_t n = 0;
        for (size_t i = 0; i < img.size(); ++i) n += (img[i] & 0xffffff) != presentation[i];
        return n;
    }
    static uint32_t Rgb(uint16_t c) {
        const auto five = [](unsigned v) { v &= 31; return (v << 3) | (v >> 2); };
        return five(c >> 10) << 16 | five(c >> 5) << 8 | five(c);
    }
};

} // namespace mk3
