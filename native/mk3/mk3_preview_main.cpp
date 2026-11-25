// MK3 2.1 native (plan 2026-11-10, J2; product 2026-11-14): mk3::Board from reset in Profile::Game, with the Wolf-Unit scanout
// (mk3_scanout.hpp) presented through the shared MK1/MK2 host (Mk5Window: DX12, video profiles, CRT/bezel/ScaleFX
// presets from the same MK1_* settings), the DCS 8K PCM through the shared DCS resampler and Windows PCM queue, and
// the keyboard/XInput word mapped to the Wolf-Unit ports (mk3_host.hpp). Translated but not yet compared code runs
// and is logged in mk3::FirstExecution; untranslated code stops with its PC, shown and printed.
// Fidelity is claimed only for the compared routes (docs/MK3_DEVELOPMENT.md).
// argv: ROM_DIRECTORY CMOS_SEED GRAPHICS_LIST [--frames N] [--window] [--audio] [--strict] [--hashes] [--dump F]
//       [--linger SECONDS] [--tape FILE] [--watch-dcs CMD] [--edition original|remastered] [--cmos-profile DIR]
//       [--dsw-word N]
//   --frames N   stop after N presented frames (default: until the frontier or the window closes)
//   --hashes     one JSON line per presented frame: FNV-1a 64 of the visible RGB (current pens, palette at VBLANK) and
//                of the previous frame's pens through the same palette (what MAME screen_device::pixels() returns in a
//                frame notifier, after video_output_update swapped m_curbitmap), and of the palette RAM
//   --dump F[,F]  the visible RGB of each frame F (both views) as hex; Remastered also the backdrop, the presented frame
//                (with the floor blood of mk3_blood.hpp) and the 4x frame
//   --linger S   with --window, keep the last frame (and the stop message) on screen S seconds (default: until Esc)
//   --tape F     legal input tape ("cycle port value" rows, tools/mk3/replay.py format; port 0 IN0, 1 IN1, 2 DSW, 3 IN2):
//                from the first TMS instruction boundary >= cycle (and >= 1000, not a PIXBLT restart re-entry, the rule
//                of mk3_replay_main.cpp) the port reads value, through the same Ops::ports the live inputs write.
//                With a tape the live inputs are not read.
//   --watch-dcs C  report the TMS cycle of the low-byte write of the first DCS command C (sound-port byte pairs)
//   --edition E  Original or Remastered (layer 1: the caller's MK1_* video settings; layer 2: the DMA classes of
//                mk3_dma_observer.hpp, with the CPU's video RAM stores (Ops::vram_observer), give the scenery backdrop
//                and the fighter layers, which the window composes 4x bicubic as MK2 (mk5_stage_bicubic.hpp,
//                mk5_fighter_bicubic.hpp); presentation only: the canonical state, scanout and hashes are the same.
//                Headless Remastered with --hashes also prints the scenery pixel count, the fighter layer count, the
//                kept floor stains and the hash of the 4x frame.
//   --wide       Remastered: the 16:9 widescreen frame (mk3_wide.hpp; also MK3_WIDESCREEN=1 with --window). --hashes then
//                adds the wide frame's diagnostics to each frame line and --dump the wide frame (raw and presented)
//   --dump-4x    with --wide and --dump: each dumped frame also has the wide frame as the window composes it (4x: the
//                scenery bicubic, the fighter layers) before its filters
//   --state-hash with --hashes: each frame line also has the hash of main RAM, video RAM and the CPU registers
//   --dma-census per-producer DMA counts (writer/enqueue PC, display list, scroll word, table routine, graphics bank)
//   --state-test PLAN  test plan of save states, e.g. save@100,hash@200,load@200,hash@200: each item once, in order, at
//                the first presented frame >= its number, as F5 (save), F7 (load) or a digest of the state (hash)
//   --cmos-out FILE  write the final NVRAM image to FILE when the run ended at no frontier (setup: factory seed)
//   --dsw-word N  the DSW port (2) from reset, as the cabinet's DIP switches: only the Blood (4000h) and Violence (8000h)
//                switches may differ from the INPUT_PORTS(mk3) default FD7Dh (the launcher's operator setting). A switch is
//                outside the machine state, so a loaded save state keeps this word (without a tape, which owns the ports)
//   --cmos-profile DIR  load DIR/cmos.bin (the seed when absent) and save the NVRAM there at exit; without it the
//                seed is used and nothing is written
// mk3 --input-map prints the Wolf-Unit ports for each key and pad control alone (tools/mk3/test_preview.py).
#include <algorithm>   // before the emulation headers, which use it
#include "mk3_board.hpp"
#include "mk3_save_state.hpp"
#include "mk3_dcs.hpp"
#include "mk3_host.hpp"
#include "mk2_audio.hpp"
#include "../audio/host_gain.hpp"
#include "../launcher/launcher_legal.hpp"
#include "mk5_audio_output.hpp"
#include "mk5_window.hpp"
#include "mk5_hitbox.hpp"
#include "mk5_profile_presentation.hpp"
#include "mk5_stage_bicubic.hpp"
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
// Window title: the product and the game as the launcher compatibility list names it (launcher_legal.hpp).
std::wstring GameTitle() { return std::wstring(mklauncher::productName) + L" | " + mklauncher::compatibleGames[2].name + L" | "; }
constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull, kFnvPrime = 0x100000001b3ull;
uint64_t HashRgb(const std::vector<uint32_t>& rgb) {
    uint64_t h = kFnvBasis;
    for (const uint32_t v : rgb) h = (h ^ (v & 0xffffff)) * kFnvPrime;
    return h;
}
uint64_t HashWords(const std::vector<uint16_t>& w) {   // little-endian u32 pairs, as tools/mk3/replay_reference.py
    uint64_t h = kFnvBasis;
    for (size_t i = 0; i + 1 < w.size(); i += 2) h = (h ^ (uint32_t(w[i]) | uint32_t(w[i + 1]) << 16)) * kFnvPrime;
    return h;
}
std::vector<uint8_t> LoadGraphics(const std::filesystem::path& list) {   // ROM_LOAD32_BYTE rows "offset size path" (mk3_replay_main.cpp)
    std::vector<uint8_t> gfx;
    std::ifstream graphics(list);
    std::string path;
    for (size_t offset, size; graphics >> offset >> size && std::getline(graphics >> std::ws, path);) {
        std::ifstream rom(path, std::ios::binary);
        std::vector<char> data(size);
        if (!rom.read(data.data(), std::streamsize(size)) || rom.peek() != std::char_traits<char>::eof()) throw std::runtime_error("bad graphics ROM " + path);
        if (gfx.size() < (offset & ~size_t(3)) + 4 * size) gfx.resize((offset & ~size_t(3)) + 4 * size);
        for (size_t i = 0; i < size; ++i) gfx[offset + 4 * i] = uint8_t(data[i]);
    }
    if (gfx.empty()) throw std::runtime_error("no graphics ROMs");
    return gfx;
}
void Hex(const std::vector<uint32_t>& rgb) {
    for (const uint32_t v : rgb) std::printf("%06x", unsigned(v & 0xffffff));
}
// --dma-census: every DMA by writer PC (the instruction that wrote DMA_COMMAND) and graphics bank (region byte
// offset / 0x400000, the ROM_LOAD32_BYTE groups of gfx.txt); read-only.
struct Census : mk3::DmaObserver {
    const uint32_t* pc = nullptr;
    const uint32_t* b14 = nullptr;
    const uint64_t* frame = nullptr;
    mk3::DmaQueue queue;
    struct Row { uint64_t draws = 0, pixels = 0, first = ~0ull, last = 0; int ymin = 9999, ymax = -1, xmin = 9999, xmax = -1, wmax = 0, hmax = 0; std::set<uint16_t> commands, palettes; std::set<uint32_t> sources; uint64_t scaled = 0; };
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, unsigned>, Row> rows;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> cpu;   // CPU video RAM writes (ResetTrace::writes) by PC: words, frames
    void CpuWrites(const std::vector<mk3::BusWrite>& writes) {
        std::set<uint32_t> seen;
        for (const auto& w : writes) if (w.address_bit <= 0x003ffff0) { ++cpu[w.pc].first; if (seen.insert(w.pc).second) ++cpu[w.pc].second; }
    }
    Row* now = nullptr;
    void Begin(const mk3::DmaDraw& d) override {
        const auto src = queue.Take(*pc, *b14);
        auto& r = rows[{src.producer, src.list, src.scroll, src.routine, (d.source >> 3) / 0x400000}];
        r.xmin = std::min(r.xmin, d.x); r.xmax = std::max(r.xmax, d.x);
        if (r.palettes.size() < 64) r.palettes.insert(d.palette);
        ++r.draws; r.first = std::min(r.first, *frame); r.last = std::max(r.last, *frame);
        r.ymin = std::min(r.ymin, d.y); r.ymax = std::max(r.ymax, d.y); r.wmax = std::max(r.wmax, d.width); r.hmax = std::max(r.hmax, d.height);
        if (r.commands.size() < 16) r.commands.insert(d.command);
        if (r.sources.size() < 100000) r.sources.insert(d.source);
        r.scaled += d.xstep != 0x100 || d.ystep != 0x100;
        now = &r;
    }
    void Pixel(size_t, uint16_t) override { ++now->pixels; }
    void Print() const {
        std::printf("{\"dma_census\":[");
        bool first = true;
        for (const auto& [k, r] : rows) {
            std::printf("%s{\"producer\":\"%08x\",\"list\":\"%08x\",\"scroll\":\"%08x\",\"routine\":\"%08x\",\"bank\":%u,\"x\":[%d,%d],\"palettes\":%zu,\"draws\":%llu,\"pixels\":%llu,\"frames\":[%llu,%llu],\"y\":[%d,%d],\"wmax\":%d,\"hmax\":%d,\"sources\":%zu,\"scaled\":%llu,\"commands\":[",
                        first ? "" : ",", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k), std::get<4>(k), r.xmin, r.xmax, r.palettes.size(), (unsigned long long)r.draws, (unsigned long long)r.pixels, (unsigned long long)r.first,
                        (unsigned long long)r.last, r.ymin, r.ymax, r.wmax, r.hmax, r.sources.size(), (unsigned long long)r.scaled);
            bool f2 = true;
            for (const auto c : r.commands) { std::printf("%s\"%04x\"", f2 ? "" : ",", c); f2 = false; }
            std::printf("]}");
            first = false;
        }
        std::printf("],\"cpu_vram_writes\":[");
        first = true;
        for (const auto& [pc, n] : cpu) { std::printf("%s{\"pc\":\"%08x\",\"words\":%llu,\"frames\":%llu}", first ? "" : ",", pc, (unsigned long long)n.first, (unsigned long long)n.second); first = false; }
        std::printf("]}\n");
    }
};
// Ops::vram_observer -> DmaLayers::Cpu (read-only; the CPU's video RAM stores, mk3_cpu.hpp).
struct CpuTap : mk3::CpuVramObserver {
    mk3::DmaLayers* layers = nullptr;
    void CpuVram(uint32_t pc, size_t word, uint16_t value, uint16_t, uint64_t) override { layers->Cpu(pc, word, value); }
};
// Widescreen diagnostics (mk5wide::Layer::debug), per finished build from its scenery draws (mk5wide::Note):
// parallax: for each scenery layer whose list camera moved and whose centre draws moved by minus that amount, the most
// frequent move of its draws in the margins must be the same (3 draws or more on each side). A margin where a draw
// moved like the centre's is no difference: blood drops fly in the world's own list, and three of a spray can pair by
// chance from one build to the next with the same move, while scenery at the wrong rate leaves no draw at the right one;
// still: with the world's camera still (the last scenery list), every scenery list's camera is still.
struct WideChecks {
    unsigned builds = 0;
    std::vector<mk5wide::Note> previous;
    uint64_t parallax_checked = 0, parallax_differ = 0, still_checked = 0, still_moved = 0;
    void Frame(const mk5wide::Layer<uint16_t>& layer) {
        if (!layer.debug || layer.builds == builds) return;
        builds = layer.builds;
        std::map<std::tuple<uint16_t, uint32_t, uint16_t>, std::array<std::vector<const mk5wide::Note*>, 2>> runs;
        for (const auto& k : previous) runs[{k.layer, k.source, k.row}][0].push_back(&k);
        for (const auto& k : layer.notes_done) runs[{k.layer, k.source, k.row}][1].push_back(&k);
        std::map<uint16_t, std::array<std::map<int, int>, 2>> moves;
        for (auto& [key, two] : runs) {
            if (two[0].size() != two[1].size()) continue;
            for (auto& v : two) std::sort(v.begin(), v.end(), [](const mk5wide::Note* p, const mk5wide::Note* r) { return p->lo < r->lo; });
            for (size_t i = 0; i < two[1].size(); ++i) {
                const auto& k = *two[1][i];
                const bool in_margin = (k.hi <= mk5wide::kWideMargin || k.lo >= mk5wide::kWideMargin + mk5wide::kVisible) && k.hi > 0 && k.lo < mk5wide::kLayerWidth;
                const bool in_centre = k.lo >= mk5wide::kWideMargin && k.hi <= mk5wide::kWideMargin + mk5wide::kVisible;
                if (in_margin || in_centre) ++moves[k.layer][in_margin ? 1 : 0][k.lo - two[0][i]->lo];
            }
        }
        const auto mode_of = [](const std::map<int, int>& c) { int best = 0, most = 0; for (const auto& [m, n] : c) if (n > most) { most = n; best = m; } return std::make_pair(best, most); };
        std::map<uint16_t, int> was, now, camera_move;
        for (const auto& k : previous) was[k.layer] = k.camera;
        for (const auto& k : layer.notes_done) { now[k.layer] = k.camera; if (was.count(k.layer)) camera_move[k.layer] = was[k.layer] - k.camera; }
        for (const auto& [l, both] : moves)
            if (!both[0].empty() && !both[1].empty() && camera_move.count(l) && camera_move[l] && mode_of(both[0]).first == camera_move[l]) {
                const auto c = mode_of(both[0]), g = mode_of(both[1]);
                if (c.second >= 3 && g.second >= 3) { ++parallax_checked; parallax_differ += c.first != g.first && !both[1].count(c.first); }
            }
        if (!now.empty() && !was.empty() && was.rbegin()->first == now.rbegin()->first && camera_move.count(now.rbegin()->first) && !camera_move[now.rbegin()->first]) {
            ++still_checked;
            for (const auto& [l, move] : camera_move) if (move) { ++still_moved; break; }
        }
        previous = layer.notes_done;
    }
};
const char* KindName(mk3::Unverified k) {
    static const char* names[] = {"tms_block", "tms_edge", "adsp_block", "adsp_edge", "dma_path", "irq_path", "device_path"};
    return names[unsigned(k)];
}
}

int wmain(int argc, wchar_t** argv) {   // -municode (generate.FLAGS): wide arguments
    if (argc == 2 && std::wstring(argv[1]) == L"--input-map") { std::printf("{\"input_map\":%s}\n", mk3host::InputMapJson().c_str()); return 0; }
    if (argc < 4) { std::fprintf(stderr, "usage: mk3 ROM_DIRECTORY CMOS_SEED GRAPHICS_LIST [options] | --input-map\n"); return 2; }
    uint64_t max_frames = 0;
    std::set<uint64_t> dump_frames;
    double linger = -1;
    bool window_on = false, audio_on = false, strict = false, hashes = false, remastered = false, census_on = false, run_ahead = false;
    bool wide_on = false, state_hash = false, dump_4x = false;
    int watch_dcs = -1;
    std::wstring tape_file, cmos_dir, cmos_out;
    enum class StateRequest { Save, Load, Hash };
    struct PlannedState { StateRequest request; uint64_t frame; };
    std::vector<PlannedState> state_plan;
    std::optional<uint16_t> dsw_word;
    for (int n = 4; n < argc; ++n) {
        const std::string a = mk3host::Narrow(argv[n]);
        if (a == "--frames" && n + 1 < argc) max_frames = std::stoull(mk3host::Narrow(argv[++n]));
        else if (a == "--dump" && n + 1 < argc) { std::stringstream list(mk3host::Narrow(argv[++n])); for (std::string f; std::getline(list, f, ',');) dump_frames.insert(std::stoull(f)); }
        else if (a == "--linger" && n + 1 < argc) linger = std::stod(mk3host::Narrow(argv[++n]));
        else if (a == "--window") window_on = true;
        else if (a == "--audio") audio_on = true;
        else if (a == "--strict") strict = true;
        else if (a == "--hashes") hashes = true;
        else if (a == "--run-ahead") run_ahead = true;
        else if (a == "--wide") wide_on = true;
        else if (a == "--state-hash") state_hash = true;
        else if (a == "--dump-4x") dump_4x = true;
        else if (a == "--training") mk5hitbox::enabled = true;   // headless: the boxes in each --hashes frame line
        else if (a == "--dma-census") census_on = true;
        else if (a == "--tape" && n + 1 < argc) tape_file = argv[++n];
        else if (a == "--watch-dcs" && n + 1 < argc) watch_dcs = int(std::stoul(mk3host::Narrow(argv[++n]), nullptr, 0) & 0xffff);
        else if (a == "--cmos-profile" && n + 1 < argc) cmos_dir = argv[++n];
        else if (a == "--cmos-out" && n + 1 < argc) cmos_out = argv[++n];
        else if (a == "--dsw-word" && !dsw_word && n + 1 < argc) {
            const std::string raw = mk3host::Narrow(argv[++n]);
            size_t used = 0;
            unsigned long value = 0;
            try { value = std::stoul(raw, &used, 0); } catch (const std::exception&) { used = 0; }
            if (raw.empty() || raw[0] == '-' || used != raw.size() || value > 0xffff || (value & ~0xc000ul) != (0xfd7dul & ~0xc000ul)) {
                std::fprintf(stderr, "--dsw-word accepts only the original blood/violence switches: %s\n", raw.c_str());
                return 2;
            }
            dsw_word = uint16_t(value);
        }
        else if (a == "--state-test" && n + 1 < argc) {
            std::stringstream list(mk3host::Narrow(argv[++n]));
            for (std::string item; std::getline(list, item, ',');) {
                const auto at = item.find('@');
                const std::string kind = item.substr(0, at);
                if (at == std::string::npos || (kind != "save" && kind != "load" && kind != "hash")) { std::fprintf(stderr, "bad --state-test item %s\n", item.c_str()); return 2; }
                state_plan.push_back({kind == "save" ? StateRequest::Save : kind == "load" ? StateRequest::Load : StateRequest::Hash,
                                      std::stoull(item.substr(at + 1))});
            }
        }
        else if (a == "--edition" && n + 1 < argc && (mk3host::Narrow(argv[n + 1]) == "original" || mk3host::Narrow(argv[n + 1]) == "remastered"))
            remastered = mk3host::Narrow(argv[++n]) == "remastered";
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    try {
        const auto program = mk3::LoadProgram(argv[1]);
        const auto u2 = mk3::LoadDcsU2(argv[1]);
        std::ifstream seed(std::filesystem::path(argv[2]), std::ios::binary);
        std::vector<uint8_t> cmos{std::istreambuf_iterator<char>(seed), std::istreambuf_iterator<char>()};
        std::unique_ptr<mk3host::CmosProfile> profile;
        bool cmos_from_profile = false;
        if (!cmos_dir.empty()) { profile = std::make_unique<mk3host::CmosProfile>(cmos_dir); cmos = profile->Load(cmos, cmos_from_profile); }
        struct Tape { uint64_t cycle; unsigned port; uint16_t value; };
        std::vector<Tape> tape;
        if (!tape_file.empty()) {
            std::ifstream in{std::filesystem::path(tape_file)};
            if (!in) throw std::runtime_error("cannot read the tape");
            for (std::string line; std::getline(in, line);) {
                std::istringstream row(line);
                Tape t{};
                unsigned value = 0;
                if (!(row >> std::ws) || row.peek() == '#' || row.eof()) continue;
                if (!(row >> t.cycle >> t.port >> value) || t.port > 3 || value > 0xffff || !(tape.empty() || t.cycle >= tape.back().cycle))
                    throw std::runtime_error("bad tape row: " + line);
                t.value = uint16_t(value);
                tape.push_back(t);
            }
        }
        const auto gfx = LoadGraphics(argv[3]);
        auto board = std::make_unique<mk3::Board>(program, cmos, u2, gfx);
        mk3::Board& b = *board;
        if (dsw_word) b.o.ports[2] = *dsw_word;   // the DIP switches are set before power-on
        const uint16_t dsw = b.o.ports[2];
        b.dcs.AttachSoundData(argv[1]);
        mk3::FirstExecution log;
        b.SetProfile(strict ? mk3::Profile::Strict : mk3::Profile::Game, &log);
        b.dcs.observe_access = [](const mk3::Dcs8k&, const mk3::AdspAccess&) { return false; };   // no access history
        mk3::Scanout scanout;
        b.AttachScanout(scanout);
        Census census;
        if (census_on) { census.pc = &b.o.instruction_pc; census.b14 = &b.s.b[14]; census.frame = &scanout.frames; b.o.blitter.observer = &census; }
        // Remastered layer 2 (presentation only): DMA classes -> scenery backdrop -> 4x bicubic of the scenery pixels.
        const bool layer2 = remastered && !census_on;
        auto layers = std::make_unique<mk3::DmaLayers>();
        CpuTap cpu_tap;
        if (layer2) {
            layers->writer_pc = &b.o.instruction_pc; layers->b14 = &b.s.b[14]; layers->ram = &b.o.ram; layers->palette = &b.o.palette;
            mk3::EnableFloorBlood(layers->blood, program, b.o.blitter.gfx);
            b.o.blitter.observer = layers.get(); scanout.layers = layers.get();
            cpu_tap.layers = layers.get(); b.o.vram_observer = &cpu_tap;
        }
        // Widescreen (MK3_WIDESCREEN=1 from the launcher, Remastered window; --wide headless): the shadow pass's 16:9 frame.
        if (const char* flag = std::getenv("MK3_WIDESCREEN"); window_on && flag && std::string(flag) == "1") wide_on = true;
        const bool widescreen = layer2 && wide_on;
        auto wide = std::make_unique<mk3::Wide>();
        if (widescreen) {
            wide->Attach(b, layers.get(), &cpu_tap, &layers->blood); scanout.EnableWide(&wide->layer, true);
            scanout.wide_dim = [&wide] { return wide->Dim(); };
        }
        mk3::Wide* state_wide = widescreen ? wide.get() : nullptr;
        WideChecks wide_checks;
        uint64_t scenery_frames = 0, scenery_pixels = 0, mismatched = 0;
        uint64_t fighter_frames = 0, fighter_layers = 0, fighter_mismatched = 0;
        mk5presentation::FighterFrame fighters_shown;
        std::vector<uint32_t> backdrop_shown;
        std::vector<mk5training::Box> frame_boxes;

        std::unique_ptr<Mk5Window> window;
        if (window_on) {
            window = widescreen ? std::make_unique<Mk5Window>(false, mk5wide::kAcross, mk5wide::kDown) : std::make_unique<Mk5Window>();
            mk3host::SetTitle(GameTitle() + (remastered ? L"Remasterizada" : L"Original"));
        }
        std::unique_ptr<mk5audio::WaveOutSink> sink;
        if (audio_on) sink = std::make_unique<mk5audio::WaveOutSink>(1, mk5audio::WaveOutPacket::Ms10);
        uint64_t output_samples = 0, output_nonzero = 0;
        // D5: host gain on the unity DCS output, then the int16 the sink plays; the canonical DCS PCM is unchanged.
        mk5audio::HostGain host_gain(mk3host::kMasterDb);
        uint64_t pcm_hash = 0xcbf29ce484222325ull;   // --state-test: the host PCM since the last hashed frame
        mk2::DcsFloatAudio audio([&](const float* f, size_t n) {
            int16_t s[1024];
            for (size_t k = 0; k < n; ++k) s[k] = host_gain.Render<1>({double(f[k]) * 32768.0})[0];
            for (size_t k = 0; k < n; ++k) pcm_hash = (pcm_hash ^ uint16_t(s[k])) * 0x100000001b3ull;
            output_samples += n;
            for (size_t k = 0; k < n; ++k) output_nonzero += s[k] != 0;
            if (sink) sink->Push(s, n);
        });
        std::set<uint32_t> sample_ns;
        size_t consumed = 0, blocks_seen = 0;
        uint64_t source_nonzero = 0, trimmed_samples = 0, input_changes = 0, dcs_commands = 0, watch_cycle = 0;
        size_t tape_next = 0;
        int dcs_high = -1;
        mk3host::Input input;
        // DCS commands: the sound-port byte writes in pairs (high, low), in write order.
        const auto scan_dcs = [&] {
            for (const auto& e : b.host_events) {
                if (e.kind != mk3::HostEvent::DataWrite) continue;
                if (dcs_high < 0) { dcs_high = e.value & 0xff; continue; }
                ++dcs_commands;
                if (((dcs_high << 8) | (e.value & 0xff)) == watch_dcs && !watch_cycle) watch_cycle = e.write_cycle;
                dcs_high = -1;
            }
        };
        uint16_t last_in[3] = {0xffff, 0xffff, 0xffff};
        std::vector<uint16_t> previous_pens(size_t(mk3::Scanout::kFrameWidth) * mk3::Scanout::kFrameHeight, mk3::Scanout::kBlackPen);
        std::vector<uint32_t> view, previous_rgb(previous_pens.size()), shown;
        bool stop = false, closed = false;
        const auto started = std::chrono::steady_clock::now();
        // F5/F7 (and --state-test): taken at the next TMS instruction boundary, where `stop` ends Run resumably.
        std::vector<StateRequest> state_requests;
        size_t state_plan_next = 0;
        std::vector<uint8_t> slot;
        const auto slot_path = std::filesystem::path(L"saves/mk3") / program.sha256 / (remastered ? L"slot1-remastered.state" : L"slot1.state");
        // Host fields of the state: the stream into the sink, its PCM cursor, the hash and tape cursors.
        const auto host_state = [&](auto&& apply) {
            return apply(audio, host_gain, consumed, blocks_seen, sample_ns, dcs_high, previous_pens, tape_next);
        };
        mk3::DmaLayers* state_layers = layer2 ? layers.get() : nullptr;
        // Run-ahead (F6): each real frame is held; the frame after it, run from a snapshot at the next instruction
        // boundary with the same inputs, is shown (never heard), then undone. 1: snapshot due, 2: speculating, 3: undo due.
        if (window && window->RunAhead()) run_ahead = true;
        if (window && window->Training()) mk5hitbox::enabled = true;
        if (census_on) run_ahead = false;
        int ahead = 0;
        mk2::state::Snapshot ahead_state;
        const auto ahead_fields = [&](auto& a) { host_state([&](auto&... host) { mk3::state::Session(a, b, scanout, state_layers, state_wide, host...); return 0; }); };
        const auto state_request = [&](StateRequest request) {
            if (census_on) { std::printf("{\"save_state\":\"refused\",\"error\":\"not with --dma-census\"}\n"); return; }
            if (request != StateRequest::Load) {
                auto bytes = host_state([&](auto&... host) { return mk3::SaveLiveState(program, b, scanout, state_layers, state_wide, host...); });
                bool written = false;
                if (request == StateRequest::Save) {
                    std::error_code error;
                    std::filesystem::create_directories(slot_path.parent_path(), error);
                    written = !error && mk3host::ReplaceFileBytes(slot_path, bytes);
                    slot = bytes;
                }
                std::printf("{\"save_state\":\"%s\",\"frame\":%llu,\"bytes\":%zu,\"sha256\":\"%s\",\"file\":%s}\n",
                            request == StateRequest::Save ? "saved" : "hash", (unsigned long long)scanout.frames, bytes.size(),
                            Sha256(bytes).c_str(), written ? "true" : "false");
                return;
            }
            auto bytes = slot;
            const bool from_file = bytes.empty();
            try {
                if (from_file) {
                    std::ifstream file(slot_path, std::ios::binary);
                    if (!file) throw std::runtime_error("no saved state");
                    bytes.assign(std::istreambuf_iterator<char>(file), {});
                }
                mk3::state::CheckLiveState(program, bytes, mk3::state::Edition(state_layers));
            } catch (const std::exception& e) {
                std::printf("{\"save_state\":\"refused\",\"error\":\"%s\"}\n", e.what());
                return;
            }
            host_state([&](auto&... host) { mk3::LoadLiveState(program, bytes, b, scanout, state_layers, state_wide, host...); return 0; });
            if (tape.empty()) b.o.ports[2] = dsw;   // the cabinet's switches, not the saved session's
            std::printf("{\"save_state\":\"loaded\",\"frame\":%llu,\"from\":\"%s\"}\n", (unsigned long long)scanout.frames,
                        from_file ? "file" : "memory");
        };

        // Inputs at the frame update, as MAME polls the host once per frame.
        const auto poll = [&] {
            if (!tape.empty()) return;
            const auto p = input.Poll();
            const uint16_t now[3] = {p.in0, p.in1, p.in2};
            for (int k = 0; k < 3; ++k) if (now[k] != last_in[k]) { ++input_changes; last_in[k] = now[k]; }
            b.o.ports[0] = p.in0; b.o.ports[1] = p.in1; b.o.ports[3] = p.in2;
        };
        const auto show = [&](const mk3::Scanout& s, bool scenery) {
            shown = layer2 ? s.presentation : s.frame;   // Remastered: with the floor blood
            for (auto& v : shown) v |= 0xff000000u;
            if (scenery) backdrop_shown = s.backdrop; else backdrop_shown.clear();
            fighters_shown.layers.clear();
            if (s.fighters.serial) fighters_shown = s.fighters;
            if (widescreen && s.wide_presentation.size() == size_t(mk3::Scanout::kWideWidth) * mk3::Scanout::kFrameHeight) {
                auto boxes = frame_boxes;   // the world's boxes move with the window
                for (auto& box : boxes) box.x += mk5wide::kWideMargin - s.wide_shift;
                window->Present(s.wide_presentation, mk3::Scanout::kWideWidth, mk3::Scanout::kFrameHeight, s.wide_backdrop, s.wide_fighters, boxes);
                return;
            }
            window->Present(shown, mk3::Scanout::kFrameWidth, mk3::Scanout::kFrameHeight, backdrop_shown, fighters_shown, frame_boxes);   // paced to the 146234-clock raster
        };
        scanout.presented = [&](const mk3::Scanout& s) {
            mk5hitbox::Capture(mk5hitbox::kMk3, b.o.ram); frame_boxes = mk5hitbox::Take();   // training mode (F8), read-only
            if (ahead == 2) {
                ahead = 3;
                if (hashes) std::printf("{\"ahead\":%llu,\"rgb\":\"%016llx\"}\n", (unsigned long long)s.frames, (unsigned long long)HashRgb(s.frame));
                if (!window) return;
                if (!window->Pump()) { stop = closed = true; return; }
                show(s, layer2 && std::any_of(s.backdrop.begin(), s.backdrop.end(), [](uint32_t v) { return (v & mk3::Scanout::kSceneryRgb) != 0; }));
                return;
            }
            if (run_ahead && window && !window->RunAhead()) run_ahead = false;   // the window stops it when the frame does not fit
            // Audio: every DAC sample delivered so far, through the shared DCS 31250 Hz -> 48 kHz resampler.
            for (; blocks_seen < b.dcs.dac_blocks.size(); ++blocks_seen) sample_ns.insert(b.dcs.dac_blocks[blocks_seen].sample_ns);
            if (consumed < b.dcs.pcm.size() && (sample_ns.size() != 1 || *sample_ns.begin() != 32000))
                throw std::runtime_error("MK3: DCS sample period is not 32000 ns (31250 Hz), which the shared resampler requires");
            float block[512];
            while (consumed < b.dcs.pcm.size()) {
                size_t n = 0;
                for (; n < 512 && consumed + n < b.dcs.pcm.size(); ++n) {
                    const int16_t v = b.dcs.pcm[consumed + n];
                    source_nonzero += v != 0;
                    block[n] = float(v) * (1.0f / 32767.0f);
                }
                audio.Feed(audio.SourceSamples(), block, n);
                consumed += n;
            }
            // Observation histories only (bus/IRQ/ADSP logs); nothing in the emulation reads them back.
            scan_dcs();
            if (census_on) census.CpuWrites(b.s.writes);
            b.s.writes.clear(); b.s.reads.clear(); b.o.interrupts.clear(); b.host_events.clear(); b.dcs.host_reads.clear();
            // Run-ahead trims every frame: the consumed PCM would otherwise be copied twice per frame by its snapshot.
            if (b.dcs.pcm.size() > (run_ahead ? 0u : 1u << 20)) { trimmed_samples += b.dcs.pcm.size(); b.dcs.pcm.clear(); b.dcs.dac_blocks.clear(); consumed = blocks_seen = 0; }

            const uint64_t frame = s.frames;
            // Every flagged backdrop pixel is the canonical pixel's own colour (same pen, same palette): counted, must be 0.
            uint64_t flagged = 0;
            if (layer2) {
                for (size_t i = 0; i < s.frame.size(); ++i)
                    if (s.backdrop[i] & mk3::Scanout::kSceneryRgb) { ++flagged; mismatched += (s.backdrop[i] & 0xffffff) != s.frame[i]; }
                scenery_pixels += flagged; scenery_frames += flagged != 0;
                if (s.fighters.serial) { ++fighter_frames; fighter_layers += s.fighters.layers.size(); fighter_mismatched += s.FighterMismatched(); }
            }
            // A load moves the frame counter: the items after it wait for the frames that follow it.
            while (state_plan_next < state_plan.size() && frame >= state_plan[state_plan_next].frame) {
                state_requests.push_back(state_plan[state_plan_next++].request);
                if (state_requests.back() == StateRequest::Load) break;
            }
            const bool dump = dump_frames.count(frame) != 0;
            if (hashes || dump) {
                view.resize(previous_pens.size());
                for (size_t i = 0; i < view.size(); ++i) {
                    const uint16_t pen = previous_pens[i];
                    previous_rgb[i] = pen == mk3::Scanout::kBlackPen ? 0 : mk3::Scanout::Rgb(b.o.palette[pen]);
                }
                // Board::As is 128-bit attoseconds; print seconds and the 18-digit remainder as one decimal string.
                const unsigned long long sec = (unsigned long long)(b.base / 1000000000000000000ull),
                                         atto = (unsigned long long)(b.base % 1000000000000000000ull);
                // Headless Remastered: the 4x presentation the window would compose (mk5stage::Bicubic4x).
                std::vector<uint32_t> composed;
                if (layer2 && !window && s.fighters.serial) composed = mk5presentation::ComposeFighterFrame4x(s.fighters, mk3::Scanout::kFrameWidth, mk3::Scanout::kFrameHeight);
                else if (layer2 && !window && flagged) composed = mk5stage::Bicubic4x(s.presentation, s.backdrop, mk3::Scanout::kFrameWidth, mk3::Scanout::kFrameHeight);
                if (hashes) {
                    std::printf("{\"frame\":%llu,\"as\":\"%llu%018llu\",\"cycle\":%llu,\"rgb\":\"%016llx\",\"prev\":\"%016llx\",\"palette\":\"%llx\"",
                                (unsigned long long)frame, sec, atto, (unsigned long long)b.Cycles(),
                                (unsigned long long)HashRgb(s.frame), (unsigned long long)HashRgb(previous_rgb),
                                (unsigned long long)HashWords(b.o.palette));
                    if (!state_plan.empty() || run_ahead || state_hash) { std::printf(",\"pcm\":\"%016llx\"", (unsigned long long)pcm_hash); pcm_hash = 0xcbf29ce484222325ull; }
                    if (state_hash) {
                        uint64_t h = kFnvBasis;
                        for (const uint16_t v : b.o.ram) h = (h ^ v) * kFnvPrime;
                        for (const uint16_t v : b.o.vram) h = (h ^ v) * kFnvPrime;
                        for (const uint16_t v : b.o.cmos) h = (h ^ v) * kFnvPrime;
                        for (const uint32_t v : b.s.a) h = (h ^ v) * kFnvPrime;
                        for (const uint32_t v : b.s.b) h = (h ^ v) * kFnvPrime;
                        h = (h ^ b.s.status) * kFnvPrime; h = (h ^ b.s.pc) * kFnvPrime; h = (h ^ b.s.instructions) * kFnvPrime;
                        const auto word = [&](uint32_t bit) { return int(int16_t(b.o.ram[(bit - 0x01000000) / 16])); };
                        std::printf(",\"state\":\"%016llx\",\"game\":[%d,%d,%d,%d]", (unsigned long long)h, word(mk3::wide::kMode), word(mk3::wide::kCameraX),
                                    word(mk3::wide::kCameraMin), word(mk3::wide::kCameraMax));
                    }
                    if (widescreen) {
                        const auto& au = s.wide_audit; const auto& l = wide->layer;
                        wide_checks.Frame(l);
                        std::printf(",\"checks\":[%llu,%llu,%llu,%llu]", (unsigned long long)wide_checks.parallax_checked, (unsigned long long)wide_checks.parallax_differ,
                                    (unsigned long long)wide_checks.still_checked, (unsigned long long)wide_checks.still_moved);
                        std::printf(",\"wide\":{\"centre\":%llu,\"filled\":%llu,\"shift\":%d,\"whole\":%d,\"soft\":%d,\"audit\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu],"
                                    "\"faults\":%llu,\"fault_pc\":%u,\"skips\":%llu,\"passes\":%llu,\"fight\":%d,\"arena\":%d,\"open\":%d,\"rgb\":\"%016llx\",\"bands\":[",
                                    (unsigned long long)s.wide_centre, (unsigned long long)s.wide_filled, s.wide_shift, int(s.wide_whole), int(s.wide_soft),
                                    (unsigned long long)au.shown, (unsigned long long)au.sentinel, (unsigned long long)au.masked, (unsigned long long)au.outside,
                                    (unsigned long long)au.outside_real, (unsigned long long)au.generated, (unsigned long long)au.stale,
                                    (unsigned long long)wide->faults, unsigned(wide->fault_pc), (unsigned long long)wide->streamer_skips,
                                    (unsigned long long)wide->passes, int(l.fight), int(l.arena), int(l.open), (unsigned long long)HashRgb(s.wide_presentation));
                        // Each measured scenery layer of the last build: the margin columns its art covers at the least.
                        for (size_t k = 0; k < l.bands_done.size(); ++k) {
                            int left = mk5wide::kWideMargin, right = mk5wide::kWideMargin;
                            for (const auto& r : l.bands_done[k].rows) {
                                left = std::min(left, std::max(0, mk5wide::kWideMargin - std::max<int>(r.left, 0)));
                                right = std::min(right, std::max(0, std::min<int>(r.right, mk5wide::kLayerWidth) - (mk5wide::kWideMargin + mk5wide::kVisible)));
                            }
                            std::printf("%s[%u,%d,%d]", k ? "," : "", unsigned(l.bands_done[k].layer), left, right);
                        }
                        std::printf("]}");
                    }
                    if (mk5hitbox::enabled) {
                        std::printf(",\"boxes\":[");
                        for (size_t i = 0; i < frame_boxes.size(); ++i)
                            std::printf("%s[%d,%d,%d,%d,%d]", i ? "," : "", frame_boxes[i].x, frame_boxes[i].y, frame_boxes[i].w, frame_boxes[i].h, int(frame_boxes[i].kind));
                        std::printf("]");
                    }
                    if (layer2) std::printf(",\"scenery\":%llu,\"fighters\":%zu,\"blood\":%zu,\"rm\":\"%016llx\"", (unsigned long long)flagged, s.fighters.serial ? s.fighters.layers.size() : size_t(0),
                                            layers->blood.stains.size(),
                                            (unsigned long long)(composed.empty() ? 0 : HashRgb(composed)));
                    std::printf("}\n");
                }
                if (dump) {
                    std::printf("{\"dump\":%llu,\"rgb\":\"", (unsigned long long)frame); Hex(s.frame);
                    std::printf("\",\"prev\":\""); Hex(previous_rgb);
                    if (layer2) { std::printf("\",\"backdrop\":\""); for (const uint32_t v : s.backdrop) std::printf("%08x", unsigned(v)); }
                    if (layer2) { std::printf("\",\"present\":\""); Hex(s.presentation); }
                    if (layer2) {   // writer class per visible pixel (DmaClass; 7 = CPU-written; 8 = blank)
                        std::printf("\",\"classes\":\"");
                        for (int y = 0; y < mk3::Scanout::kFrameHeight; ++y)
                            for (int x = 0; x < mk3::Scanout::kFrameWidth; ++x) {
                                const uint32_t pen = s.backdrop_pens[size_t(y + mk3::Scanout::kTop) * mk3::Scanout::kWidth + size_t(x + mk3::Scanout::kLeft)];
                                std::printf("%x", (pen & 0xffff) == mk3::Scanout::kBlackPen ? 8u : unsigned(pen >> 17) & 7u);
                            }
                    }
                    if (!composed.empty() && !widescreen) { std::printf("\",\"rm4x\":\""); Hex(composed); }
                    if (widescreen) { std::printf("\",\"wide\":\""); Hex(s.wide_presentation); std::printf("\",\"wide_raw\":\""); Hex(s.wide_frame); }
                    if (widescreen && dump_4x) {
                        const unsigned w = mk3::Scanout::kWideWidth, h = mk3::Scanout::kFrameHeight;
                        const auto wide4x = s.wide_fighters.serial ? mk5presentation::ComposeFighterFrame4x(s.wide_fighters, w, h)
                                          : !s.wide_backdrop.empty() ? mk5stage::Bicubic4x(s.wide_presentation, s.wide_backdrop, w, h) : std::vector<uint32_t>{};
                        std::printf("\",\"wide4x\":\""); Hex(wide4x);
                    }
                    if (widescreen && wide->layer.debug) {   // the last build's staged draws as placed: list, columns, row, rows, fixed, source
                        std::printf("\",\"draws\":\"");
                        for (const auto& d : wide->layer.placed_done) std::printf("%d:%d:%d:%u:%u:%d:%x ", int(d.layer), int(d.lo), int(d.hi), unsigned(d.row), unsigned(d.rows), int(d.fixed), unsigned(d.source));
                        std::printf("\",\"planes\":\"");   // per scroll slot: ratio, the arcade's span, the measured and the taken bounds
                        for (unsigned k = 0; k < 9; ++k) { const auto& l = wide->layer;
                            std::printf("%u:%lld/%lld:%d..%d:%d..%d:%d..%d ", k, (long long)l.ratio[k].plane, (long long)l.ratio[k].world, l.span[k][0], l.span[k][1],
                                        l.want[k][0], l.want[k][1], l.bound[k][0], l.bound[k][1]); }
                        std::printf("\",\"notes\":\"");
                        for (const auto& d : wide->layer.notes_done) std::printf("%u:%d:%d:%u:%d ", unsigned(d.layer), int(d.lo), int(d.hi), unsigned(d.row), int(d.camera));
                    }
                    std::printf("\"}\n");
                }
            }
            for (int y = 0; y < mk3::Scanout::kFrameHeight; ++y)
                for (int x = 0; x < mk3::Scanout::kFrameWidth; ++x)
                    previous_pens[size_t(y) * mk3::Scanout::kFrameWidth + x] =
                        s.pens[size_t(y + mk3::Scanout::kTop) * mk3::Scanout::kWidth + size_t(x + mk3::Scanout::kLeft)];

            if (window) {
                if (!window->Pump()) { stop = closed = true; return; }
                if (!run_ahead) show(s, flagged != 0);
                for (const auto r : window->TakeStateRequests())
                    state_requests.push_back(r == Mk5Window::StateRequest::Save ? StateRequest::Save : StateRequest::Load);
                if (!run_ahead) poll();   // run-ahead polls after the undo, before the real frame that reads it
            }
            if (max_frames && frame >= max_frames) stop = true;
            if (run_ahead && !stop) ahead = 1;
        };
        b.stop = [&](const mk3::Board& board) {
            if ((!state_requests.empty() || ahead == 1 || ahead == 3) && !stop) return true;   // before the observers' Step: Run resumes at this instruction
            if (census_on) census.queue.Step(board.s.pc, board.s.a.data());
            if (layer2) layers->Step(board.s.pc, board.s.a.data());
            if (widescreen) wide->Step();
            if (board.s.pc == mk5hitbox::kMk3.strike_pc) mk5hitbox::Strike(mk5hitbox::kMk3, board.s.a.data(), b.o.ram);
            if (tape_next < tape.size()) {
                const uint64_t cycle = board.Cycles();
                if (cycle >= 1000 && !(board.s.status & 0x02000000u))   // ST.P: a PIXBLT restart re-entry is not a boundary
                    for (; tape_next < tape.size() && tape[tape_next].cycle <= cycle; ++tape_next) b.o.ports[tape[tape_next].port] = tape[tape_next].value;
            }
            return stop;
        };

        std::string frontier = "none";
        try {
            for (b.Run(); !stop && (!state_requests.empty() || ahead == 1 || ahead == 3); b.Run()) {
                const auto requests = std::move(state_requests);
                state_requests.clear();
                for (const auto r : requests) state_request(r);
                if (ahead == 1) { ahead_state.Save(ahead_fields); ahead = 2; mk5hitbox::strikes.clear(); }   // the shown frame's own
                else if (ahead == 3) { ahead_state.Load(ahead_fields); ahead = 0; if (window) poll(); }
            }
            if (ahead >= 2) { ahead_state.Load(ahead_fields); ahead = 0; }   // closed during the speculative frame
        } catch (const std::runtime_error& e) { frontier = e.what(); }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (sink) sink->Drain();
        scan_dcs();
        // As MK2: the NVRAM is written only after a session that ended at no translation frontier and that the shared
        // D3D12 presenter showed (mk5_profile_presentation.hpp); otherwise the profile keeps its previous image.
        if (!cmos_out.empty() && frontier == "none" && b.dcs_frontier.empty() && !mk3host::ReplaceFileBytes(cmos_out, mk3host::CmosImage(b.o.cmos)))
            throw std::runtime_error("MK3 --cmos-out write failed");
        std::string cmos_refused;
        if (profile) {
            try {
                if (frontier != "none" || !b.dcs_frontier.empty()) throw std::runtime_error("the session stopped at a translation frontier");
                if (!window) throw std::runtime_error("no D3D12 presentation (headless session)");
                const auto confirm_window = [&] {
                    const bool running = window->Pump();
                    window->Pump();   // surface a failure published around the running/close observation
                    if (!running && !closed) throw std::runtime_error("native profile window closed before successful completion");
                };
                confirm_window();
                ValidateProfilePresentation(window->presentation_json(), closed, window->GraphicsValidationEnabled());
                confirm_window();   // again after the metrics snapshot, before the durable write
                profile->Save(b.o.cmos);
            } catch (const std::exception& e) { cmos_refused = e.what(); }
        }

        std::map<std::string, uint64_t> kinds;
        for (const auto& e : log.order) ++kinds[KindName(e.kind)];
        if (window && !closed) {
            std::wstring title = GameTitle() + (remastered ? L"Remasterizada | " : L"Original | ");
            title += frontier == "none" ? L"fim da sonda" : L"parada: " + mk3host::Widen(frontier);
            wchar_t counts[160];
            std::swprintf(counts, 160, L" | PC 0x%08x | 1ª exec.: %llu blocos TMS, %llu arestas | Esc", unsigned(b.s.pc),
                          (unsigned long long)kinds["tms_block"], (unsigned long long)kinds["tms_edge"]);
            mk3host::SetTitle(title + counts);
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(linger < 0 ? 1e9 : linger);
            while (window->Pump() && std::chrono::steady_clock::now() < until) {
                window->Present(shown.empty() ? std::vector<uint32_t>(size_t(mk3::Scanout::kFrameWidth) * mk3::Scanout::kFrameHeight, 0xff000000u) : shown,
                                mk3::Scanout::kFrameWidth, mk3::Scanout::kFrameHeight);
            }
        }
        if (census_on) census.Print();
        const double emulated = double(b.Cycles()) * 160e-9;
        std::printf("{\"preview\":true,\"profile\":\"%s\",\"frames\":%llu,\"cycles\":%llu,\"emulated_seconds\":%.3f,\"wall_seconds\":%.3f,"
                    "\"fps\":%.2f,\"realtime_factor\":%.3f,\"frontier\":\"%s\",\"stop_pc\":%u,\"dcs_frontier\":\"%s\",\"last_frame\":\"%016llx\",",
                    strict ? "strict" : "game", (unsigned long long)scanout.frames, (unsigned long long)b.Cycles(), emulated, wall,
                    wall > 0 ? double(scanout.frames) / wall : 0.0, wall > 0 ? emulated / wall : 0.0, frontier.c_str(), b.s.pc,
                    b.dcs_frontier.c_str(), (unsigned long long)HashRgb(scanout.frame));
        std::printf("\"audio\":{\"source_samples\":%llu,\"source_nonzero\":%llu,\"output_samples\":%llu,\"output_nonzero\":%llu,\"sample_ns\":[",
                    (unsigned long long)(trimmed_samples + consumed), (unsigned long long)source_nonzero,
                    (unsigned long long)output_samples, (unsigned long long)output_nonzero);
        bool first = true;
        for (const auto v : sample_ns) { std::printf("%s%u", first ? "" : ",", v); first = false; }
        std::printf("],\"host_gain_db\":%.2f,\"host_limited\":%llu,\"host_clipped\":%llu,\"device\":%s", mk3host::kMasterDb,
                    (unsigned long long)host_gain.Limited(), (unsigned long long)host_gain.Clipped(), sink ? "true" : "false");
        if (sink) std::printf(",\"submitted\":%llu,\"completed\":%llu,\"underruns\":%llu,\"queue_ms\":%u,\"max_producer_gap_ms\":%llu",
                              (unsigned long long)sink->Submitted(), (unsigned long long)sink->Completed(), (unsigned long long)sink->Underruns(),
                              sink->QueueCapacityMs(), (unsigned long long)sink->MaxProducerGapMs());
        std::printf("},\"inputs\":{\"changes\":%llu,\"pads\":%u},\"tape\":{\"events\":%zu,\"applied\":%zu},\"dcs\":{\"commands\":%llu,\"watch\":%d,\"watch_cycle\":%llu},"
                    "\"edition\":\"%s\",\"dsw\":%u,\"cmos\":{\"profile\":%s,\"from_profile\":%s,\"saved\":%s,\"refused\":\"%s\"},\"first_execution\":{",
                    (unsigned long long)input_changes, input.pads(), tape.size(), tape_next, (unsigned long long)dcs_commands, watch_dcs,
                    (unsigned long long)watch_cycle, remastered ? "remastered" : "original", unsigned(dsw), profile ? "true" : "false",
                    cmos_from_profile ? "true" : "false", profile && cmos_refused.empty() ? "true" : "false", cmos_refused.c_str());
        first = true;
        for (const auto& [k, v] : kinds) { std::printf("%s\"%s\":%llu", first ? "" : ",", k.c_str(), (unsigned long long)v); first = false; }
        std::printf("}");
        if (layer2) {
            std::printf(",\"layer2\":{\"scenery_frames\":%llu,\"scenery_pixels\":%llu,\"mismatched\":%llu,\"staged_left\":%zu,"
                        "\"fighter_frames\":%llu,\"fighter_layers\":%llu,\"fighter_mismatched\":%llu,\"cpu_words\":{\"cpu\":%llu,\"page_fill\":%llu,\"picture\":%llu},"
                        "\"bound_draws\":{\"child\":%llu,\"screen_art\":%llu,\"text\":%llu},\"classes\":{",
                        (unsigned long long)scenery_frames, (unsigned long long)scenery_pixels, (unsigned long long)mismatched,
                        layers->queue.staged.size(), (unsigned long long)fighter_frames,
                        (unsigned long long)fighter_layers, (unsigned long long)fighter_mismatched,
                        (unsigned long long)layers->cpu_words[0], (unsigned long long)layers->cpu_words[1], (unsigned long long)layers->cpu_words[2],
                        (unsigned long long)layers->bound_draws[0], (unsigned long long)layers->bound_draws[1], (unsigned long long)layers->bound_draws[2]);
            for (unsigned k = 0; k < 8; ++k)
                std::printf("%s\"%s\":{\"draws\":%llu,\"pixels\":%llu}", k ? "," : "", mk3::kDmaClassNames[k], (unsigned long long)layers->draws[k], (unsigned long long)layers->pixels[k]);
            std::printf("}}");
        }
        std::printf(",\"window\":%s,\"closed_by_user\":%s}\n", window ? window->presentation_json().c_str() : "null", closed ? "true" : "false");
        std::fflush(stdout);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
