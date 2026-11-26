#pragma once
// MK3 rev 2.1 widescreen (Remastered presentation only; rules in docs/WIDESCREEN.md, the layer in mk5_wide.hpp, the
// game's address facts in mk3_widescreen.hpp). Once per frame, where the frame builder starts its display table walk,
// the shadow pass runs the generated code again on the same machine: main RAM stores go to a journal that is undone
// afterwards, no cycle, record or note is kept, any device access ends the pass as a fault (Ops::shadow_on). With the
// clip longs widened to the 16:9 window and the scrolls at the wide camera, the scenery updater (when the game runs
// one) and then the walk stage their entries, which are drawn into the wide layer in their order. The real walk's own
// entries are then left out of the layer; every other real draw (direct DMAs, the entries staged outside the walk, CPU
// stores, the page clear) is mirrored as it happens.
// Wide is the Board's DMA and CPU video RAM observer in front of the Remastered layers (DmaLayers), and Step runs
// before every TMS instruction (Board::stop).
#include "mk3_board.hpp"
#include "mk3_dma_observer.hpp"
#include "mk3_widescreen.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace mk3 {

struct Wide : DmaObserver, CpuVramObserver {
    using Layer = mk5wide::Layer<uint16_t>;   // the floor stains as FloorBlood::layer keeps them (RGB555 | 8000h)
    // A layer pixel of a draw the arcade has not made yet (an object only the shadow's updater built): in the arcade's
    // columns it fills the arcade's pop-in at a fast scroll (mk5wide::Audit::filled).
    static constexpr uint8_t kFresh = 32;
    // A margin pixel past a far backdrop's end, shown soft (SoftBackdrop; the scanout resolves its colour).
    static constexpr uint8_t kSoft = 64;
    // A far plane's end that the arcade shows within its last columns at its camera limit (a margin's width of them)
    // still counts as an end the plane is held at (mk5wide::Layer::seen): an arena's wall that stops short of the
    // arcade's last screen, which shows the page's background past it.
    static constexpr int kSeen = mk5wide::kWideMargin;
    Layer layer;
    Board* board = nullptr;
    DmaObserver* next_dma = nullptr;        // the Remastered layers
    CpuVramObserver* next_cpu = nullptr;
    const FloorBlood* blood = nullptr;      // the kept stains (null: none)

    // State (Fields): the scenery updater has run since the pool reset / is running; this frame's walk entries are in the
    // layer (their real DMAs are not mirrored); the real walk's entries (queue cursors); the shadow's entries after the
    // table's marker, which wait until the real pump has drawn the floor shadows (kMarkerDone).
    bool streamer = false, streaming = false, walk_drawn = false, shadow_post = false;
    uint32_t updater_idle = 0;   // display interrupts since the game last called its updater (up to kUpdaterStopped)
    // This fight's build shows scenery the game is moving by hand, built for its own window alone: the passage between
    // two arenas (a fight under a screen's camera limits: the old arena scrolls away by draws outside the walk), or
    // a finishing move that scrolls the arena away (the shadow's updater gave up: the pass went without it). Its
    // sides are soft.
    bool hand = false;
    std::vector<uint32_t> walk_entries;
    struct Draw {
        std::array<uint16_t, 12> entry{};
        bool fixed = false, fresh = false, stains = false;   // stains: no entry, the floor stains of the world pass (entry: camera, page)
        int16_t layer = -1;
        template<class A> friend void Fields(A& a, Draw& d) { a(d.entry, d.fixed, d.fresh, d.stains, d.layer); }
    };
    std::vector<Draw> pending;
    uint64_t faults = 0, streamer_skips = 0, passes = 0;   // diagnostics (kept with the state, so a loaded session logs the same)
    uint32_t fault_pc = 0;                                  // bit 0: in the walk; bit 1: an exception
    template<class A> friend void Fields(A& a, Wide& w) {
        w.layer.LayerFields(a);
        a(w.layer.builds, w.layer.ended, w.layer.idle, w.layer.idle_serial);   // the builds' count and the pause without one (Layer::EndFrame)
        a(w.streamer, w.streaming, w.walk_drawn, w.shadow_post, w.walk_entries, w.pending, w.fill_start, w.fill_count,
          w.faults, w.streamer_skips, w.passes, w.fault_pc, w.updater_idle, w.cpu_next, w.cpu_run, w.hand);
    }

    void Start() { layer.Enable(board->o.vram, true); layer.seen = kSeen; layer.edges = false; }
    // A state saved without widescreen was loaded: the layer starts over from what VRAM holds.
    void Restart() {
        layer.Reset(); Start();
        streamer = streaming = walk_drawn = shadow_post = hand = false; walk_entries.clear(); pending.clear(); fill_count = cpu_run = 0; updater_idle = 0;
    }
    void Attach(Board& b, DmaObserver* dma, CpuVramObserver* cpu, const FloorBlood* stains) {
        board = &b; next_dma = dma; next_cpu = cpu; blood = stains;
        b.o.blitter.observer = this; b.o.vram_observer = this;
        Start();
    }

    // The game's fade right now, of 256: per palette slot, its colours' light in the palette RAM against the ROM palette
    // the slot holds; the median slot (the HUD's palettes do not fade with the scene). 256 when nothing fades. A colour
    // of an object only the wide picture has (mk5wide::Layer::Book) takes it, as the palette RAM would hold the colour
    // had the game loaded it.
    unsigned Dim() const {
        const auto& o = board->o;
        const auto light = [](unsigned c) { return (c >> 10 & 31) + (c >> 5 & 31) + (c & 31); };
        std::vector<unsigned> slots;
        for (unsigned slot = 0; slot < wide::kPaletteSlots; ++slot) {
            const uint32_t pointer = Long(wide::kPaletteTable + slot * wide::kPaletteEntry);
            if (pointer < kProgramBaseBit || size_t(pointer - kProgramBaseBit) / 8 + 2 * 257 > o.program.bytes.size()) continue;
            unsigned was = 0, now = 0;
            for (unsigned k = 0, count = std::min(unsigned(o.program.Word(pointer)), 256u); k < count; ++k) {
                was += light(o.program.Word(pointer + 16 + 16 * k)); now += light(o.palette[slot * 256 + k]);
            }
            if (was) slots.push_back(std::min(256u, now * 256 / was));
        }
        if (slots.empty()) return 256;
        std::nth_element(slots.begin(), slots.begin() + ptrdiff_t(slots.size() / 2), slots.end());
        return slots[slots.size() / 2];
    }
    // Before every TMS instruction of the game.
    void Step() {
        namespace w = wide;
        auto& s = board->s;
        switch (s.pc) {
        case w::kDisplayIrq: {   // a new frame (entries still waiting: no marker branch ran)
            Flush();
            if (updater_idle < w::kUpdaterStopped) ++updater_idle;
            // A far plane's picture no wider than the arcade's screen (the versus screen) is no arena: its sides are soft.
            if (!layer.wider) layer.scene = false;
            // A fight that stops building while it wipes the screen (a finisher's flash: mk5wide::Layer::EndFrame lets
            // the pages go) is still the fight when its builds come back: its planes stay where they were held.
            const bool fight = layer.fight && w::FightMode(Word(w::kMode));
            layer.EndFrame();
            if (layer.released && fight) layer.fight = true;
            // A fight's build that leaves its margins without art (a finishing move's own scene, no wider than the
            // arcade's screen: a pit under the arena), or whose scenery the game moves by hand, gets soft sides.
            else if (layer.fight && (hand || Bare())) layer.page_soft[layer.page] = true;
            // A frozen picture keeps the fight's sides until the screen draws over it; from then on they are soft.
            else if (Frozen()) layer.page_soft[layer.page] = layer.serial != layer.page_start[layer.page];
            break;
        }
        case w::kStreamer: streamer = streaming = true; updater_idle = 0; break;
        case w::kStreamerEnd: streaming = false; break;
        case w::kPoolReset: streamer = false; NewArena(); break;
        case w::kArenaSwitch: NewArena(); break;
        case w::kQueueReset: {   // the frame's build starts on its page, as VRAM holds it after the page clear
            static const std::vector<uint16_t> none;
            const int camera = int16_t(Word(w::kCameraX));
            const unsigned row = Word(w::kPageOrigin + 16) & 0x1ff;
            // A screen's camera limits end the fight, wherever its camera stands. Over a page the game has not cleared,
            // whose last build had the wide window's own picture, the screen is drawn on the fight's last picture
            // (the match's end: the frozen picture, then the continue screen's doors): the page keeps that picture
            // and its window, and this build's draws keep their screen place over it (Frozen).
            const bool screen = ScreenNow();
            layer.StartFrame(row, board->o.vram, blood ? blood->layer : none, screen || camera == layer.held[0]);
            if (screen && layer.whole) { layer.shift = 0; layer.fight = false; }
            walk_entries.clear(); pending.clear(); walk_drawn = shadow_post = hand = false;
            break;
        }
        case w::kDrawerStage: case w::kFloorStage: walk_entries.push_back(s.a[1] - 0xc0); break;
        case w::kMarkerDone: case w::kMarkerEnd: Flush(); break;
        case w::kWalkStart: Shadow(); break;
        }
    }

    // The built page has kPicture lines or more with kSliver margin pixels or more that show no art.
    bool Bare() const {
        int lines = 0;
        for (size_t row = size_t(layer.page) * 256; row < size_t(layer.page) * 256 + 254; ++row) {
            int bare = 0;
            for (int L = 0; L < mk5wide::kLayerWidth; ++L) bare += !Layer::Arcade(L) && !layer.Shown(row * mk5wide::kLayerWidth + size_t(L));
            lines += bare >= Layer::kSliver;
        }
        return lines >= Layer::kPicture;
    }
    // This build is a screen's, drawn over the fight's last wide picture, which its page keeps with its window
    // (kQueueReset): mk5wide::Layer::whole with no fight (a fight's build that is whole is the fight's).
    bool Frozen() const { return layer.whole && !layer.fight; }
    // The camera limits are a screen's, not an arena's: a fight's last window is not this build's, even if its camera
    // stands where the fight's did (a match ended at the arena's left end, then the continue screen at camera 0).
    bool ScreenNow() const { return wide::ScreenLimits(int16_t(Word(wide::kCameraMin)), int16_t(Word(wide::kCameraMax))); }

    // Another arena's planes: what was measured of the last one's (ratios, art ends) is forgotten.
    void NewArena() { layer.ratio = {}; layer.bound = layer.want = Layer::Unbound(); layer.span = Layer::NoSpan(); }

    // DmaObserver: a real draw. A walk entry the shadow drew is left out; one it did not draw (no shadow this frame) is
    // scenery. The pump's entries outside the walk (text, bars) keep their screen place; every other real DMA draws the
    // world in the arcade's window and lands at the wide window's column.
    void Begin(const DmaDraw& d) override {
        if (next_dma) next_dma->Begin(d);
        mirror_ = false;
        if (!layer.Enabled()) return;
        const bool pump = board->o.instruction_pc == wide::kPump;
        const auto own = pump ? std::find(walk_entries.begin(), walk_entries.end(), board->s.b[14]) : walk_entries.end();
        const bool walked = own != walk_entries.end();
        if (walked) walk_entries.erase(own);
        if (walked && walk_drawn) return;
        mirror_ = true; mirror_fixed_ = pump && !walked; mirror_actor_ = Layer::ActorDraw(d.command) ? Layer::kActor : 0;
        mirror_shift_ = mirror_fixed_ ? 0 : layer.shift;
        mirror_fill_ = (d.command & 15) == 12 && !mirror_shift_;   // one colour whatever the source: a clear or a wipe
        const auto [lo, hi] = mk5wide::Span(uint16_t(d.x), uint16_t(d.width), d.xstep, (d.command & 0x10) != 0);
        layer.Draw(mk5wide::Kind::real, lo + mk5wide::kExtra - mirror_shift_, hi + mk5wide::kExtra - mirror_shift_);
    }
    void Pixel(size_t index, uint16_t value) override {
        if (next_dma) next_dma->Pixel(index, value);
        if (!mirror_) return;
        const int column = int(index & 511) + mk5wide::kExtra - mirror_shift_;
        if (column < 0 || column >= mk5wide::kLayerWidth) return;
        const size_t at = (index >> 9) * mk5wide::kLayerWidth + size_t(column);
        layer.Write(at, value, mirror_fixed_);
        layer.fixed[at] |= mirror_actor_;
        // A fill that reaches the VRAM line's end column goes on over the layer's columns past it, as a line copy does.
        if (const size_t c = index & 511; mirror_fill_ && (c == 0 || c == 511))
            for (int L = c ? mk5wide::kWidth + mk5wide::kExtra : 0; L < (c ? mk5wide::kLayerWidth : mk5wide::kExtra); ++L)
                layer.Write((index >> 9) * mk5wide::kLayerWidth + size_t(L), value, mirror_fixed_);
    }
    // CpuVramObserver: the page clear's shift-register transfers are line copies (two VRAM lines each); any other store
    // is a pixel at its VRAM column.
    // Two whole lines stored in a row are a clear, by the page clear or by a loop of the CPU's. One on the page of the
    // build in progress, after its walk and before the pump draws it (the continue screen's doors opening over a page
    // the game has just wiped), takes the shadow's draws with it: the real ones are then mirrored as they come.
    void CpuVram(uint32_t pc, size_t word, uint16_t value, uint16_t mask, uint64_t cycle) override {
        if (next_cpu) next_cpu->CpuVram(pc, word, value, mask, cycle);
        if (!layer.Enabled()) return;
        const auto wiped = [&](size_t first) { if (((first >> 17) & 1) == layer.page) walk_drawn = false; };
        if (pc != wide::kPageFill) {
            const auto& vram = board->o.vram;
            layer.Mirror(vram, word, 1);
            cpu_run = word == cpu_next ? cpu_run + 1 : 1; cpu_next = uint32_t(word + 1);
            // A whole line stored in one run, all in one pen, is cleared as the page clear leaves it: the page's
            // background, not art (the power-on clear of video RAM, under the advisory screen's bars).
            if (const size_t first = word - 511; (word & 511) == 511 && cpu_run >= 512 &&
                std::all_of(vram.begin() + ptrdiff_t(first), vram.begin() + ptrdiff_t(word) + 1, [&](uint16_t v) { return v == vram[first]; }))
                layer.Mirror(vram, first, 512);
            if (cpu_run % 1024 == 0) wiped(word);
            return;
        }
        if (!fill_count || word != fill_start + fill_count) { fill_start = uint32_t(word); fill_count = 0; }
        if (++fill_count == 1024) {
            layer.MarkCleared(fill_start); layer.Mirror(board->o.vram, fill_start, 1024); wiped(fill_start);
            fill_count = 0;
        }
    }

private:
    bool mirror_ = false, mirror_fixed_ = false, mirror_fill_ = false;
    uint8_t mirror_actor_ = 0;
    int mirror_shift_ = 0;
    uint32_t fill_start = 0, fill_count = 0;   // the page clear's transfer in progress (state)
    uint32_t cpu_next = 0, cpu_run = 0;        // the CPU's stores in a row: the word after the last one, their count (state)
    // One pass (not state): the list being walked.
    struct Staged { std::array<uint16_t, 12> entry; bool fixed, fresh; };
    std::vector<Staged> plane_;
    bool hud_ = false;
    int list_ = 0, slot_ = -1, wide_lines_ = 0;
    int16_t camera_ = 0;
    std::vector<uint32_t> free_;                                  // the pool's free objects when the pass starts (sorted)
    // A far plane's own pixels as measured, per VRAM row in canvas columns: its outermost ones, the arcade columns it
    // covers, and the sides where one of its draws reaches past the canvas (1 left, 2 right).
    static constexpr size_t kCoverWords = (mk5wide::kVisible + 63) / 64;
    struct Extent { std::vector<int16_t> lo, hi; std::vector<uint16_t> cover; std::vector<uint64_t> bits; std::vector<uint8_t> reach; } extent_;
    std::array<uint32_t, wide::kPaletteSlots> palettes_{};        // the palette slots' ROM pointers when the pass starts

    uint16_t Word(uint32_t bit) const { return board->o.ram[(bit - 0x01000000) / 16]; }
    uint32_t Long(uint32_t bit) const { return RamLong(board->o.ram, bit); }

    // The shadow's clip and camera, written through the journal: with a window offset the world's scroll moves by it and
    // each far plane's by its ratio times it, held where its art covers the wide screen (mk5wide::Layer::PlaneScroll).
    void View() {
        namespace w = wide;
        auto& o = board->o;
        o.Write32(w::kClipTopLeft, w::WideTopLeft()); o.Write32(w::kClipBottomRight, w::WideBottomRight());
        const int camera = int16_t(Word(w::kCameraX));
        const int64_t offset = mk5wide::ScrollOffset(int32_t(Long(w::kScrollFirst + w::kWorldSlot * 0x20)), layer.shift, layer.AtLimit(camera));
        for (unsigned slot = 0; slot < w::kScrollSlots; ++slot) {
            const uint32_t scroll = w::kScrollFirst + slot * 0x20;
            const auto arcade = int32_t(Long(scroll));
            const auto wide_scroll = slot == w::kWorldSlot ? int32_t(arcade + offset) : layer.PlaneScroll(slot, arcade, camera, offset);
            if (wide_scroll != arcade) { o.Write32(scroll, uint32_t(wide_scroll)); layer.Whole(); }
            // The floor follows this slot's velocity from its own longs, each line faster than the one above it (the
            // fighters stand on the line that moves with the world): they move by the slot's share of the window's
            // offset, whether or not the slot's plane is held at its art's end, so the floor stays under the fighters.
            if (offset && Long(w::kFloorVelocity) == scroll - w::kScrollVelocityGap) {
                const auto& r = layer.ratio[slot];
                const int32_t moved = int32_t(r.world ? offset * r.plane / r.world : offset);
                o.Write32(w::kFloorX, Long(w::kFloorX) - uint32_t(moved));
                if (Long(w::kFloorRoutine) == w::kFloorPerspective) o.Write32(w::kFloorSlope, Long(w::kFloorSlope) - uint32_t(moved >> w::kFloorSlopeShift));
            }
        }
    }
    void Undo() {
        auto& o = board->o;
        for (auto it = o.shadow_journal.rbegin(); it != o.shadow_journal.rend(); ++it) o.ram[it->first] = it->second;
        o.shadow_journal.clear(); o.shadow_fault = false;
    }

    void Shadow() {
        namespace w = wide;
        auto& s = board->s;
        auto& o = board->o;
        if (!layer.Enabled() || o.shadow_on) return;
        if (Long(w::kClipTopLeft) != 0 || Long(w::kClipBottomRight) != w::kArcadeBottomRight) return;   // not the arcade's clip: the real walk is mirrored
        ++passes;
        const auto a = s.a; const auto b = s.b;
        const uint32_t status = s.status, pc = s.pc, at_pc = o.instruction_pc;
        const uint64_t cycles = s.cycles, instructions = s.instructions, at_cycles = o.instruction_cycles;
        const unsigned elapsed = o.elapsed;
        const auto restore = [&] { s.a = a; s.b = b; s.status = status; s.pc = pc; s.cycles = cycles; s.instructions = instructions; };
        plane_.clear(); hud_ = false; list_ = 0; slot_ = -1; camera_ = 0; wide_lines_ = 0;
        free_.clear();
        uint32_t object = Long(w::kFreeList), link = w::kFreeList;
        for (; object >= 0x01000000 && object < 0x013fffe0 && free_.size() < w::kPoolObjects; link = object, object = Long(object)) free_.push_back(object);
        std::sort(free_.begin(), free_.end());
        for (unsigned slot = 0; slot < w::kPaletteSlots; ++slot) palettes_[slot] = Long(w::kPaletteTable + slot * w::kPaletteEntry);
        {   // the frame's window offset; a moving world gives each plane's ratio (its velocity over the world's)
            const unsigned mode = Word(w::kMode);
            const int camera = int16_t(Word(w::kCameraX)), low = int16_t(Word(w::kCameraMin)), high = int16_t(Word(w::kCameraMax));
            layer.Offset(w::Shifting(mode) || w::Demo(mode, low, high, layer.arena, layer.fight, camera == layer.held[0]), camera, low, high,
                         mode != w::kAttractMode && !w::ScreenLimits(low, high));
            hand = layer.fight && w::ScreenLimits(low, high);
            if (const auto world = int32_t(Long(w::kScrollFirst + w::kWorldSlot * 0x20 - w::kScrollVelocityGap)))
                for (unsigned slot = 0; slot < w::kScrollSlots; ++slot)
                    layer.PlaneRatio(slot) = mk5wide::MeasuredRatio(int32_t(Long(w::kScrollFirst + slot * 0x20 - w::kScrollVelocityGap)), world);
        }
        o.shadow_on = true; o.shadow_fault = false; o.shadow_journal.clear();
        View();
        // A free list that does not end in 0: the game never takes its last object, the shadow's wider window can. In the
        // journal the last link is 0, so the allocator finds the pool empty instead of reading that value as an object.
        if (object) o.Write32(link, 0);
        bool ok = true;
        // Not while the interrupted main process is inside the real updater: its lists are half updated. Only in a fight's
        // build: there a block the updater builds early is scenery entering the screen; on another screen it would show
        // a picture the game has not built yet (the continue screen's doors, staged frames before the game draws them).
        if (streamer && !streaming && layer.fight) {
            o.Push(w::kStreamerReturn); s.pc = w::kStreamer;
            if (!Run(w::kStreamerReturn, false)) {   // the updater met something it cannot do: this frame's walk without it
                Undo(); restore(); ++streamer_skips; View(); hand = true;
                if (object) o.Write32(link, 0);
            }
            s.a = a; s.b = b; s.status = status; s.pc = pc;
        }
        ok = Run(w::kWalkEnd, true) && PlaneEnd();
        if (!ok) {   // an incomplete pass: the real walk, mirrored, centred
            ++faults;
            layer.bands.clear(); pending.clear(); shadow_post = false;
            layer.page_shift[layer.page] = layer.shift = 0; layer.page_whole[layer.page] = layer.whole = false;
        }
        walk_drawn = ok;
        Undo(); restore();
        o.shadow_on = false; o.instruction_pc = at_pc; o.instruction_cycles = at_cycles; o.elapsed = elapsed;
    }

    // The generated code from s.pc until `until`; false: a fault (its PC in fault_pc).
    bool Run(uint32_t until, bool walk) {
        namespace w = wide;
        auto& s = board->s;
        auto& o = board->o;
        constexpr uint64_t kSteps = 4000000;
        for (uint64_t steps = 0;; ++steps) {
            const uint32_t pc = s.pc;
            if (o.shadow_fault) { fault_pc = o.instruction_pc | (walk ? 1u : 0u); return false; }
            if (pc == until) return true;
            if (pc < kProgramBaseBit || steps > kSteps) { fault_pc = pc | (walk ? 1u : 0u); return false; }
            // The game's own updater has stopped and the shadow's would free an object the game still keeps: the game
            // is moving its scenery by hand (a finishing move that scrolls the arena away). This frame goes without it.
            if (pc == w::kPlaneFree && updater_idle >= w::kUpdaterStopped && !std::binary_search(free_.begin(), free_.end(), s.a[8])) { fault_pc = pc; return false; }
            if (walk) {
                if (pc == w::kKick) { s.pc = w::kKickEnd; continue; }
                switch (pc) {
                case w::kMarker: if (!PlaneEnd()) return false; shadow_post = true; break;
                case w::kTableNext: if (!PlaneEnd()) return false; ++list_; slot_ = -1; hud_ = false; break;
                case w::kCameraLoad:   // a list with a screen-fixed camera (HUD) keeps its place
                    hud_ = !w::ScrollCamera(s.a[13]);
                    camera_ = hud_ ? int16_t(0) : int16_t(Word(s.a[13]));
                    slot_ = w::FarSlot(s.a[13]);
                    break;
                case w::kListWalk:   // the world pass: the floor stains lie under its objects
                    if (s.a[0] == w::kWorldList && blood && !Frozen()) {   // a frozen picture keeps its stains
                        Draw stains; stains.stains = true;
                        stains.entry[0] = uint16_t(s.a[4]); stains.entry[1] = uint16_t(s.a[4] >> 16);
                        if (!PlaneEnd()) return false;
                        Place(stains);
                    }
                    break;
                case w::kDrawerStage: {   // A12 r0/r1, A11 r2/r3, A10 r4/r5, A9 r6/r7, A8 r8/r9, A6 r10/r11; A0: the object
                    const unsigned regs[6] = {12, 11, 10, 9, 8, 6};
                    Stage(Entry(regs), hud_ || (s.a[7] & w::kFixedObject), std::binary_search(free_.begin(), free_.end(), s.a[0]));
                    break;
                }
                case w::kFloorStage: {    // the floor producer: A9 r0/r1, A8 r2/r3, A7 r4/r5, A6 r6/r7, A3 r8/r9, A2 r10/r11
                    const unsigned regs[6] = {9, 8, 7, 6, 3, 2};
                    auto entry = Entry(regs);
                    // Each floor line is one unscaled 400-pixel row (at the shadow's camera): the same row of the floor
                    // art from the wide window's left edge (67 pixels earlier) and as wide as the layer.
                    if (entry[6] == 400 && entry[7] == 1 && !(entry[1] & 0x10)) {
                        const unsigned code = (entry[1] >> 12) & 7, bpp = code ? code : 8;
                        const uint32_t source = (entry[2] | uint32_t(entry[3]) << 16) - uint32_t(mk5wide::kWideMargin) * bpp;
                        entry[2] = uint16_t(source); entry[3] = uint16_t(source >> 16);
                        entry[4] = uint16_t(entry[4] - mk5wide::kWideMargin); entry[6] = mk5wide::kLayerWidth;
                    }
                    Stage(entry, false, false);
                    break;
                }
                }
            }
            try { StepGenerated(o); } catch (const std::runtime_error&) { fault_pc = pc | 2u | (walk ? 1u : 0u); return false; }
        }
    }
    std::array<uint16_t, 12> Entry(const unsigned regs[6]) const {
        std::array<uint16_t, 12> entry{};
        for (unsigned k = 0; k < 6; ++k) { entry[2 * k] = uint16_t(board->s.a[regs[k]]); entry[2 * k + 1] = uint16_t(board->s.a[regs[k]] >> 16); }
        return entry;
    }
    // A staged entry waits for its list's end, so that the list's art is measured whole (mk5wide::MeasureBand). One in
    // a palette slot the pass itself booked for another palette takes its colours from the ROM (mk5wide::Layer::Book);
    // it is left out when the slot holds no ROM palette.
    void Stage(const std::array<uint16_t, 12>& entry, bool fixed, bool fresh) {
        namespace w = wide;
        const unsigned slot = (entry[8] >> 8) & 0x7f;
        if (slot < w::kPaletteSlots) {
            const uint32_t pointer = Long(w::kPaletteTable + slot * w::kPaletteEntry);
            if (pointer != palettes_[slot] && !layer.booked[slot]) {
                const auto& rom = board->o.program.bytes;
                if (pointer < kProgramBaseBit || size_t(pointer - kProgramBaseBit) / 8 + 2 * 257 > rom.size()) return;
                layer.Book(slot, pointer, [&](uint32_t bit) { return board->o.program.Word(bit); });
            }
        }
        plane_.push_back({entry, fixed || Frozen(), fresh});   // over a frozen picture every draw is the screen's
    }
    // An entry's registers for the layer: x in layer columns, the layer's own columns as its clip.
    std::array<uint16_t, 18> LayerRegisters(const std::array<uint16_t, 12>& entry) const {
        auto r = board->o.blitter.reg;
        std::copy(entry.begin(), entry.end(), r.begin());
        r[Dma::XSTART] = mk5wide::LayerX(r[Dma::XSTART]); r[Dma::LEFTCLIP] = 0; r[Dma::RIGHTCLIP] = mk5wide::kLayerWidth - 1;
        return r;
    }
    // Registers drawn through the board's blitter into a destination `stride` columns wide; false: it draws nothing the
    // board would (no start bit, a source outside the graphics) or a read the board refuses.
    // check: every ROM read is made before anything is stored.
    template<class Put> bool Render(const std::array<uint16_t, 18>& r, size_t stride, bool check, Put&& put) const {
        const uint16_t command = r[Dma::COMMAND];
        uint32_t source = 0;
        if (!(command & 0x8000) || !Dma::Source(command, r[Dma::OFFSETLO], r[Dma::OFFSETHI], source)) return false;
        const unsigned xstep = r[Dma::SCALE_X] ? r[Dma::SCALE_X] : 0x100, ystep = r[Dma::SCALE_Y] ? r[Dma::SCALE_Y] : 0x100;
        const auto& blitter = board->o.blitter;
        try {
            if (check) blitter.Blit(r, command, source, xstep, ystep, false, stride, [](size_t, uint16_t) {});
            blitter.Blit(r, command, source, xstep, ystep, true, stride, put);
        } catch (const std::runtime_error&) { return false; }
        return true;
    }
    // The waiting list ends: its scenery entries are measured over the shadow's whole clip (the layer and kReach columns
    // on each side, so a far plane's art ends are known before they reach the wide screen) and all are placed in order.
    bool PlaneEnd() {
        if (plane_.empty()) return true;
        constexpr int kOrigin = mk5wide::kReach, kCanvas = mk5wide::kLayerWidth + 2 * mk5wide::kReach;
        std::vector<std::array<uint16_t, 18>> draws;
        std::vector<std::pair<int, int>> meant;
        const bool backdrop = slot_ >= 0 && layer.fight && !shadow_post;   // a far plane of a fight's build, placed now
        if (backdrop) {
            extent_.lo.assign(512, int16_t(kCanvas)); extent_.hi.assign(512, 0); extent_.cover.assign(512, 0);
            extent_.bits.assign(512 * kCoverWords, 0); extent_.reach.assign(512, 0);
        }
        std::vector<size_t> of;          // each measured draw's entry
        std::vector<uint8_t> refused(plane_.size(), 0);   // the board would not draw it: left out of the layer
        for (const auto& e : plane_) {
            if (e.fixed) continue;
            of.push_back(size_t(&e - plane_.data()));
            const auto [lo, hi] = mk5wide::Span(e.entry[4], e.entry[6], e.entry[10], (e.entry[1] & 0x10) != 0);
            auto canvas = LayerRegisters(e.entry);
            canvas[Dma::XSTART] = uint16_t((e.entry[4] + kOrigin + mk5wide::kExtra) & 0x3ff); canvas[Dma::RIGHTCLIP] = kCanvas - 1;
            draws.push_back(canvas); meant.push_back({lo + kOrigin + mk5wide::kExtra, hi + kOrigin + mk5wide::kExtra});
            if (layer.debug) layer.notes.push_back({uint16_t(list_), uint32_t(e.entry[2] | uint32_t(e.entry[3]) << 16), uint16_t(e.entry[5] & 0xff),
                                                    int16_t(lo + mk5wide::kExtra), int16_t(hi + mk5wide::kExtra), camera_});
        }
        if (!draws.empty()) {
            auto band = mk5wide::MeasureBand(uint16_t(list_), draws, meant, [&](const std::array<uint16_t, 18>& r, auto&& put) {
                if (!Render(r, kCanvas, false, [&](size_t index, uint16_t) {
                        put(index);
                        if (!backdrop) return;
                        const size_t row = index / size_t(kCanvas);
                        const int at = int(index % size_t(kCanvas)), c = at - kOrigin - mk5wide::kWideMargin;
                        if (row >= 512) return;
                        extent_.lo[row] = std::min(extent_.lo[row], int16_t(at)); extent_.hi[row] = std::max(extent_.hi[row], int16_t(at + 1));
                        if (c < 0 || c >= mk5wide::kVisible) return;
                        uint64_t& word = extent_.bits[row * kCoverWords + size_t(c) / 64];
                        if (const uint64_t bit = uint64_t(1) << (c % 64); !(word & bit)) { word |= bit; ++extent_.cover[row]; }
                    })) refused[of[size_t(&r - draws.data())]] = 1; }, kCanvas, kOrigin);
            // An arena here is many planes with holes, none of which need cover kPicture whole lines on its own (one
            // whose floor alone does shows fewer of them while the screen bobs): the lines that go on past the
            // arcade's screen (mk5wide::Layer::wider) are counted over all the build's layers.
            int narrow = 0;
            for (const auto& r : band.rows) narrow += r.left > mk5wide::kWideMargin - Layer::kSliver && r.right < mk5wide::kWideMargin + mk5wide::kVisible + Layer::kSliver;
            wide_lines_ += std::max(int(band.full) - narrow, 0);
            layer.Measured(slot_, camera_, std::move(band));
            layer.wider |= wide_lines_ >= Layer::kPicture;
            if (backdrop) {
                for (size_t k = 0; k < draws.size(); ++k) {
                    const uint8_t past = uint8_t((meant[k].first < 0 ? 1 : 0) | (meant[k].second > kCanvas ? 2 : 0));
                    const auto [first, last] = mk5wide::Rows(draws[k]);
                    if (past) for (int y = std::max(first, 0); y < std::min(last, 512); ++y) extent_.reach[size_t(y)] |= past;
                }
                // Every line of a far plane is its art's end where it stops (a wall with arches, a ragged end, a statue
                // at the arena's end), not only the lines it covers whole: the plane is held at them all
                // (mk5wide::Layer::Learn; only an end the arcade shows within its last columns at its limit counts).
                mk5wide::Band holed;
                for (uint16_t y = 0; y < 512; ++y) if (extent_.hi[y] > extent_.lo[y]) {
                    const int16_t left = int16_t(((extent_.reach[y] & 1) ? 0 : extent_.lo[y]) - kOrigin), right = int16_t(((extent_.reach[y] & 2) ? kCanvas : extent_.hi[y]) - kOrigin);
                    if (left > -kOrigin || right < kCanvas - kOrigin) holed.rows.push_back({y, left, right});
                }
                layer.Learn(unsigned(slot_), camera_, holed);
            }
        }
        const uint32_t before = layer.serial;
        for (const auto& e : plane_) {
            if (refused[size_t(&e - plane_.data())]) continue;
            Draw d; d.entry = e.entry; d.fixed = e.fixed; d.fresh = e.fresh; d.layer = e.fixed ? int16_t(-1) : int16_t(list_);
            Place(d);
        }
        if (backdrop && !draws.empty()) SoftBackdrop(before);
        plane_.clear();
        return true;
    }
    // A far backdrop narrower than the wide screen (a skyline, a sky: a picture the size of the arcade's screen that
    // hardly scrolls): where its lines end on one straight edge in a margin, the margin past it has no art of the
    // plane. Those lines go on soft: each margin pixel past the edge is marked kSoft, in the plane's own place in the
    // draw order (anything nearer is drawn over it), holding the line's last four columns' pens; the scanout shows them
    // as their average colour, blurred over the lines around and dimmed towards the screen's edge
    // (Scanout::PresentWide). Nothing of the picture is copied: no object can appear twice.
    // A line counts when the plane's own pixels cover three quarters of the arcade's columns or more (a backdrop's
    // line, not an object's) and none of its draws reaches past the measured canvas on that side; a side counts with
    // kPicture such lines or more, 9 in 10 of them on a straight edge: a column, give or take one, where kPicture
    // lines or more end (a backdrop made of two pictures, one above the other, has two).
    // before: the layer's serial before the plane's draws (a later one: the plane's own pixel).
    void SoftBackdrop(uint32_t before) {
        constexpr int kOrigin = mk5wide::kReach, W = mk5wide::kLayerWidth, kFirst = mk5wide::kWideMargin, kLast = mk5wide::kWideMargin + mk5wide::kVisible - 1;
        const size_t page = size_t(layer.page) * 256;
        for (int side = 0; side < 2; ++side) {
            const auto edge = [&](size_t y) -> int {   // the line's outermost own pixel (layer column) when it ends in the margin; -1: none
                if (extent_.cover[y] < mk5wide::kVisible * 3 / 4 || (extent_.reach[y] & (side ? 2 : 1))) return -1;
                const int e = side ? extent_.hi[y] - 1 - kOrigin : extent_.lo[y] - kOrigin;
                return (side ? e >= kLast - mk5wide::kSoftSkip && e < W - 1 : e > 0 && e <= kFirst + mk5wide::kSoftSkip) ? e : -1;
            };
            std::array<int, mk5wide::kLayerWidth + 2> ends{};
            int lines = 0, straight_lines = 0;
            for (size_t y = page; y < page + 256; ++y) if (const int e = edge(y); e >= 0) { ++lines; ++ends[size_t(e)]; }
            const auto run = [&](int c) { return c > 0 && c < W - 1 ? ends[size_t(c - 1)] + ends[size_t(c)] + ends[size_t(c + 1)] : 0; };
            const auto straight = [&](int e) { return std::max({run(e - 1), run(e), run(e + 1)}) >= Layer::kPicture; };
            for (size_t y = page; y < page + 256; ++y) if (const int e = edge(y); e >= 0) straight_lines += straight(e);
            if (lines < Layer::kPicture || straight_lines * 10 < lines * 9) continue;
            layer.Draw(mk5wide::Kind::continued, side ? kLast + 1 : 0, side ? W : kFirst);
            for (size_t y = page; y < page + 256; ++y) {
                const int e = edge(y);
                const size_t row = y * size_t(W);
                if (e < 0 || !straight(e) || layer.written[row + size_t(e)] <= before) continue;
                for (int L = side ? std::max(e + 1, kLast + 1) : std::min(e - 1, kFirst - 1), k = 0; L >= 0 && L < W; L += side ? 1 : -1, ++k) {
                    int from = side ? e - (k & 3) : e + (k & 3);
                    if (layer.written[row + size_t(from)] <= before) from = e;
                    const size_t to = row + size_t(L);
                    layer.pens[to] = layer.pens[row + size_t(from)]; layer.fixed[to] = kSoft;
                    if (!layer.blood.empty()) layer.blood[to] = 0;
                    layer.Stamp(to);
                }
            }
        }
    }
    void Place(const Draw& d) { if (shadow_post) pending.push_back(d); else DrawWide(d); }
    void Flush() {
        for (const auto& d : pending) DrawWide(d);
        pending.clear(); shadow_post = false;
    }
    void DrawWide(const Draw& d) {
        if (d.stains) { Stains(uint32_t(d.entry[0]) | uint32_t(d.entry[1]) << 16); return; }
        const auto r = LayerRegisters(d.entry);
        const auto [lo, hi] = mk5wide::Span(d.entry[4], d.entry[6], d.entry[10], (d.entry[1] & 0x10) != 0);
        layer.Draw(mk5wide::Kind::shadow, lo + mk5wide::kExtra, hi + mk5wide::kExtra, d.layer);
        if (layer.debug) {
            const auto [first, last] = mk5wide::Rows(r);
            layer.placed.push_back({int16_t(lo + mk5wide::kExtra), int16_t(hi + mk5wide::kExtra), uint16_t(first), uint16_t(std::max(last - first, 0)),
                                    uint32_t(d.entry[2] | uint32_t(d.entry[3]) << 16), d.entry[1], d.fixed, d.layer});
        }
        // mk5wide::Layer::Write through plain pointers (the blitter's stores cannot move the layer).
        uint16_t* const pens = layer.pens.data();
        uint8_t* const marks = layer.fixed.data();
        uint16_t* const stains = layer.blood.empty() ? nullptr : layer.blood.data();
        const size_t size = layer.pens.size();
        const uint16_t booked = layer.booked[(r[Dma::PALETTE] >> 8) & 0x7f] ? Layer::kBooked : 0;
        const uint8_t mark = uint8_t((d.fixed ? Layer::kFixed : 0) | (Layer::ActorDraw(r[Dma::COMMAND]) ? Layer::kActor : 0) | (d.fresh ? kFresh : 0));
        Render(r, mk5wide::kLayerWidth, d.layer < 0, [&](size_t i, uint16_t pen) {
            if (i >= size) return;
            pens[i] = uint16_t(pen | booked); marks[i] = mark;
            if (stains) stains[i] = 0;
            layer.Stamp(i);
        });
    }
    // The floor stains over the wide window, at the shadow's world camera (y<<16|x): the stains' own state (the pens
    // they first cover) stays the arcade's (FloorBlood::Draw).
    void Stains(uint32_t camera) {
        if (!blood || layer.blood.empty()) return;
        const uint32_t page = Long(wide::kPageOrigin);
        const int page_x = uint16_t(page), page_y = int(page >> 16);
        if (page_y != 0 && page_y != 256) return;
        std::fill_n(layer.blood.begin() + ptrdiff_t(page_y) * mk5wide::kLayerWidth, size_t(256) * mk5wide::kLayerWidth, uint16_t(0));
        if (!Word(FloorBlood::kArenaShown)) return;
        const int camera_x = int16_t(camera), camera_y = int16_t(camera >> 16);
        for (const auto& [pose, stain] : blood->stains) {
            const auto [origin_x, origin_y, flip] = pose;
            for (int dy = 0; dy < FloorBlood::kHeight; ++dy) {
                const int y = origin_y - camera_y + dy;
                if (y < 0 || y >= 254) continue;
                for (int dx = 0; dx < FloorBlood::kWidth; ++dx) {
                    const int column = page_x + origin_x - camera_x + dx + mk5wide::kExtra;
                    const size_t k = size_t(dy) * FloorBlood::kWidth + size_t(flip ? FloorBlood::kWidth - 1 - dx : dx);
                    if (!blood->shape[k] || column < 0 || column >= mk5wide::kLayerWidth) continue;
                    layer.blood[size_t(page_y + y) * mk5wide::kLayerWidth + size_t(column)] = uint16_t(stain.colors[blood->shape[k]] | 0x8000);
                }
            }
        }
    }
};

} // namespace mk3
