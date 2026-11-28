#include "mk5_boot_flow.hpp"
#include "mk5_task_body.hpp"
#include "mk5_scanout.hpp"
#include "mk5_window.hpp"
#include "mk5_hitbox.hpp"
#include "mk5_audio_link.hpp"
#include "mk5_save_state.hpp"
#include "mk5_profile_presentation.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
struct FlowOptions {
    std::filesystem::path roms,cmos,cmos_output,scanout_ppm,live_status;
    uint16_t initial_dsw=0,boot_dsw=0,task_dsw=0,pending=0,next_line=0;
    uint32_t phase=0;
    uint16_t in0=0xffff,in1=0xffff,in2=0xffff;uint8_t sound_status=0xff;
    BootDiInputs di_inputs{};
    bool execute_di=false;
    bool task_body=false;
    bool main_loop=false;
    bool decompressor=false;
    bool visual=false;
    bool factory=false;
    uint32_t window_frames=0;
    uint32_t main_di_limit=12,main_slices=4;
    TaskBodyInputs body;
};
uint32_t number(const std::wstring& v,uint32_t max){
    if(v.empty() || v[0]<L'0' || v[0]>L'9')throw std::runtime_error("boot-flow: expected unsigned decimal/hex value");
    size_t used=0;unsigned long n=std::stoul(v,&used,v.size()>2 && v[0]==L'0' && (v[1]==L'x' || v[1]==L'X')?16:10);
    if(used!=v.size() || n>max)throw std::runtime_error("boot-flow: numeric argument out of range");
    return uint32_t(n);
}
FlowOptions parse(int argc,wchar_t** argv){
    FlowOptions o;std::set<std::wstring> seen;
    for(int i=1;i<argc;i+=2){
        if(i+1>=argc)throw std::runtime_error("boot-flow: every option requires a value");
        std::wstring key=argv[i],v=argv[i+1];
        if(!seen.insert(key).second)throw std::runtime_error("boot-flow: duplicate option");
        if(key==L"--stage"){if(v!=L"boot-flow" && v!=L"task-body" && v!=L"main-loop" && v!=L"decompressor" && v!=L"visual" && v!=L"factory-cmos")throw std::runtime_error("boot-flow: conflicting stage");o.factory=v==L"factory-cmos";o.visual=v==L"visual";o.decompressor=v==L"decompressor" || o.visual;o.main_loop=v==L"main-loop" || o.decompressor;o.task_body=v!=L"boot-flow" && !o.factory;}
        else if(key==L"--roms"){if(v.empty())throw std::runtime_error("boot-flow: empty ROM path");o.roms=v;}
        else if(key==L"--cmos-input"){if(v.empty())throw std::runtime_error("boot-flow: empty CMOS path");o.cmos=v;}
        else if(key==L"--cmos-output"){if(v.empty())throw std::runtime_error("visual: empty CMOS output path");o.cmos_output=v;}
        else if(key==L"--live-status"){if(v.empty())throw std::runtime_error("visual: empty live status path");o.live_status=v;}
        else if(key==L"--scanout-ppm"){if(v.empty())throw std::runtime_error("decompressor: empty scanout path");o.scanout_ppm=v;}
        else if(key==L"--boot-dsw-word")o.boot_dsw=uint16_t(number(v,65535));
        else if(key==L"--task-dsw-word")o.task_dsw=uint16_t(number(v,65535));
        else if(key==L"--dsw-word")o.initial_dsw=uint16_t(number(v,65535));
        else if(key==L"--irq-pending")o.pending=uint16_t(number(v,65535));
        else if(key==L"--raster-next-line")o.next_line=uint16_t(number(v,288));
        else if(key==L"--raster-quanta"){o.phase=number(v,6325);if(!o.phase)throw std::runtime_error("boot-flow: raster phase must be positive");}
        else if(key==L"--irq-in01")o.di_inputs.in01=number(v,0xffffffff);
        else if(key==L"--irq-in2-dsw")o.di_inputs.in2_dsw=number(v,0xffffffff);
        else if(key==L"--body-dsw-word")o.body.dsw=uint16_t(number(v,65535));
        else if(key==L"--main-dsw-word")o.body.main_dsw=uint16_t(number(v,65535));
        else if(key==L"--body-sound-status-byte")o.body.sound_status=uint8_t(number(v,255));
        else if(key==L"--body-irq-in01"){if(!o.body.quiet_di)o.body.quiet_di=BootDiInputs{};o.body.quiet_di->in01=number(v,0xffffffff);}
        else if(key==L"--body-irq-in2-dsw"){if(!o.body.quiet_di)o.body.quiet_di=BootDiInputs{};o.body.quiet_di->in2_dsw=number(v,0xffffffff);}
        else if(key==L"--active-irq-in01"){if(!o.body.active_di)o.body.active_di=BootDiInputs{};o.body.active_di->in01=number(v,0xffffffff);}
        else if(key==L"--active-irq-in2-dsw"){if(!o.body.active_di)o.body.active_di=BootDiInputs{};o.body.active_di->in2_dsw=number(v,0xffffffff);}
        else if(key==L"--main-di-limit"){o.main_di_limit=number(v,64);if(!o.main_di_limit)throw std::runtime_error("main-loop: positive DI limit required");}
        else if(key==L"--main-slices"){o.main_slices=number(v,64);if(!o.main_slices)throw std::runtime_error("main-loop: positive slice count required");}
        else if(key==L"--window-frames"){o.window_frames=number(v,500000);}
        else if(key==L"--in0-word")o.in0=uint16_t(number(v,65535));
        else if(key==L"--in1-word")o.in1=uint16_t(number(v,65535));
        else if(key==L"--in2-word")o.in2=uint16_t(number(v,65535));
        else if(key==L"--sound-status-byte")o.sound_status=uint8_t(number(v,255));
        else throw std::runtime_error("boot-flow: unknown option");
    }
    for(const auto* key:{L"--stage",L"--roms",L"--boot-dsw-word",L"--task-dsw-word",L"--irq-pending",L"--raster-next-line",L"--raster-quanta"})if(!seen.count(key))throw std::runtime_error("boot-flow: requires ROMs, boot/task DSW snapshots, pending IRQ and explicit raster epoch");
    if(seen.count(L"--irq-in01")!=seen.count(L"--irq-in2-dsw"))throw std::runtime_error("boot-flow: DI requires both 32-bit input snapshots");
    o.execute_di=seen.count(L"--irq-in01")!=0;
    // The power-on path reads one cabinet: IN0..IN2 and the DSW switches hold
    // the same values at every read from reset to the first task entry.
    if(seen.count(L"--dsw-word") && o.initial_dsw!=o.boot_dsw)throw std::runtime_error("boot-flow: DSW must be one switch setting from reset");
    if(o.task_dsw!=o.boot_dsw)throw std::runtime_error("boot-flow: DSW must be one switch setting from reset");
    const BootDiInputs ports{uint32_t(o.in0)|(uint32_t(o.in1)<<16),uint32_t(o.in2)|(uint32_t(o.boot_dsw)<<16)};
    if(o.execute_di && (o.di_inputs.in01!=ports.in01 || o.di_inputs.in2_dsw!=ports.in2_dsw))throw std::runtime_error("boot-flow: boot DI inputs must equal IN0..IN2 and DSW");
    o.di_inputs=ports;
    if(seen.count(L"--body-irq-in01")!=seen.count(L"--body-irq-in2-dsw"))throw std::runtime_error("task-body: both fresh DI snapshots required");
    if(seen.count(L"--active-irq-in01")!=seen.count(L"--active-irq-in2-dsw"))throw std::runtime_error("main-loop: both fresh active DI snapshots required");
    if(!o.main_loop)for(const auto* k:{L"--active-irq-in01",L"--active-irq-in2-dsw",L"--main-di-limit",L"--main-slices"})if(seen.count(k))throw std::runtime_error("main-loop options require main-loop stage");
    if(!o.task_body)for(const auto* k:{L"--body-dsw-word",L"--main-dsw-word",L"--body-sound-status-byte",L"--body-irq-in01",L"--body-irq-in2-dsw"})if(seen.count(k))throw std::runtime_error("task-body options require task-body stage");
    if(!o.decompressor && o.main_slices>16)throw std::runtime_error("main-loop: at most 16 slices");
    if(!o.decompressor && !o.scanout_ppm.empty())throw std::runtime_error("scanout export requires decompressor stage");
    if(!o.visual && seen.count(L"--window-frames"))throw std::runtime_error("window frames require visual stage");
    if(!o.cmos_output.empty()){
        if(!o.visual && !o.factory)throw std::runtime_error("CMOS session output requires visual stage");
        o.cmos_output=std::filesystem::absolute(o.cmos_output);
        if(o.cmos_output.filename().empty() || std::filesystem::exists(o.cmos_output) || !std::filesystem::is_directory(o.cmos_output.parent_path()))throw std::runtime_error("CMOS output must be a new file in an existing directory");
    }
    if(o.factory && (!o.cmos.empty() || o.cmos_output.empty() || seen.count(L"--irq-in01")))throw std::runtime_error("factory-cmos: blank CMOS only, a new CMOS output file, no DI snapshots");
    if(!o.live_status.empty()){
        if(!o.visual)throw std::runtime_error("live status requires visual stage");
        o.live_status=std::filesystem::absolute(o.live_status).lexically_normal();
        if(o.live_status.filename().empty() || std::filesystem::exists(o.live_status) || !std::filesystem::is_directory(o.live_status.parent_path()))throw std::runtime_error("live status must be a new file in an existing directory");
        if((!o.cmos_output.empty() && o.live_status==o.cmos_output.lexically_normal()) || (!o.scanout_ppm.empty() && o.live_status==std::filesystem::absolute(o.scanout_ppm).lexically_normal()))throw std::runtime_error("live status must not share a session output path");
    }
    if(o.visual && (!o.body.active_di || !o.body.quiet_di || !o.execute_di || !o.body.dsw || !o.body.main_dsw || !o.body.sound_status))throw std::runtime_error("visual requires explicit body and active input snapshots");
    if(o.decompressor){o.body.stop_after_decompression=true;if(!seen.count(L"--main-slices"))o.main_slices=32;if(!seen.count(L"--main-di-limit"))o.main_di_limit=64;}
    return o;
}
template<size_t N>void array(const std::array<uint32_t,N>& a){std::cout<<'[';for(size_t i=0;i<a.size();++i){if(i)std::cout<<',';std::cout<<a[i];}std::cout<<']';}
class LiveStatus {
    HANDLE file=INVALID_HANDLE_VALUE;
public:
    explicit LiveStatus(const std::filesystem::path&path){if(!path.empty()){file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create exclusive live status file");}}
    ~LiveStatus(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    LiveStatus(const LiveStatus&)=delete;LiveStatus&operator=(const LiveStatus&)=delete;
    bool Enabled()const{return file!=INVALID_HANDLE_VALUE;}
    void Write(const std::string&text){
        if(!Enabled())return;
        if(text.size()>4096)throw std::runtime_error("live status size bound");
        LARGE_INTEGER zero{};DWORD written=0;
        if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN) || !WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr) || written!=text.size() || !SetEndOfFile(file))throw std::runtime_error("live status write failed");
    }
};
}
int Mk5BootFlowCli(int argc,wchar_t** argv,bool remastered,Mk5StateHost host){
    int error=2;
    try{
        auto o=parse(argc,argv);LiveStatus live_status(o.live_status);auto p=LoadMk5Program(o.roms);auto c=o.cmos.empty()?std::vector<uint16_t>(8192):LoadMk5CmosSnapshot(o.cmos);
        if(o.factory){
            // Blank CMOS (every word 0: an unprogrammed board, and MAME's NVRAM default for this driver). The game's own
            // power-on runs its factory restore and waits for input; that image, powered on again with the same cabinet,
            // has to reach the first task and come out unchanged. This is the CMOS seed the setup writes.
            const auto power_on=[&](std::vector<uint16_t> cmos,TaskBodyStop& stop){
                Mk5MemoryBuffers m;Mk5BoardState f;f.cmos=std::move(cmos);BootDiState di;
                Mk5IrqController irq(p,f.check.cpu,m);Mk5TaskBodyRunner runner(p,f,m,irq,di);
                Mk5BootInputs in;in.ports=o.di_inputs;in.sound_status=o.sound_status;in.pending=o.pending;in.next_line=o.next_line;in.quanta_to_next=o.phase;in.instruction_budget=24000000;
                stop=runner.Boot(in).stop;return f.cmos;
            };
            TaskBodyStop restore,reboot;const auto image=power_on(c,restore);const auto again=power_on(image,reboot);
            if(restore!=TaskBodyStop::Budget)throw std::runtime_error("factory-cmos: the power-on did not stay in the post-restore input wait");
            if(reboot!=TaskBodyStop::TaskEntry || again!=image)throw std::runtime_error("factory-cmos: the power-on rejected or changed the restored CMOS");
            SaveMk5CmosSnapshot(o.cmos_output,image);
            std::cout<<"{\"scope\":\"mk5-factory-cmos\",\"program_sha256\":\""<<p.sha256<<"\",\"stop\":\"factory-cmos\",\"cmos_sha256\":\""<<Sha256(Mk5CmosBytes(image))<<"\"}\n";
            return 0;
        }
        // Finish host I/O and window creation before boot publishes any PCM.
        // These operations must not drain a live bounded audio queue.
        std::unique_ptr<VerifiedMk5GlyphBank> glyph_bank;
        if(o.main_loop)glyph_bank=std::make_unique<VerifiedMk5GlyphBank>(LoadMk5GlyphBank(o.roms));
        // Widescreen (MK1_WIDESCREEN=1, Remastered window only): the shadow pass's 512 columns, shown 4:3 x 512/400.
        const char* widescreen_flag=std::getenv("MK1_WIDESCREEN");
        const bool widescreen=remastered && o.visual && widescreen_flag && std::string(widescreen_flag)=="1";
        std::unique_ptr<Mk5Window> prepared_window;
        if(o.visual)prepared_window=widescreen?std::make_unique<Mk5Window>(false,mk5wide::kAcross,mk5wide::kDown):std::make_unique<Mk5Window>();
        error=3;Mk5MemoryBuffers m;Mk5BoardState f;f.cmos=c;auto& s=f.check.cpu;
        uint64_t before=s.reference_cycles;std::string stop="boot-input";BootDiState di;TaskBodyResult body,boot;bool body_executed=false;
        TaskBodyResult main;bool main_executed=false;uint64_t main_cycles=0,main_instructions=0,main_yields=0,main_loops=0;size_t main_calls=0,dma_commands=0,dma_completions=0;uint64_t drawn_pixels=0,shift_loads=0,shift_stores=0;
        uint64_t scanout_frames=0,scanout_quanta=0;unsigned scanout_width=0,scanout_height=0;
        uint64_t window_frames=0,input_events=0;bool window_presented=false,cmos_saved=false,cmos_presented=false;std::string window_stop,window_presentation="null",cmos_refused;
        std::vector<Mk5IrqEntry> entries;uint64_t elapsed=0;size_t scanlines=0;std::array<uint16_t,32> io{};Mk5DiagnosticTrim discarded;
        {
            Mk5IrqController irq(p,s,m);Mk5TaskBodyRunner runner(p,f,m,irq,di);
            Mk5BootInputs boot_inputs;boot_inputs.ports=o.di_inputs;boot_inputs.sound_status=o.sound_status;boot_inputs.pending=o.pending;boot_inputs.next_line=o.next_line;boot_inputs.quanta_to_next=o.phase;
            boot=runner.Boot(boot_inputs);
            switch(boot.stop){
                case TaskBodyStop::TaskEntry:stop="task-body-untranslated";break;
                case TaskBodyStop::IrqHandler:stop="irq-handler-frontier";break;
                case TaskBodyStop::TemporalBarrier:stop="temporal-barrier";break;
                case TaskBodyStop::Budget:stop="boot-input";break;
                default:stop="untranslated-frontier";
            }
            if(o.task_body && boot.stop==TaskBodyStop::TaskEntry){
                body=runner.Run(o.body);body_executed=true;
                switch(body.stop){
                    case TaskBodyStop::DswInput:stop="fresh-dsw-input";break;
                    case TaskBodyStop::SoundInput:stop="fresh-sound-input";break;
                    case TaskBodyStop::DiInput:stop="fresh-di-input";break;
                    case TaskBodyStop::IrqHandler:stop="irq-handler-frontier";break;
                    case TaskBodyStop::TemporalBarrier:stop="temporal-barrier";break;
                    case TaskBodyStop::Budget:stop="instruction-budget";break;
                    case TaskBodyStop::MainLoop:stop="first-main-loop-complete";break;
                    default:stop="untranslated-frontier";
                }
                if(o.main_loop && body.stop==TaskBodyStop::MainLoop){
                    const bool blood_decals=remastered && o.visual;
                    runner.AttachVideo(*glyph_bank,blood_decals);
                    std::unique_ptr<Mk5Scanout> screen;
                    if(o.decompressor)screen=std::make_unique<Mk5Scanout>(irq,m,blood_decals?&runner.Video():nullptr);
                    if(widescreen && screen){runner.EnableWide();screen->EnableWidePresentation(runner.Video());}
                    std::unique_ptr<Mk5Window> window;uint64_t shown_frame=0;auto active_base=o.body.active_di;
                    // F5/F7: one slot per edition, in memory and in saves/mk1/<program sha>/, between two ContinueMain calls.
                    const auto slot_path=std::filesystem::path(L"saves/mk1")/kMk5ProgramSha256/(remastered?L"slot1-remastered.state":L"slot1.state");
                    std::vector<uint8_t> slot;
                    const auto state_request=[&](Mk5Window::StateRequest request){
                        const Mk5LiveSession live{runner,*screen,*host.audio,*host.gain};
                        if(request==Mk5Window::StateRequest::Save){
                            slot=Mk5SaveLiveState(live);
                            std::error_code error;std::filesystem::create_directories(slot_path.parent_path(),error);
                            auto pending=slot_path;pending+=L".tmp";
                            {std::ofstream file(pending,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(slot.data()),std::streamsize(slot.size()));if(!file)error=std::make_error_code(std::errc::io_error);}
                            if(error || !MoveFileExW(pending.c_str(),slot_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                                std::cerr<<"MK1 save state kept in memory only: cannot write "<<slot_path.generic_string()<<'\n';
                            else std::cerr<<"MK1 state saved: "<<slot_path.generic_string()<<" ("<<slot.size()<<" bytes)\n";
                            return;
                        }
                        auto bytes=slot;
                        try{
                            if(bytes.empty()){std::ifstream file(slot_path,std::ios::binary);if(!file)throw std::runtime_error("no saved state");bytes.assign(std::istreambuf_iterator<char>(file),{});}
                            mk5state::CheckLiveState(bytes,mk5state::LiveIdentity(live));
                        }catch(const std::exception& e){std::cerr<<"MK1 save state not loaded: "<<e.what()<<'\n';return;}
                        Mk5LoadLiveState(bytes,live);
                        shown_frame=screen->frames();   // the restored frame counter; present from the next frame on
                        std::cerr<<"MK1 state loaded from "<<(slot.empty()?slot_path.generic_string():std::string("memory"))<<'\n';
                    };
                    // Keep the producer responsive while reaching the first
                    // decompressed frame as well as after window activation.
                    if(o.visual)o.body.instruction_budget=10000;
                    // Run-ahead (F6): each real frame is held; the frame after it, run from a snapshot, is shown, then undone.
                    const bool run_ahead=prepared_window&&prepared_window->RunAhead()&&host.audio&&host.gain&&screen;
                    mk2::state::Snapshot ahead;
                    // Training mode (F8): the window draws the inputs and the boxes the strike check tests (mk5_hitbox.hpp).
                    mk5hitbox::enabled=prepared_window&&prepared_window->Training();
                    const auto boxes=[&]{mk5hitbox::Capture(mk5hitbox::kMk1,m.ram);return mk5hitbox::Take();};
                    const auto present=[&]{
                        if(screen->wide_presentation_enabled()){
                            auto world=boxes();   // the world's boxes move with the window
                            const int move=int(Mk5Scanout::kWideWidth-screen->width())/2;
                            for(auto& box:world)box.x+=move;
                            window->Present(screen->wide_presentation(),Mk5Scanout::kWideWidth,screen->height(),
                                            screen->wide_backdrop_pixels(),screen->wide_fighter_frame(),world);
                        }else window->Present(screen->pixels(),screen->width(),screen->height(),
                                              screen->backdrop_pixels(),screen->fighter_frame(),boxes());
                    };
                    const uint64_t call_limit=o.visual?(o.window_frames?20000ull+uint64_t(o.window_frames)*16:uint64_t(-1)):o.main_slices;
                    for(uint64_t call=0;call<call_limit;++call){
                        if(o.visual && !window && call>=20000){window_stop="window-start-budget";break;}
                        if(window){if(!window->Pump()){window_stop="window-closed";break;}o.body.active_di=window->Inputs(*active_base);}
                        main=runner.ContinueMain(o.body,o.main_di_limit);main_executed=true;++main_calls;main_cycles+=main.reference_cycles;main_instructions+=main.instruction_count;main_yields+=main.yields;main_loops+=main.main_loops;
                        if(o.visual && main.stop==TaskBodyStop::DecompressorComplete){window=std::move(prepared_window);window->TakeStateRequests();/* drop keys pressed while loading */o.body.stop_after_decompression=false;o.body.instruction_budget=10000;shown_frame=screen->frames();continue;}
                        if(window && screen->frames()>shown_frame){
                            // A bounded run (a test) keeps every frame's status; a session rewrites the file ten times a
                            // second (its reader samples it four times a second): a file write per frame is the game's thread
                            // waiting on the disk.
                            static ULONGLONG status_at=0;const ULONGLONG status_now=GetTickCount64();
                            if(live_status.Enabled() && (o.window_frames || status_now-status_at>=100)){
                                status_at=status_now;
                                auto word=[&](uint32_t address){return m.ram.at((address-0x01000000u)/16);};
                                auto wide=[&](uint32_t address){return uint32_t(word(address))|(uint32_t(word(address+16))<<16);};
                                auto life=[&](unsigned player){return std::max(0,int(int16_t(word(player?0x010515e0:0x01051300)))-int(int16_t(word(player?0x010515f0:0x01051310))));};
                                auto coordinate=[&](unsigned player){const auto actor=wide(player?0x01051510:0x01051230);return actor>=0x01000000u&&actor+0x300<0x01400000u&&!(actor&15)?std::to_string(int16_t(word(actor+0x90))):std::string("null");};
                                std::ostringstream status;status<<"{\"scanout_frame\":"<<screen->frames()<<",\"presented_before\":"<<window->frames()<<",\"reference_cycles\":"<<s.reference_cycles<<",\"mode\":"<<wide(0x01051190)<<",\"p1\":"<<wide(0x01051200)<<",\"p2\":"<<wide(0x010514e0)<<",\"fighter1\":"<<word(0x01051290)<<",\"fighter2\":"<<word(0x01051570)<<",\"life1\":"<<life(0)<<",\"life2\":"<<life(1)<<",\"round_wins1\":"<<word(0x01051330)<<",\"round_wins2\":"<<word(0x01051610)<<",\"winner\":"<<word(0x010519e0)<<",\"tower_slot\":"<<word(0x01051a10)<<",\"tower_table\":"<<wide(0x010519f0)<<",\"reserve\":"<<word(0x01051a50)<<",\"in01\":"<<o.body.active_di->in01<<",\"credits\":"<<unsigned(uint8_t(f.cmos.at(270)))<<"}\n";
                                auto metrics=status.str();metrics.resize(metrics.size()-2);metrics+=",\"x1\":"+coordinate(0)+",\"x2\":"+coordinate(1)+",\"presentation\":"+window->presentation_json()+"}\n";live_status.Write(metrics);
                            }
                            if(!run_ahead || !window->RunAhead()){
                                present();
                                shown_frame=screen->frames();if(o.window_frames && window->frames()>=o.window_frames){window_stop="window-frame-limit";break;}
                            }
                        }
                        if(window){const auto trim=runner.CompactDiagnostics();discarded.writes+=trim.writes;discarded.returns+=trim.returns;discarded.cmos+=trim.cmos;discarded.irqs+=trim.irqs;discarded.scanlines+=trim.scanlines;discarded.dma+=trim.dma;discarded.completions+=trim.completions;mk5audio::TrimGameAudioDiagnostics();
                            const bool boundary=host.audio && host.gain && screen && (main.stop==TaskBodyStop::Budget || main.stop==TaskBodyStop::DiLimit);
                            if(boundary)for(const auto request:window->TakeStateRequests())state_request(request);
                            if(run_ahead && window->RunAhead() && boundary && screen->frames()>shown_frame){
                                // The next frame with the same live inputs: shown, never heard, then undone at a boundary.
                                const auto fields=[&](auto& a){a(runner,*screen,*host.audio,*host.gain);};
                                const auto real=screen->frames();const auto inputs=o.body.active_di;bool closed=false;
                                ahead.Save(fields);mk5audio::SetGameAudioSpeculative(true);mk5hitbox::strikes.clear();   // the shown frame's own
                                while(screen->frames()==real){
                                    if(!window->Pump()){closed=true;break;}
                                    o.body.active_di=window->Inputs(*active_base);
                                    const auto next=runner.ContinueMain(o.body,o.main_di_limit);
                                    if(next.stop!=TaskBodyStop::Budget && next.stop!=TaskBodyStop::DiLimit)break; // the real run meets it
                                }
                                if(!closed && screen->frames()>real)present();
                                runner.CompactDiagnostics();mk5audio::TrimGameAudioDiagnostics();
                                ahead.Load(fields);mk5audio::SetGameAudioSpeculative(false);o.body.active_di=inputs;
                                shown_frame=real;
                                if(closed){window_stop="window-closed";break;}
                                if(o.window_frames && window->frames()>=o.window_frames){window_stop="window-frame-limit";break;}
                            }}
                        if(main.stop!=TaskBodyStop::Budget && !(o.decompressor && main.stop==TaskBodyStop::DiLimit))break;
                    }
                    switch(main.stop){
                        case TaskBodyStop::DecompressorComplete:stop="decompressor-complete";break;
                        case TaskBodyStop::DiLimit:stop="main-di-limit";break;
                        case TaskBodyStop::Budget:stop="instruction-budget";break;
                        case TaskBodyStop::IrqHandler:stop="irq-handler-frontier";break;
                        case TaskBodyStop::TemporalBarrier:stop="temporal-barrier";break;
                        case TaskBodyStop::DswInput:stop="fresh-dsw-input";break;
                        case TaskBodyStop::SoundInput:stop="fresh-sound-input";break;
                        case TaskBodyStop::DiInput:stop="fresh-active-di-input";break;
                        default:stop="untranslated-frontier";
                    }
                    const auto& v=runner.Video();dma_commands=discarded.dma+v.commands.size();dma_completions=discarded.completions+v.completions.size();drawn_pixels=v.pixels_written;shift_loads=v.shift_loads;shift_stores=v.shift_stores;
                    if(!window_stop.empty())stop=window_stop;
                    if(window){window_frames=window->frames();input_events=window->input_events();window_presented=window_frames!=0;window_presentation=window->presentation_json();}
                    // As MK2 and MK3: the NVRAM is written only after the shared D3D12 presenter showed this session
                    // (mk5_profile_presentation.hpp), checked while the window lives; otherwise the profile keeps its image.
                    if(window && !o.cmos_output.empty() && (window_stop=="window-closed" || window_stop=="window-frame-limit")){
                        const bool closed=window_stop=="window-closed";
                        try{
                            const auto confirm_window=[&]{
                                const bool running=window->Pump();
                                window->Pump();   // surface a failure published around the running/close observation
                                if(!running && !closed)throw std::runtime_error("native profile window closed before successful completion");
                            };
                            confirm_window();
                            ValidateProfilePresentation(window->presentation_json(),closed,window->GraphicsValidationEnabled());
                            confirm_window();   // again after the metrics snapshot, before the durable write
                            cmos_presented=true;
                        }catch(const std::exception& e){cmos_refused=e.what();}
                    }
                    if(screen){scanout_frames=screen->frames();scanout_quanta=screen->last_frame_quanta();scanout_width=screen->width();scanout_height=screen->height();if(!o.scanout_ppm.empty())screen->SavePpm(o.scanout_ppm);}
                }
            }
            // new_cycles keeps its meaning: CPU cycles since the declared raster epoch (first EINT).
            if(irq.Started())before=s.reference_cycles-irq.elapsed_quanta()/16;
            entries=irq.entries();elapsed=irq.elapsed_quanta();scanlines=discarded.scanlines+irq.scanlines().size();io=irq.io();
        }
        if(!o.cmos_output.empty() && cmos_presented && (stop=="window-closed" || stop=="window-frame-limit")){SaveMk5CmosSnapshot(o.cmos_output,f.cmos);cmos_saved=true;}
        else if(!o.cmos_output.empty() && cmos_refused.empty())cmos_refused="the session did not end at the window (stop: "+stop+")";
        if(!cmos_refused.empty())std::cerr<<"MK1 CMOS not saved: "<<cmos_refused<<'\n';
        std::cout<<"{\"scope\":\""<<(o.visual?"mk5-visual":o.decompressor?"mk5-decompressor":o.main_loop?"mk5-main-loop":o.task_body?"mk5-task-body":"mk5-boot-flow")<<"\",\"program_sha256\":\""<<p.sha256<<"\",\"stop\":\""<<stop<<"\",\"next_original_pc\":"<<s.next_original_pc<<",\"a\":";array(s.a);
        std::cout<<",\"status\":"<<s.status<<",\"reference_model_cycles\":"<<s.reference_cycles<<",\"new_cycles\":"<<s.reference_cycles-before
            <<",\"epoch\":{\"origin\":\"explicit-host-snapshot-at-first-eint\",\"initial_pending\":"<<o.pending<<",\"next_scanline\":"<<o.next_line<<",\"quanta_to_next\":"<<o.phase<<",\"quantum_ns\":10,\"initial_dpyadr\":\"prefix-DPYSTRT\"},\"elapsed_quanta\":"<<elapsed<<",\"scanline_events\":"<<scanlines<<",\"io\":[";
        for(size_t i=0;i<io.size();++i){if(i)std::cout<<',';std::cout<<io[i];}
        std::cout<<"],\"irq_entries\":[";
        for(size_t i=0;i<entries.size();++i){if(i)std::cout<<',';const auto& e=entries[i];std::cout<<"{\"vector\":"<<e.vector_bit<<",\"target\":"<<e.target_pc<<",\"return_pc\":"<<e.return_pc<<",\"saved_status\":"<<e.saved_status<<",\"at_quanta\":"<<e.at_quanta<<'}';}
        std::cout<<"]";
        if(o.main_loop){
            auto ram32=[&](uint32_t a){size_t i=(a-0x01000000)/16;return m.ram.at(i)|(uint32_t(m.ram.at(i+1))<<16);};
            std::cout<<",\"main\":{\"executed\":"<<(main_executed?"true":"false")<<",\"calls\":"<<main_calls<<",\"cycles\":"<<main_cycles<<",\"instructions\":"<<main_instructions<<",\"yields\":"<<main_yields<<",\"loops\":"<<main_loops<<",\"di_limit\":"<<o.main_di_limit<<",\"saved_task_pc\":"<<ram32(0x01053470)<<",\"saved_a12\":"<<ram32(0x010533d0)<<",\"sleep\":"<<m.ram.at((0x010533c0-0x01000000)/16)<<",\"external_continuation\":"<<ram32(0x01053910)<<",\"glyph_head\":"<<ram32(0x0104fa10)<<",\"dma_commands\":"<<dma_commands<<",\"dma_completions\":"<<dma_completions<<",\"drawn_pixels\":"<<drawn_pixels<<",\"shift_loads\":"<<shift_loads<<",\"shift_stores\":"<<shift_stores<<",\"b\":";array(s.b);std::cout<<'}';
        }
        if(o.task_body)std::cout<<",\"body\":{\"executed\":"<<(body_executed?"true":"false")<<",\"cycles\":"<<body.reference_cycles<<",\"instructions\":"<<body.instruction_count<<",\"yield_count\":"<<body.yields<<",\"main_loops\":"<<body.main_loops<<",\"raw_dsw\":"<<body.dsw_read.raw<<",\"normalized_dsw\":"<<body.normalized_dsw<<",\"quiet_handlers\":"<<body.quiet_handlers<<",\"normal_boot_complete\":"<<(body_executed && body.stop==TaskBodyStop::MainLoop?"true":"false")<<",\"input_contract\":\"fresh-explicit-frozen-host-samples-for-this-call\"}";
        if(o.visual){
            auto word=[&](uint32_t a){return m.ram.at((a-0x01000000)/16);};
            auto raw_stack_words=uint32_t(word(0x010fffd0))|(uint32_t(word(0x010fffe0))<<16);
            std::cout<<",\"coin_start\":{\"credits\":"<<unsigned(uint8_t(f.cmos.at(270)))<<",\"mode\":"<<word(0x01051190)<<",\"p1\":"<<word(0x01051200)<<",\"p2\":"<<word(0x010514e0)<<",\"p1_character\":"<<word(0x01051290)<<",\"p2_character\":"<<word(0x01051570)<<",\"raw_stack_010fffd0\":"<<raw_stack_words<<'}';
        }
        if(o.visual)std::cout<<",\"window\":{\"frames\":"<<window_frames<<",\"input_events\":"<<input_events<<",\"controls_connected\":true,\"presentation\":"<<window_presentation<<"}";
        if(!o.cmos_output.empty()){
            std::cout<<",\"cmos_saved\":"<<(cmos_saved?"true":"false");
            if(!cmos_saved){std::cout<<",\"cmos_refused\":\"";for(const char c:cmos_refused)std::cout<<(c=='"'||c=='\\'?'\'':c);std::cout<<'"';}
        }
        if(o.visual)std::cout<<",\"diagnostic_history\":{\"compacted\":true,\"discarded_writes\":"<<discarded.writes<<",\"discarded_returns\":"<<discarded.returns<<",\"discarded_cmos_records\":"<<discarded.cmos<<",\"discarded_irq_entries\":"<<discarded.irqs<<",\"discarded_scanlines\":"<<discarded.scanlines<<",\"retained_irq_entries\":"<<entries.size()<<",\"scope\":\"host history only; live CPU/memory/devices unchanged\"}";
        if(o.decompressor){
            std::vector<uint8_t> live_cmos;for(auto word:f.cmos){live_cmos.push_back(uint8_t(word));live_cmos.push_back(uint8_t(word>>8));}
            std::cout<<",\"scanout\":{\"frames\":"<<scanout_frames<<",\"width\":"<<scanout_width<<",\"height\":"<<scanout_height<<",\"last_frame_quanta\":"<<scanout_quanta<<"},\"live_cmos_sha256\":\""<<Sha256(live_cmos)<<'"';
        }
        std::cout<<",\"boot\":{\"cycles\":"<<boot.reference_cycles<<",\"instructions\":"<<boot.instruction_count<<",\"source\":\"generated-power-on-graph\"},\"di\":{\"completed_handlers\":"<<di.completed_handlers<<"},\"scheduler_tick\":"<<m.ram.at((0x01053360-0x01000000)/16)<<",\"consumed_tick\":"<<m.ram.at((0x01053370-0x01000000)/16)<<",\"handler_tick_counter\":"<<m.ram.at((0x010511f0-0x01000000)/16)<<",\"handler_executed\":"<<(di.completed_handlers?"true":"false")<<",\"scanout_executed\":"<<(scanout_frames?"true":"false")<<",\"window_presented\":"<<(window_presented?"true":"false")<<",\"m1_complete\":false,\"m2_visual_test_ready\":false,\"playable\":false}\n";
        return !o.cmos_output.empty()&&!cmos_saved?3:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return error;}
}
