// Read-only observation of mk3::Dma draws for the Remastered presentation (plan 2026-11-10, "Imagem e Remasterizada",
// layer 2): which blit wrote each video RAM word, from which graphics ROM bit offset, palette and clip window. The
// observer never changes the canonical video RAM, registers or timing; Dma calls it only when attached.
// CPU video RAM stores (midtunit_vram_w, shift register) do not pass through Dma: the host forwards Ops::vram_observer
// (mk3::CpuVramObserver, mk3_cpu.hpp) to DmaLayers::Cpu.
#pragma once
#include "../include/mk5_fighter_video.hpp"  // relative: core test builds pass only -I native/mk3
#include "mk3_blood.hpp"
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mk3 {

struct DmaDraw {               // dma_w state at the trigger (V:726-800), after skipdma is excluded
    uint64_t serial, cycle;    // 1-based draw number; the caller's cycle argument of Dma::Write
    uint16_t command;
    uint32_t source;           // graphics region bit offset (after the 0xf8000000 fold); 0 for mode 0xc
    int x, y, width, height;   // XSTART/YSTART masked, WIDTH/HEIGHT & 0x3ff
    uint16_t palette, color;   // PALETTE & 0x7f00, palette | COLOR & 0xff
    int left, right, top, bottom;
    uint16_t xstep, ystep;     // 0x100 = unscaled
};

struct DmaObserver {
    virtual ~DmaObserver() = default;
    virtual void Begin(const DmaDraw&) {}
    virtual void Pixel(size_t /*vram_index*/, uint16_t /*value*/) {}   // every video RAM word the draw stores
};

// Last DMA writer of every video RAM word (0 = none since the last Cpu() or Clear()).
struct DmaOwners : DmaObserver {
    std::vector<DmaDraw> draws;
    std::vector<uint32_t> owner = std::vector<uint32_t>(0x80000);
    void Begin(const DmaDraw& d) override { draws.push_back(d); }
    void Pixel(size_t index, uint16_t) override { owner[index] = uint32_t(draws.size()); }
    void Cpu(size_t index) { owner[index] = 0; }
    const DmaDraw* Owner(size_t index) const { return owner[index] ? &draws[owner[index] - 1] : nullptr; }
};

// Producer of a draw (read-only, per TMS instruction from the host's Board::stop): the program's DMA queue (enqueue,
// display pass, object walker, table routines, direct list) at the addresses of DmaQueue; any other DMA_COMMAND
// writer draws directly. (the prose that described the program's routines and screens behind these addresses was removed for publication).
struct DmaSource {
    uint32_t producer = 0;      // enqueue PC (queued) or the direct DMA_COMMAND writer
    uint32_t list = 0, scroll = 0;   // object walker: list head and the table's scroll word
    uint32_t routine = 0;       // table routine being called, kAfterTable, or 0
    uint32_t object = 0;        // object walker: the object (A0 at FF804410; its parts share it)
    uint32_t appender = 0;      // direct list (FF8052B0): the return PC of the FF8081A0 caller; 0 = not observed
    uint8_t bound = 0;          // object walker: the object's ObjectBinder class (DmaClass) at the enqueue; 0 = none
};
inline uint32_t RamLong(const std::vector<uint16_t>& ram, uint32_t a) {   // main RAM long at bit address a; 0 outside
    const size_t i = (a - 0x01000000) / 16;
    return a >= 0x01000000 && i + 1 < ram.size() ? uint32_t(ram[i]) | uint32_t(ram[i + 1]) << 16 : 0;
}
struct DmaQueue {
    static constexpr uint32_t kPass = 0xff804f10, kScroll = 0xff805000, kWalk = 0xff805060, kWalkDone = 0xff805080,
        kCall = 0xff8050a0, kCallDone = 0xff8050b0, kAfterTable = 0xff8050c0, kObject = 0xff804410, kPump = 0xff804640, kReset = 0xff806840;
    static constexpr uint32_t kAppend = 0xff8081a0, kDirectPop = 0xff8051d0, kDirect = 0xff8052b0;
    static constexpr uint32_t kUnstaged = 0xffffffff;   // popped entry without an observed enqueue
    uint32_t list = 0, scroll = 0, routine = 0, direct = 0;
    std::unordered_map<uint32_t, DmaSource> staged;
    std::unordered_map<uint32_t, uint32_t> appended;   // direct list entry -> appender's caller return
    void Step(uint32_t pc, const uint32_t* a, const std::vector<uint16_t>* ram = nullptr) {
        if (pc == kAppend && ram) appended[a[13] - 0xa0] = RamLong(*ram, a[15] + 0x60);
        if (pc == kDirectPop) direct = a[1];
        switch (pc) {
        case kPass: routine = 0; break;
        case kScroll: scroll = a[13]; break;
        case kWalk: list = a[0]; break;
        case kWalkDone: list = scroll = 0; break;
        case kCall: routine = a[2]; break;
        case kCallDone: routine = 0; break;
        case kAfterTable: routine = kAfterTable; break;
        case kReset: staged.clear(); break;   // (instruction detail removed)
        case kObject: case 0xff800470: case 0xff805400:
            staged[a[1] - 0xc0] = pc == kObject ? DmaSource{pc, list, scroll, routine, a[0]} : DmaSource{pc, 0, 0, routine};
            break;
        }
    }
    DmaSource Take(uint32_t writer, uint32_t b14) {
        if (writer == kDirect) {
            const auto it = appended.find(direct);
            if (it == appended.end()) return {writer, 0, 0, routine};
            const DmaSource s{writer, 0, 0, routine, 0, it->second};
            appended.erase(it);
            return s;
        }
        if (writer != kPump) return {writer, 0, 0, routine};
        const auto it = staged.find(b14);
        if (it == staged.end()) return {kPump, kUnstaged, 0, 0};
        const DmaSource s = it->second;
        staged.erase(it);
        return s;
    }
};

// Remastered layer 2: the class of each draw from its producer (DmaQueue): Scenery, Fighter (the objects held in
// the two fighter slots), World (any other object), Text, Other, Clear (colour fills). Producers without a rule stay
// None ("sem classe"). Evidence: the display tables of the program ROM and the per-producer census of the routes
// (mk3-preview --dma-census). (the prose that described the program's routines and screens behind these addresses was removed for publication).
inline constexpr uint32_t kFighterSlots[2] = {0x010609b0, 0x01061560};
enum class DmaClass : uint8_t { None, Scenery, Fighter, Text, Clear, Other, World, Cpu };
inline constexpr const char* kDmaClassNames[] = {"sem classe", "cenario", "lutador", "texto/HUD", "limpeza", "outros", "objetos", "cpu"};
inline bool FighterObject(uint32_t object, const std::vector<uint16_t>* ram) {
    return ram && object && (object == RamLong(*ram, kFighterSlots[0]) || object == RamLong(*ram, kFighterSlots[1]));
}
// Objects of the world list classed by their creator (read-only, per TMS instruction): every insertion runs kInsert,
// so the creator's return PC is the long at SP + 0A0h. A site of kObjectSites binds the object; any other insertion
// unbinds it, and so does its return to the free list (stores kFreeHeadStores). Sites were found by read-only probes
// of the route receipts (K5-K10). The bound class is the one at the enqueue (DmaSource::bound).
inline constexpr uint8_t kAnyParent = 0xff, kEffectParent = 0xfe, kStackParent = 0x80;   // 0x80 | k: the long at SP + 20h * k
struct ObjectSite { uint32_t ret; uint8_t parent; DmaClass cls; };   // parent: A register of the fighter (Fighter only)
inline constexpr ObjectSite kObjectSites[] = {
    {0xff963bd0, 8, DmaClass::Fighter}, {0xff999c70, 8, DmaClass::Fighter}, {0xff9a1220, 8, DmaClass::Fighter},
    {0xffa0eba0, 8, DmaClass::Fighter}, {0xffa108a0, 8, DmaClass::Fighter}, {0xffa132d0, 8, DmaClass::Fighter},
    {0xffa16610, 8, DmaClass::Fighter}, {0xffa16810, 8, DmaClass::Fighter}, {0xffa17930, 8, DmaClass::Fighter},
    {0xffa1ab80, 8, DmaClass::Fighter}, {0xffa219a0, 8, DmaClass::Fighter},
    {0xffa690c0, 11, DmaClass::Fighter}, {0xffa69e60, 11, DmaClass::Fighter}, {0xffa696b0, 11, DmaClass::Fighter},
    {0xffa9e7a0, 9, DmaClass::Fighter}, {0xffa955a0, 10, DmaClass::Fighter}, {0xffa95190, 10, DmaClass::Fighter},
    {0xffa9d8f0, 10, DmaClass::Fighter}, {0xffa685d0, 11, DmaClass::Fighter}, {0xffa68ab0, 11, DmaClass::Fighter},
    {0xffaa0ba0, 11, DmaClass::Fighter}, {0xffa8f7d0, 10, DmaClass::Fighter}, {0xffa9f190, 10, DmaClass::Fighter},
    {0xffa67db0, 11, DmaClass::Fighter}, {0xffa680f0, 11, DmaClass::Fighter}, {0xff99ef70, 10, DmaClass::Fighter},
    {0xff99ff50, 10, DmaClass::Fighter}, {0xff9a09a0, 11, DmaClass::Fighter}, {0xffa9a920, 10, DmaClass::Fighter},
    {0xffa2a4e0, 10, DmaClass::Fighter}, {0xffa9b770, 10, DmaClass::Fighter}, {0xffa9b8a0, 10, DmaClass::Fighter},
    {0xff9e0440, 10, DmaClass::Fighter}, {0xff9e0980, 10, DmaClass::Fighter},
    {0xff93f680, kAnyParent, DmaClass::Fighter}, {0xff944de0, kAnyParent, DmaClass::Fighter},
    {0xff9e8d10, 0, DmaClass::Scenery}, {0xff9e91d0, 0, DmaClass::Scenery},
    {0xffa019c0, 0, DmaClass::Scenery}, {0xffa01b30, 0, DmaClass::Scenery}, {0xffa01c50, 0, DmaClass::Scenery},
    {0xffa01cc0, 0, DmaClass::Scenery}, {0xffa02240, 0, DmaClass::Scenery}, {0xffa02c80, 0, DmaClass::Scenery},
    {0xffa073f0, 0, DmaClass::Scenery}, {0xffa07550, 0, DmaClass::Scenery}, {0xffa07730, 0, DmaClass::Scenery},
    {0xff9fc940, 0, DmaClass::Scenery}, {0xff93e390, 0, DmaClass::Scenery}, {0xff93e4b0, 0, DmaClass::Scenery},
    {0xffa41ca0, 0, DmaClass::Scenery},
    {0xffa99ad0, 0, DmaClass::Text}, {0xffa960c0, 0, DmaClass::Text}, {0xffa96100, 0, DmaClass::Text},
    {0xffa96160, 0, DmaClass::Text}, {0xff9ec7e0, 0, DmaClass::Text}, {0xff940010, 0, DmaClass::Text},
    {0xff940130, 0, DmaClass::Text}, {0xffa94120, 0, DmaClass::Text}, {0xff9f2d20, 0, DmaClass::Text},
    {0xffac0030, 0, DmaClass::Text},
    {0xffa9bff0, 10, DmaClass::Fighter}, {0xffa19260, 10, DmaClass::Fighter}, {0xffaa21c0, 10, DmaClass::Fighter}, {0xffa13ff0, 11, DmaClass::Fighter},
    {0xffa1ba60, kStackParent | 7, DmaClass::Fighter},
    {0xffa9cb70, kEffectParent, DmaClass::Fighter}, {0xffa9c950, kEffectParent, DmaClass::Fighter},
    {0xffaa3790, 0, DmaClass::Text}, {0xffa8e020, 0, DmaClass::Text}, {0xffa921d0, 0, DmaClass::Text}, {0xffac0450, 0, DmaClass::Text},
    {0xff90d5a0, 0, DmaClass::Scenery}, {0xff90d6b0, 0, DmaClass::Scenery}, {0xff90d830, 0, DmaClass::Scenery},
    {0xffaa2df0, 11, DmaClass::Fighter}, {0xff9a4990, kStackParent | 6, DmaClass::Fighter}, {0xffa93be0, 0, DmaClass::Text},
    {0xff912820, 0, DmaClass::Scenery}, {0xffa8e8e0, 0, DmaClass::Text}};
// More object sites (kChain, kPrinterAllocs): chained screen art, banners and printer glyph objects, bound as above.
struct ObjectBinder {
    static constexpr uint32_t kInsert = 0xff8071d0, kWorldList = 0x0105dc10, kFreeHead = 0x010608c0, kChain = 0xff9fca00;
    static constexpr uint32_t kAlloc = 0xff806eb0, kPrinterAllocs[] = {0xffab8fc0, 0xffab9600};
    static constexpr uint32_t kFreeHeadStores[] = {0xff8066d0, 0xff806ec0, 0xff807540, 0xff807730, 0xff90ddd0, 0xff925220,
                                                   0xff95f4a0, 0xff960240, 0xff960730};
    static constexpr uint32_t kEffect = 0xffa8c3d0, kProcessMade = 0xff9270c0, kTextList = 0x0105dc30, kTextMove = 0xff960080, kTextFront = 0xff9443d0;
    std::unordered_map<uint32_t, DmaClass> bound;
    std::unordered_map<uint32_t, bool> effect_parent;   // effect process (FFA8C3D0 -> FF926F20, A0) -> the caller's A8 was a fighter
    bool freed = false, effect = false, pending = false;
    bool Fighter(uint32_t parent, const std::vector<uint16_t>& ram) const {   // a fighter slot object or a bound child
        const auto it = bound.find(parent);
        return FighterObject(parent, &ram) || (it != bound.end() && it->second == DmaClass::Fighter);
    }
    void Step(uint32_t pc, const uint32_t* a, const std::vector<uint16_t>& ram) {
        if (freed) {   // after the store: unbind the free list from its new head
            freed = false;
            for (uint32_t o = RamLong(ram, kFreeHead), n = 0; o && n < 4096 && !bound.empty(); o = RamLong(ram, o), ++n) bound.erase(o);
        }
        if (pc == kInsert && a[4] == kWorldList) {
            const uint32_t ret = RamLong(ram, a[15] + 0xa0);
            DmaClass c = DmaClass::World;
            for (const auto& s : kObjectSites) {
                if (s.ret != ret) continue;
                if (s.parent == kEffectParent) {   // the parent's class at the call: it can be freed, even reused for this object
                    const auto it = effect_parent.find(a[13]);
                    c = it != effect_parent.end() && it->second ? s.cls : DmaClass::World;
                    continue;
                }
                const uint32_t parent = s.parent == kAnyParent ? 0 : s.parent & kStackParent ? RamLong(ram, a[15] + 0x20 * (s.parent & 0x0f)) : a[s.parent];
                c = s.cls != DmaClass::Fighter || s.parent == kAnyParent || (parent && parent != a[0] && Fighter(parent, ram)) ? s.cls : DmaClass::World;
            }
            if (c == DmaClass::World) bound.erase(a[0]); else bound[a[0]] = c;
        }
        if (pc == kAlloc) {
            const uint32_t ret = RamLong(ram, a[15] + 0x40);
            if (ret == kPrinterAllocs[0] || ret == kPrinterAllocs[1]) bound[a[0]] = DmaClass::Text; else bound.erase(a[0]);
        }
        if ((pc == kTextMove && a[0] == kTextList && a[1] == kWorldList) || pc == kTextFront)   // the text list joins 0105DC10
            for (uint32_t o = RamLong(ram, kTextList), n = 0; o && n < 4096; o = RamLong(ram, o), ++n) bound[o] = DmaClass::Text;
        if (pc == kEffect) { effect = true; pending = Fighter(a[8], ram); }   // JA FF926F20: the next process made is the effect's
        if (pc == kProcessMade && effect) { effect = false; if (a[0]) effect_parent[a[0]] = pending; }
        if (pc == kChain) {   // A8 linked after A9
            if (const auto it = bound.find(a[9]); it != bound.end()) bound[a[8]] = it->second; else bound.erase(a[8]);
        }
        for (const uint32_t s : kFreeHeadStores) freed |= pc == s;
    }
};
// Diagnostics: the operator DMA self-test draw (kDmaSelfTest) compares a graphics ROM row: opaque, nearest, Clear.
inline constexpr uint32_t kDmaSelfTest = 0xffaefa50;
inline DmaClass ClassifyDma(const DmaSource& s, const DmaDraw& d, const std::vector<uint16_t>* ram = nullptr) {
    if (s.producer == DmaQueue::kObject) {
        if (s.scroll >= 0x0105d9e0 && s.scroll <= 0x0105dac0 && !(s.scroll & 0x1f)) return DmaClass::Scenery;
        if (s.scroll == 0x0105dae0 && s.list == 0x0105dc10) return FighterObject(s.object, ram) ? DmaClass::Fighter : DmaClass::World;
        if (s.scroll == 0x0105dae0 && s.list == 0x0105dc30) return DmaClass::Text;
        if (s.scroll == 0x0105dae0 && s.list >= 0x0105db30 && s.list <= 0x0105dbf0 && (s.list & 0x1f) == 0x10) return DmaClass::Scenery;
        if (s.list >= 0x01062730 && s.list <= 0x01062810 && (s.list & 0x1f) == 0x10 && s.scroll == s.list + 0x150) return DmaClass::Scenery;
        return DmaClass::None;
    }
    if (s.producer == kDmaSelfTest && d.command == 0x8003) return DmaClass::Clear;
    if (s.routine == 0xff800000 && s.producer == 0xff800470) return DmaClass::Scenery;
    if (s.routine == DmaQueue::kAfterTable) {
        if (s.producer == 0xff805400) return DmaClass::Text;
        if (s.producer == 0xff801b10 || s.producer == 0xff801b80) return DmaClass::Other;
        if (s.producer == DmaQueue::kDirect && (d.command & 0x0f) == 0x0c) return DmaClass::Clear;
        if (s.producer == DmaQueue::kDirect && (s.appender == 0xffab9b90 || s.appender == 0xffab9c00)) return DmaClass::Text;
    }
    return DmaClass::None;
}

// Per video RAM word: the value its last writer stored and the scenery pen under it. A Scenery draw sets the pen and the
// kScenery flag; a Clear draw or a CPU store replaces the pen (its own pixel); any other draw keeps the pen as the hidden
// scenery and drops the flag. Backdrop(): the pen the Remastered filter reads and whether the visible word is still the
// scenery draw's value. Cpu() is the host's forward of Ops::vram_observer; without it a word whose value differs from what
// its last DMA stored is taken as CPU-written (its own pen, unflagged).
// Fighters (as MK2, mk5_fighter_video.hpp): every draw also goes into `fighters`; a Fighter draw is a layer texel owned by
// its object, any other draw (and a CPU store) an opaque base pen, bit 15 = scenery. The scanout samples it per row.
// Floor blood (mk3_blood.hpp, enabled by EnableFloorBlood): composed when the pump pops the world pass's first entry;
// every later DMA or CPU store covers it, and a fighter texel over a stain keeps it as its base.
struct DmaLayers : DmaObserver {
    static constexpr uint32_t kScenery = 0x10000;
    const uint32_t* writer_pc = nullptr;   // Ops::instruction_pc
    const uint32_t* b14 = nullptr;         // ResetTrace::b[14], the queue cursor
    const std::vector<uint16_t>* ram = nullptr;   // Ops::ram (fighter slots); null: no Fighter class
    const std::vector<uint16_t>* palette = nullptr;   // Ops::palette: the colours of a kept stain
    FloorBlood blood;
    DmaQueue queue;
    ObjectBinder objects;
    std::vector<uint16_t> stored = std::vector<uint16_t>(0x80000), under = std::vector<uint16_t>(0x80000);
    std::vector<uint8_t> cls = std::vector<uint8_t>(0x80000);   // DmaClass of the last writer
    mk5presentation::FighterSurface fighters{0x80000};
    uint64_t draws[8] = {}, pixels[8] = {}, cpu_words[3] = {}, bound_draws[3] = {};   // cpu_words: Cpu, page fill, picture; bound_draws: child, screen art, text
    DmaClass now = DmaClass::None;
    mk5presentation::FighterSurface::DrawToken token{0};
    size_t cpu_begin = 0, cpu_end = 0;     // pending run of CPU-stored words, not yet in `fighters`
    // The binding is read at the enqueue (FF804410): the DMA-done handler pops the entry later, while the processes run on,
    // and an object can be freed (FF807410: unlinked, pushed on the free list) or reallocated before its last draw is popped.
    void Step(uint32_t pc, const uint32_t* a) {   // the host calls it before every TMS instruction
        queue.Step(pc, a, ram);
        if (ram) objects.Step(pc, a, *ram);
        if (ram) blood.Step(pc, a, *ram, palette);
        if (pc == DmaQueue::kObject)
            if (const auto it = objects.bound.find(a[0]); it != objects.bound.end()) queue.staged[a[1] - 0xc0].bound = uint8_t(it->second);
    }
    void Begin(const DmaDraw& d) override {
        if (*writer_pc == DmaQueue::kPump) blood.Pump(*b14, stored, palette);
        Flush();
        const DmaSource src = queue.Take(*writer_pc, *b14);
        now = ClassifyDma(src, d, ram);
        if (now == DmaClass::World && src.bound) { now = DmaClass(src.bound); ++bound_draws[now == DmaClass::Fighter ? 0 : now == DmaClass::Scenery ? 1 : 2]; }
        ++draws[unsigned(now)];
        token = fighters.Draw(now == DmaClass::Fighter ? src.object : 0);
    }
    void Pixel(size_t i, uint16_t v) override {
        stored[i] = v; ++pixels[unsigned(now)];
        cls[i] = uint8_t(now);
        if (now == DmaClass::Scenery || now == DmaClass::Clear) under[i] = v;
        const uint16_t stain = blood.At(i);
        blood.Mask(i);
        fighters.Write(token, i, uint16_t((v & 0x7fff) | (now == DmaClass::Scenery ? 0x8000 : 0)),
                       now == DmaClass::Fighter && stain ? FloorBlood::kMark | (stain & 0x7fffu) : 0);
    }
    // CPU stores: the page fill (kPageFill) and the CPU-decoded pictures (kPictureRows) are Scenery; any other CPU
    // store: Cpu (nearest).
    static constexpr uint32_t kPageFill = 0xff81b580, kPictureRows[3] = {0xff92a8b0, 0xff92a8d0, 0xff92a8f0};
    void Cpu(uint32_t pc, size_t i, uint16_t v) {
        const bool picture = pc == kPictureRows[0] || pc == kPictureRows[1] || pc == kPictureRows[2];
        const DmaClass c = pc == kPageFill || picture ? DmaClass::Scenery : DmaClass::Cpu;
        stored[i] = under[i] = v; cls[i] = uint8_t(c); ++cpu_words[picture ? 2 : c == DmaClass::Scenery];
        blood.Mask(i);
        if (i != cpu_end) { Flush(); cpu_begin = i; }
        cpu_end = i + 1;
    }
    // n words stored in a row by one instruction, as n Cpu calls: a run that does not follow the pending one is flushed
    // with its first word stored and no more, as Cpu flushes it.
    void CpuRun(uint32_t pc, size_t i, size_t n, const uint16_t* v) {
        if (!n) return;
        const bool picture = pc == kPictureRows[0] || pc == kPictureRows[1] || pc == kPictureRows[2];
        const DmaClass c = pc == kPageFill || picture ? DmaClass::Scenery : DmaClass::Cpu;
        if (i != cpu_end) { stored[i] = under[i] = v[0]; cls[i] = uint8_t(c); Flush(); cpu_begin = i; }
        std::copy_n(v, n, stored.begin() + ptrdiff_t(i)); std::copy_n(v, n, under.begin() + ptrdiff_t(i));
        std::fill_n(cls.begin() + ptrdiff_t(i), n, uint8_t(c)); cpu_words[picture ? 2 : c == DmaClass::Scenery] += n;
        blood.Mask(i, n);
        cpu_end = i + n;
    }
    void Flush() {   // CPU stores become opaque base pens (one ClearWith per contiguous run)
        if (cpu_end == cpu_begin) return;
        fighters.ClearWith(cpu_begin, cpu_end - cpu_begin, [this](size_t k) {
            const size_t i = cpu_begin + k;
            return uint16_t((stored[i] & 0x7fff) | (cls[i] == uint8_t(DmaClass::Scenery) ? 0x8000 : 0));
        });
        cpu_begin = cpu_end = 0;
    }
    // Bits 17-19: the writer's DmaClass (7 = CPU; also a value differing from the last DMA's without the hook).
    uint32_t Backdrop(size_t i, uint16_t value) const {
        if (stored[i] != value) return (value & 0x7fffu) | 7u << 17;
        return under[i] | (cls[i] == uint8_t(DmaClass::Scenery) ? kScenery : 0) | uint32_t(cls[i]) << 17;
    }
};

} // namespace mk3
