// MK3 Remastered: the floor splashes stay on the arena floor for the whole fight and are cleared when a new fight
// starts, as MK1 (mk5_live_video.cpp) and MK2 (mk2_video.hpp). Presentation only: read-only on the program's state,
// never writes video RAM, palette or registers; the pixels are the program's own splash frame, decoded from the user's
// graphics ROM (EnableFloorBlood, mk3_dma.hpp). Facts (addresses only) from the read-only probes of plan 2026-11-19
// (stage A, routes match, r1fin00/01, r2t0, r2e2, r3s00):
// - kFrame: header of the splash frame that rests on the floor (35 x 11). The other frames deleted by the same
//   routine end in the air (drops) and are not kept.
// - kEnqueue: the object walker stages the draw; A10 = video RAM XY of its first pixel (page, camera and flip
//   applied), A5 = page, A4 = camera.
// - kKeep: the splash object deletes itself (A8); the stain stays at the world position of its last staged draw.
// - kRound with A0 = 1: the round counter store of round 1 (a new fight); kArenaSwitch: the arena store of the
//   in-fight arena change. Both clear.
// - kWalk with A0 = kWorldList: the world-object pass; its first staged entry (cursor A1 - 0C0h) is composed when the
//   pump pops it, after the scenery. Page @kPage, camera A4, clip A14 (first) / A13 (past the last, the walker's
//   reject test); arena shown while @kArenaShown != 0.
// The program's fades (the screen fading to black between rounds, the arena darkening behind a finishing move) rewrite the
// arena's palette, even while no pass runs, as the stain keeps the colours of its keep: as MK1/MK2 (mk5_blood_shade.hpp),
// each stain pixel also keeps the pen of the video RAM word it first covered and that pen's brightness then, and the
// scanout's Present dims every stain of the frame by those same pens through the palette of that VBLANK against their
// brightness then (never brightened). Without a palette change the ratio is 1 and the colours are the kept ones,
// exactly, whatever the program draws under the stain later.
// With the blood DIP off the program never creates the splash, so nothing is kept.
#pragma once
#include "mk5_blood_shade.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace mk3 {

struct FloorBlood {
    static constexpr uint32_t kFrame = 0xffc83cb0, kEnqueue = 0xff804410, kKeep = 0xffa6aaf0, kRound = 0xff811430,
        kArenaSwitch = 0xff90ebe0, kWalk = 0xff805060, kWorldList = 0x0105dc10, kPage = 0x0105d7c0, kArenaShown = 0x01062100;
    static constexpr int kWidth = 35, kHeight = 11;
    static constexpr uint32_t kMark = 0x00800000;   // a presentation pen holding an RGB555 colour instead of a palette index
    using Colors = std::array<uint16_t, 16>;         // 4-bit frame: palette slot + 0..15
    struct Staged { int16_t x = 0, y = 0; bool flip = false; };   // world position of the first pixel column/row
    struct Pass { uint32_t cursor = 0, page = 0, camera = 0, clip_start = 0, clip_end = 0; bool shown = false; };
    // Per splash pixel, the floor it first covered: pen | mk5blood::kTaken (0 = not drawn yet) and its Sum then.
    struct Stain { Colors colors{}; std::array<uint16_t, kWidth * kHeight> pen{}; std::array<uint8_t, kWidth * kHeight> floor{}; };

    std::vector<uint8_t> shape;   // kWidth x kHeight palette indices from the ROM (not state); empty = disabled
    std::unordered_map<uint32_t, Staged> staged;   // splash objects drawn since their creation
    std::map<std::tuple<int16_t, int16_t, bool>, Stain> stains;
    std::vector<uint16_t> layer;   // per video RAM word: RGB555 | 0x8000, 0 = no stain
    std::vector<uint16_t> pens;    // per video RAM word under a stain: its Stain::pen (0 = never dimmed)
    std::vector<uint8_t> floor;    // and its Stain::floor
    Pass pass;

    bool enabled() const { return !shape.empty(); }
    uint16_t At(size_t i) const { return layer.empty() ? 0 : layer[i]; }
    uint16_t Pen(size_t i) const { return pens.empty() ? 0 : pens[i]; }
    uint8_t Floor(size_t i) const { return floor.empty() ? 0 : floor[i]; }
    void Mask(size_t i) { if (!layer.empty()) layer[i] = 0; }
    void Mask(size_t i, size_t n) { if (!layer.empty()) std::fill_n(layer.begin() + ptrdiff_t(i), n, uint16_t(0)); }
    void Clear() {
        staged.clear(); stains.clear(); pass.cursor = 0;
        std::fill(layer.begin(), layer.end(), uint16_t(0)); std::fill(pens.begin(), pens.end(), uint16_t(0));
        std::fill(floor.begin(), floor.end(), uint8_t(0));
    }

    // Before every TMS instruction (DmaLayers::Step); ram: main RAM words, palette: palette RAM.
    void Step(uint32_t pc, const uint32_t* a, const std::vector<uint16_t>& ram, const std::vector<uint16_t>* palette) {
        if (!enabled()) return;
        const auto word = [&](uint32_t bit) -> uint16_t {
            const size_t i = (bit - 0x01000000) / 16;
            return bit >= 0x01000000 && i < ram.size() ? ram[i] : 0;
        };
        const auto splash = [&](uint32_t o) { return !(o & 15) && (word(o + 0x1a0) | uint32_t(word(o + 0x1b0)) << 16) == kFrame; };
        switch (pc) {
        case kRound: if (a[0] == 1) Clear(); break;
        case kArenaSwitch: Clear(); break;
        case kEnqueue:
            if (splash(a[0])) {
                const bool flip = word(a[0] + 0xf0) & 0x10;   // the DMA start is the right column when mirrored
                staged[a[0]] = {int16_t(int16_t(a[10]) - (flip ? kWidth - 1 : 0) - int16_t(a[5]) + int16_t(a[4])),
                                int16_t(int16_t(a[10] >> 16) - int16_t(a[5] >> 16) + int16_t(a[4] >> 16)), flip};
            }
            break;
        case kKeep:
            if (const auto it = staged.find(a[8]); it != staged.end() && splash(a[8]) && palette) {
                const size_t slot = word(a[8] + 0x140) & 0x7f00;   // the slot is freed with the object: keep its colours
                auto& stain = stains[{it->second.x, it->second.y, it->second.flip}];
                for (size_t k = 0; k < stain.colors.size(); ++k) stain.colors[k] = (*palette)[slot + k];
                staged.erase(it);
            }
            break;
        case kWalk:
            if (a[0] == kWorldList) pass = {a[1] - 0xc0, uint32_t(word(kPage)) | uint32_t(word(kPage + 0x10)) << 16, a[4], a[14], a[13],
                                            word(kArenaShown) != 0};
            break;
        }
    }
    // A DMA popped by the pump with this queue cursor (DmaLayers::Begin): the world pass's first entry composes over
    // `vram` (the words stored so far, DmaLayers::stored), whose pens are the floor references, with their brightness
    // through `palette` (null: never dimmed).
    void Pump(uint32_t cursor, const std::vector<uint16_t>& vram, const std::vector<uint16_t>* palette) {
        if (!pass.cursor || pass.cursor != cursor) return;
        pass.cursor = 0;
        Draw(pass, vram, palette);
    }
    void Draw(const Pass& p, const std::vector<uint16_t>& vram, const std::vector<uint16_t>* palette) {
        const int page_x = uint16_t(p.page), page_y = int(p.page >> 16);
        if (page_y != 0 && page_y != 256) return;
        if (pens.size() != layer.size()) { pens.assign(layer.size(), 0); floor.assign(layer.size(), 0); }
        std::fill_n(layer.begin() + page_y * 512, 256 * 512, uint16_t(0));
        std::fill_n(pens.begin() + page_y * 512, 256 * 512, uint16_t(0));
        std::fill_n(floor.begin() + page_y * 512, 256 * 512, uint8_t(0));
        if (!p.shown) return;
        const int camera_x = int16_t(p.camera), camera_y = int16_t(p.camera >> 16);
        const int left = int16_t(p.clip_start), top = int16_t(p.clip_start >> 16), right = int16_t(p.clip_end), bottom = int16_t(p.clip_end >> 16);
        for (auto& [pose, stain] : stains) {
            const auto [origin_x, origin_y, flip] = pose;
            for (int dy = 0; dy < kHeight; ++dy) {
                const int y = origin_y - camera_y + dy;
                if (y < top || y >= bottom || y >= 256) continue;
                for (int dx = 0; dx < kWidth; ++dx) {
                    const int x = origin_x - camera_x + dx;
                    const size_t k = size_t(dy) * kWidth + size_t(flip ? kWidth - 1 - dx : dx);
                    if (!shape[k] || x < left || x >= right || x + page_x >= 512) continue;
                    const size_t i = size_t(page_y + y) * 512 + size_t(page_x + x);
                    if (palette && !stain.pen[k]) {
                        stain.pen[k] = uint16_t(vram[i] | mk5blood::kTaken);
                        stain.floor[k] = uint8_t(mk5blood::Sum((*palette)[vram[i] & 0x7fff]));
                    }
                    layer[i] = uint16_t(stain.colors[shape[k]] | 0x8000);
                    pens[i] = stain.pen[k]; floor[i] = stain.floor[k];
                }
            }
        }
    }
};

} // namespace mk3
