#include "mk2_task_batch.hpp"
#include "mk2_executor.hpp"
#include "mk2_scanout.hpp"
#include "mk2_music.hpp"
#include "mk2_cmos_image.hpp"
#include "mk2_profile.hpp"
#include "mk2_live_host.hpp"
#include "mk2_save_state.hpp"
#include "mk2_replay.hpp"
#include "mk2_valid_cmos_boot.hpp"
#include "mk2_valid_cmos_continuation.hpp"
#include "mk2_valid_cmos_startup.hpp"
#include "mk2_valid_cmos_draw.hpp"
#include "mk5_window.hpp"
#include "mk5_audio_link.hpp"
#include "mk5_hitbox.hpp"
#include "mk5_program.hpp"
#include "mk5_profile_presentation.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <thread>

int wmain(int argc, wchar_t** argv) { mk2::ReplayCapture replay;try {
    bool show_window=false,play_audio=false,write_frames=false,host_controls=false;
    std::wstring through=L"first-draw",frame_path,cmos_path,profile_path;
    bool selected_frontier=false,load_cmos=false,direct_profile=false,selected_dsw=false;
    uint16_t dsw_word=0xfc7d;
    for(int n=2;n<argc;++n) {
        const std::wstring option=argv[n];
        if(option==L"--window" && !show_window) show_window=true;
        else if(option==L"--audio" && !play_audio) play_audio=true;
        else if(option==L"--host-controls" && !host_controls) host_controls=true;
        else if(option==L"--frames" && !write_frames && n+1<argc) {write_frames=true;frame_path=argv[++n];}
        else if(option==L"--through" && !selected_frontier && n+1<argc) {selected_frontier=true;through=argv[++n];}
        else if(option==L"--cmos-image" && !load_cmos && !direct_profile && n+1<argc) {load_cmos=true;cmos_path=argv[++n];}
        else if(option==L"--profile" && !direct_profile && !load_cmos && n+1<argc) {direct_profile=true;profile_path=argv[++n];}
        else if(option==L"--dsw-word" && !selected_dsw && n+1<argc) {
            selected_dsw=true;const std::wstring raw=argv[++n];size_t consumed=0;
            const auto value=std::stoul(raw,&consumed,0);
            if(raw.empty() || raw[0]==L'-' || consumed!=raw.size() || value>0xffff || !mk2::OperatorDswSupported(uint16_t(value)))
                throw std::runtime_error("native operator DIP accepts only original blood/violence switches");
            dsw_word=uint16_t(value);
        }
        else throw std::runtime_error("unknown, duplicate or incomplete native diagnostic option");
    }
    const bool cmos_boot=through==L"cmos-boot";
    const bool cmos_startup=through==L"cmos-startup";
    const bool cmos_draw=through==L"cmos-draw";
    const bool cmos_host=through==L"cmos-host";
    const bool cmos_live=through==L"cmos-live";
    const bool test_switch=through==L"test-switch";
    const bool persisted=cmos_boot||cmos_startup||cmos_draw||cmos_host||cmos_live;
    if(argc<2 || (through!=L"first-draw" && through!=L"next-wait" && through!=L"first-fight" && !persisted && !test_switch) ||
        (play_audio && !show_window) || (write_frames && show_window) ||
        (host_controls && (!show_window || (through!=L"first-fight" && !cmos_host && !cmos_live))) ||
        ((cmos_host||cmos_live) && !host_controls) || (cmos_live && !direct_profile) ||
        (load_cmos||direct_profile)!=(persisted||test_switch) || (selected_dsw&&!persisted) ||
        (test_switch && (!show_window || !load_cmos)))
        throw std::runtime_error("usage: mk2-native-check ROM_DIRECTORY [--window [--audio] [--host-controls] | --frames FILE] [--through first-draw|next-wait|first-fight|cmos-boot|cmos-startup|cmos-draw|cmos-host|cmos-live|test-switch] [--cmos-image FILE | --profile EXISTING_DIRECTORY]; Test Switch requires --window and the factory --cmos-image");
    mk2::CmosProfileSnapshot loaded_profile;
    std::vector<uint8_t> cmos_image;
    if(direct_profile) {
        loaded_profile=mk2::LoadCmosProfile(std::filesystem::path(profile_path));
        cmos_image=loaded_profile.image;
    }
    std::ofstream frames;
    if (write_frames) {
        frames.open(std::filesystem::path(frame_path),std::ios::binary);
        if (!frames) throw std::runtime_error("cannot open transient raster output");
    }
    const auto program=mk2::LoadProgram(argv[1]);
    if(load_cmos) {
        std::ifstream input(std::filesystem::path(cmos_path),std::ios::binary);
        if(!input)throw std::runtime_error("cannot open exact session CMOS image");
        cmos_image.resize(16385);input.read(reinterpret_cast<char*>(cmos_image.data()),cmos_image.size());
        cmos_image.resize(size_t(input.gcount()));
        mk2::DecodeCmosImage(cmos_image);
        if(test_switch && Sha256(cmos_image)!=mk2::kTestSwitchFactorySha256)
            throw std::runtime_error("MK2 Test Switch requires the authenticated factory CMOS image");
    }
    if(cmos_live)replay.Start(Sha256(program.bytes),Sha256(cmos_image),cmos_image);
    const auto rom=mk2::LoadDcsDataRoms(argv[1]);
    const auto boot=mk2::DecodeDcsBoot({rom.begin(),rom.begin()+0x1000});
    auto machine=std::make_unique<mk2::DcsBootMachine>(boot,rom);
    // Presentation consumes current state, without retaining instruction histories.
    machine->dsp.SetTraceObservation(false);
    const char* music_mode=std::getenv("MK1_MUSIC_MODE");
    const bool remastered=music_mode && std::string(music_mode)=="remastered";
    if(music_mode && !remastered && std::string(music_mode)!="original")throw std::runtime_error("invalid MK2 music mode");
    // Widescreen (MK2_WIDESCREEN=1, Remastered window only): the shadow pass's 16:9 frame (mk5_wide.hpp) is shown.
    const char* widescreen_flag=std::getenv("MK2_WIDESCREEN");
    const bool widescreen=remastered&&show_window&&!test_switch&&widescreen_flag&&std::string(widescreen_flag)=="1";
    std::unique_ptr<Mk5Window> window;
    if (show_window) window=widescreen?std::make_unique<Mk5Window>(cmos_live&&!remastered,mk5wide::kAcross,mk5wide::kDown):std::make_unique<Mk5Window>(cmos_live&&!remastered);
    mk5hitbox::enabled=window&&window->Training();   // training mode (F8): boxes the strike check tests
    bool stop_requested=false,valid_host_admitted=false,live_boundary_validated=false;
    // Run-ahead (F6): the real frames are held; the frame after each one, run from a snapshot, is shown, then undone.
    bool hold_frames=false,speculating=false;
    uint64_t raster_tick=0; // the native tick of the last complete raster
    uint32_t last_host_input=0xffffffffu;
    uint64_t provider_requests=0,last_host_input_tick=0,stop_observed_tick=0;
    uint64_t service_first_tick=0;
    auto pump_host=[&](uint64_t tick) {
        if(!stop_requested && !window->Pump()) {
            // Recheck a driver failure published between Pump's failure check and running read.
            window->Pump();
            stop_requested=true;stop_observed_tick=tick;
        }
        return !stop_requested;
    };
    std::unique_ptr<mk5audio::WaveOutSink> sink;
    std::unique_ptr<mk2::OriginalAudio> audio;
    std::unique_ptr<mk2::RemasteredAudio> music;
    if (play_audio) {
        if(remastered){
            const wchar_t* directory=_wgetenv(L"MK1_MUSIC_DIRECTORY");
            if(!directory || !*directory)throw std::runtime_error("MK2 remastered music directory missing");
            music=std::make_unique<mk2::RemasteredAudio>(machine->dsp,std::filesystem::path(directory),[&](const int16_t*p,size_t n){if(!speculating)sink->Push(p,n);});
        }
        // 10 ms packets: shorter completion waits on the guest thread, and the 60 ms reserve underruns with 20 ms ones
        // (audio_latency_probe).
        sink=std::make_unique<mk5audio::WaveOutSink>(remastered?2:1,mk5audio::WaveOutPacket::Ms10);
        // The frame pacer holds the queue at its reserve (mk5_timing.hpp) instead of rebasing late frames, which drains it.
        mk5audio::detail::hooks.output_clock=true;
        if(!remastered)audio=std::make_unique<mk2::OriginalAudio>([&](const int16_t* samples,size_t count) {if(!speculating)sink->Push(samples,count);});
        machine->dsp.SetDacObserver([&](uint64_t first,const float* samples,size_t count) {if(music)music->Feed(first,samples,count);else audio->Feed(first,samples,count);});
    }
    const auto source_samples=[&]{return music?music->SourceSamples():audio->SourceSamples();};
    const auto output_samples=[&]{return music?music->OutputSamples():audio->OutputSamples();};
    mk2::Scanout scanout(remastered && show_window);
    // Widescreen W0/W1 debug view (MK2_WIDE=1): per frame, FNV-1a of the 512 columns (the shadow pass's wide layer), of
    // their visible centre and of the scanout's own frame, and the shadow passes left incomplete so far;
    // MK2_WIDE_DUMP=<frame,...> adds those frames' 512 columns as hex RGB.
    const bool wide_debug=std::getenv("MK2_WIDE")!=nullptr,wide=wide_debug||widescreen;std::set<uint64_t> wide_dump;
    if(widescreen)scanout.EnableWidePresentation();
    if(wide_debug){
        scanout.EnableWide();
        if(const char* list=std::getenv("MK2_WIDE_DUMP"))for(const char* c=list;*c;){char* end=nullptr;wide_dump.insert(std::strtoull(c,&end,10));c=*end?end+1:end;}
    }
    // One slot per program ROM and edition: in memory, mirrored to saves/mk2/<program sha>/ for later sessions.
    const auto slot_path=std::filesystem::path(L"saves/mk2")/program.sha256/(remastered?L"slot1-remastered.state":L"slot1.state");
    std::vector<uint8_t> slot;
    int64_t state_output_offset=0; // Output samples a load rewound or skipped; the sink still counts every one.
    const auto state_request=[&](Mk5Window::StateRequest request,mk2::LiveHostContinuation& continuation) {
        const mk2::LiveAudio live_audio{audio.get(),music.get()};
        if(request==Mk5Window::StateRequest::Save) {
            slot=mk2::SaveLiveState(program,continuation,*machine,scanout,live_audio);
            try {
                std::filesystem::create_directories(slot_path.parent_path());
                auto pending=slot_path;pending+=L".tmp";
                std::ofstream file(pending,std::ios::binary|std::ios::trunc);
                file.write(reinterpret_cast<const char*>(slot.data()),std::streamsize(slot.size()));file.close();
                if(!file || !MoveFileExW(pending.c_str(),slot_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("cannot write "+slot_path.string());
                std::cerr<<"MK2 state saved: "<<slot_path.generic_string()<<" ("<<slot.size()<<" bytes)\n";
            } catch(const std::exception& error) {std::cerr<<"MK2 save state kept in memory only: "<<error.what()<<'\n';}
            return;
        }
        auto bytes=slot;
        try {
            if(bytes.empty()) {
                std::ifstream file(slot_path,std::ios::binary);
                if(!file)throw std::runtime_error("no saved state");
                bytes.assign(std::istreambuf_iterator<char>(file),{});
            }
            mk2::state::CheckLiveState(program,bytes,mk2::state::LiveIdentity(continuation,scanout,live_audio));
        } catch(const std::exception& error) {std::cerr<<"MK2 save state not loaded: "<<error.what()<<'\n';return;}
        const auto before=sink?output_samples():0;
        mk2::LoadLiveState(program,bytes,continuation,*machine,scanout,live_audio);
        if(sink)state_output_offset+=int64_t(before)-int64_t(output_samples());
        replay.End();
        std::cerr<<"MK2 state loaded from "<<(slot.empty()?slot_path.generic_string():std::string("memory"))<<'\n';
    };
    std::vector<std::string> hashes;
    const auto raster_bytes=[&] {
        const auto& pixels=scanout.pixels();
        std::vector<uint8_t> bytes(pixels.size()*sizeof(uint32_t));
        std::memcpy(bytes.data(),pixels.data(),bytes.size());
        return bytes;
    };
    machine->SetScanlineObserver([&](const auto& edge,const auto& live_video) {
        const auto before=scanout.frames();scanout.Line(edge,live_video);
        if (scanout.frames()==before) return;
        if(wide_debug){
            const auto& w=scanout.wide_pixels();const unsigned n=mk2::Scanout::kWideWidth,width=scanout.width(),margin=(n-width)/2;
            uint64_t all=0xcbf29ce484222325ull,centre=all,plain=all;
            for(size_t i=0;i<w.size();++i){all=(all^w[i])*0x100000001b3ull;if(i%n>=margin&&i%n<margin+width)centre=(centre^w[i])*0x100000001b3ull;}
            for(const uint32_t v:scanout.pixels())plain=(plain^v)*0x100000001b3ull;
            std::printf("{\"wide\":%llu,\"all\":\"%016llx\",\"centre\":\"%016llx\",\"plain\":\"%016llx\",\"faults\":%llu,\"added\":%llu,\"centre_diff\":%llu,\"sentinel\":%llu,\"masked\":%llu,\"outside\":%llu,\"generated\":%llu,\"fault_pc\":%u,\"streamer_skips\":%llu,\"streamer\":%d,\"whole\":%d",(unsigned long long)scanout.frames(),
                        (unsigned long long)all,(unsigned long long)centre,(unsigned long long)plain,(unsigned long long)live_video.margin_faults,(unsigned long long)scanout.wide_added(),(unsigned long long)scanout.wide_centre(),(unsigned long long)scanout.wide_audit().sentinel,(unsigned long long)scanout.wide_audit().masked,(unsigned long long)scanout.wide_audit().outside,(unsigned long long)scanout.wide_audit().generated,unsigned(live_video.margin_fault_pc),(unsigned long long)live_video.margin_streamer_skips,int(live_video.margin_streamer),int(scanout.wide_whole()));
            if(wide_dump.count(scanout.frames())){std::printf(",\"rgb\":\"");for(const uint32_t v:w)std::printf("%06x",unsigned(v&0xffffff));std::printf("\"");}
            std::printf("}\n");
        }
        raster_tick=94875+uint64_t(edge.scanline_events-1)*6325;
        if(!cmos_live&&!test_switch) {
            const auto bytes=raster_bytes();
            hashes.push_back(Sha256(bytes));
            if (frames.is_open()) {
                const uint64_t tick=94875+uint64_t(edge.scanline_events-1)*6325;
                const uint32_t width=scanout.width(),height=scanout.height();
                frames.write(reinterpret_cast<const char*>(&tick),sizeof(tick));
                frames.write(reinterpret_cast<const char*>(&width),sizeof(width));
                frames.write(reinterpret_cast<const char*>(&height),sizeof(height));
                frames.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                if (!frames) throw std::runtime_error("transient raster write failed");
            }
        }
        if (window) {
            if(cmos_host||cmos_live||test_switch) {
                if(!pump_host(94875+uint64_t(edge.scanline_events-1)*6325))return;
            } else if (!window->Pump()) throw std::runtime_error("native diagnostic window closed");
            if(widescreen && (!hold_frames || speculating)){
                auto boxes=mk5hitbox::Take();   // the world's boxes move with the window
                const int move=int(mk2::Scanout::kWideWidth-scanout.width())/2;
                for(auto& box:boxes)box.x+=move;
                window->Present(scanout.wide_presentation(),mk2::Scanout::kWideWidth,scanout.height(),
                                scanout.wide_backdrop_pixels(),scanout.wide_fighter_frame(),boxes);
            }else if(!hold_frames || speculating)window->Present(scanout.presentation(),scanout.width(),scanout.height(),
                                                           scanout.backdrop_pixels(),scanout.fighter_frame(),mk5hitbox::Take());
        }
    });
    const auto host_range=host_controls&&!cmos_host&&!cmos_live ? mk2::HostControlRange(0,int(mk2::kFirstFightWindowCount)-2) : mk2::HostControlBounds{};
    const auto valid_host_range=cmos_host ? mk2::ValidHostControlRange() : mk2::ValidHostControlBounds{};
    const auto s=[&]() {
    // Generated closure from the reset vector to the route's delivery boundary (mk2_executor.hpp).
    mk2::GeneratedBoot generated(program,*machine,(load_cmos||direct_profile)?&cmos_image:nullptr,
        test_switch?mk2::kTestSwitchDsw:dsw_word);
    if((remastered && show_window) || wide) generated.OnVideoReady([&](mk2::BootVideo& video) {
        if(wide)video.EnableWide();   // the shadow pass's 512-column frame
        if(!(remastered && show_window))return;
        video.EnableBlood(program);
        if(!test_switch){
            video.EnableBackdrop();
            video.EnableFighters();
        }
    });
    if(test_switch) {
        auto startup=mk2::DeliverAs<mk2::ValidCmosContinuation>(generated);
        return mk2::RunTestSwitch(program,std::move(startup),*machine,[&,pace_start=std::chrono::steady_clock::time_point{}](uint64_t tick) mutable {
            if(!pump_host(tick))return last_host_input;
            if(!service_first_tick) {
                service_first_tick=tick;
                pace_start=std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_until(pace_start+std::chrono::nanoseconds(10*(tick-service_first_tick)));
            ++provider_requests;
            last_host_input=window->Inputs({0xffffffffu,0xffffffffu}).in01;
            return last_host_input;
        },[&]{return !stop_requested;});
    }
    if(cmos_boot)return generated.RunToBoundary(mk2::BootEvent::admit_startup).ready;
    if(cmos_startup||cmos_draw||cmos_host||cmos_live) {
        if(cmos_startup)return generated.RunToBoundary(mk2::BootEvent::admit_draw).ready;
        const auto draw=mk2::DeliverAs<mk2::ValidCmosContinuation>(generated);
        if(!(cmos_host||cmos_live) || !pump_host(draw.ready.cycles*16))return draw.ready;
        if(cmos_live) {
            auto continuation=mk2::RunValidLiveHost(program,draw,*machine,[&](uint64_t tick) {
                ++provider_requests;
                if(pump_host(tick)) {last_host_input=window->Inputs({0xffffffffu,0xffffffffu}).in01;last_host_input_tick=tick;}
                if(!speculating)replay.Observe(tick,last_host_input);
                return last_host_input;
            },draw.ready.cycles+mk2::live_detail::kRequestCycles);
            valid_host_admitted=true;
            // Run-ahead: each chunk ends just after the next raster (one native frame of 1827925 ticks after the last).
            const auto frame_end=[&] {
                const auto now=continuation.State().ready.cycles,next=(raster_tick+1827925)/16+1;
                return next>now?next:now+1827925/16;
            };
            mk2::state::Snapshot ahead;
            while(!stop_requested) {
                hold_frames=window->RunAhead();   // the window stops it when the frame does not fit
                if(!hold_frames) {
                    const auto target=continuation.State().ready.cycles+mk2::live_detail::kRequestCycles;
                    continuation=mk2::ResumeValidLiveHost(program,std::move(continuation),*machine,target);
                    for(const auto request:window->TakeStateRequests())state_request(request,continuation);
                    continue;
                }
                for(const auto real=scanout.frames();!stop_requested && scanout.frames()==real;)
                    continuation=mk2::ResumeValidLiveHost(program,std::move(continuation),*machine,frame_end());
                for(const auto request:window->TakeStateRequests())state_request(request,continuation);
                if(stop_requested)break;
                // The next frame with the same live inputs: shown, never heard nor recorded, then undone.
                const mk2::LiveAudio live_audio{audio.get(),music.get()};
                const auto fields=[&](auto& a){mk2::state::Session(a,continuation,*machine,scanout,live_audio);};
                const auto host=std::make_tuple(raster_tick,last_host_input,last_host_input_tick,provider_requests);
                ahead.Save(fields);speculating=true;mk5hitbox::strikes.clear();   // the shown frame's own
                for(const auto real=scanout.frames();!stop_requested && scanout.frames()==real;)
                    continuation=mk2::ResumeValidLiveHost(program,std::move(continuation),*machine,frame_end());
                ahead.Load(fields);speculating=false;
                std::tie(raster_tick,last_host_input,last_host_input_tick,provider_requests)=host;
            }
            continuation.ValidateBoundary(program,*machine);live_boundary_validated=true;
            return continuation.State().ready;
        }
        auto continuation=std::make_unique<mk2::TaskBatchInit>(mk2::RunValidHostContinuation(
            program,draw,*machine,[&](uint64_t tick) {
                ++provider_requests;
                if(pump_host(tick)) {
                    last_host_input=window->Inputs({0xffffffffu,0xffffffffu}).in01;
                    last_host_input_tick=tick;
                }
                return last_host_input;
            },std::min(draw.ready.cycles+2000000,valid_host_range.maximum_stop_cycle)));
        valid_host_admitted=true;
        while(!stop_requested && continuation->ready.cycles<valid_host_range.maximum_stop_cycle) {
            const auto stop=std::min(continuation->ready.cycles+2000000,valid_host_range.maximum_stop_cycle);
            continuation=std::make_unique<mk2::TaskBatchInit>(
                mk2::ResumeValidHostContinuation(program,*continuation,*machine,stop));
        }
        return continuation->ready;
    }
    const auto& draw=generated.Deliver();
    std::unique_ptr<mk2::TaskBatchInit> continuation;
    if(through!=L"first-draw") continuation=std::make_unique<mk2::TaskBatchInit>(
        mk2::RunTranslatedContinuation(program,draw,*machine,
            through==L"first-fight" ? (host_controls ? int(host_range.start_window) : int(mk2::kFirstFightWindowCount)-1) : -1));
    if(host_controls) continuation=std::make_unique<mk2::TaskBatchInit>(
        mk2::RunHostContinuation(program,*continuation,*machine,[&](uint64_t) {
            if(!window->Pump())throw std::runtime_error("native host-control window closed");
            return window->Inputs({0xffffffffu,0xffffffffu}).in01;
            },host_range.start_window,1));
    if(host_controls) {
        while(continuation->ready.cycles<host_range.minimum_stop_cycle) {
            const auto stop=std::min(continuation->ready.cycles+2000000,host_range.minimum_stop_cycle);
            continuation=std::make_unique<mk2::TaskBatchInit>(
                mk2::ResumeHostContinuation(program,*continuation,*machine,stop));
        }
    }
    return continuation ? continuation->ready : draw.ready;
    }();
    machine->SetScanlineObserver({});
    const auto live_last_frame_sha256=(cmos_live||test_switch)&&scanout.frames()?Sha256(raster_bytes()):std::string{};
    machine->dsp.SetDacObserver({});
    if (sink) sink->Drain();
    if (frames.is_open()) {
        frames.close();
        if (!frames) throw std::runtime_error("transient raster close failed");
    }
    if(test_switch) {
        if(!stop_requested || !machine->TestSwitchAdmitted() || machine->LastMainCycles()!=s.cycles ||
           machine->ScanlineEvents()!=s.board.scanline_events || s.board.HostInputProviderActive())
            throw std::runtime_error("MK2 Test Switch requires synchronized state at window close");
        std::cout<<"{\"scope\":\"mk2-native-test-switch\",\"termination\":\"window_closed\",\"operator_dsw_word\":"
                 <<s.board.dip_switches<<",\"cpu34\":["<<s.next_pc<<','<<s.status<<','<<s.cycles;
        for(auto value:s.a)std::cout<<','<<value;
        for(auto value:s.b)std::cout<<','<<value;
        std::cout<<"],\"frames\":"<<scanout.frames()<<",\"last_frame_sha256\":\""<<live_last_frame_sha256
                 <<"\",\"first_input_tick\":"<<service_first_tick<<",\"input_samples\":"<<provider_requests
                 <<",\"presentation\":"<<window->presentation_json()<<"}\n";
        return 0;
    }
    if ((cmos_live ? (!stop_requested || (valid_host_admitted ?
                        !machine->ValidCmosLiveHostAdmitted() || !live_boundary_validated : !mk2::ValidCmosDrawBoundary(s))) :
         cmos_host ? (valid_host_admitted ? s.next_pc!=0xff80dd30 || s.cycles<=valid_host_range.start_cycle ||
                        s.cycles>valid_host_range.maximum_cycle ||
                        (!stop_requested && s.cycles<valid_host_range.maximum_stop_cycle) :
                        !stop_requested || !mk2::ValidCmosDrawBoundary(s)) :
         cmos_draw ? !mk2::ValidCmosDrawBoundary(s) : cmos_startup ? !mk2::ValidCmosStartupBoundary(s,5) : cmos_boot ? s.next_pc!=0xff811340 || s.cycles!=mk2::OperatorCycle(dsw_word,mk2::OperatorBoundary::Reset) : s.next_pc!=0xff80dd30) || (through==L"first-draw" && s.cycles!=1261260) ||
        scanout.width()!=400 || scanout.height()!=254 || scanout.frames()!=s.cycles*16/1827925)
        throw std::runtime_error("native first-draw CPU or complete-raster boundary differs");
    if (window && !((cmos_host||cmos_live)&&stop_requested)) {
        window->Frontier(s.next_pc);
        // Scalar GPU validation runs on the UI thread; wait for its completion, not a fixed delay.
        const auto deadline=GetTickCount64()+10000;
        while (((cmos_host||cmos_live)?pump_host(s.cycles*16):window->Pump()) && GetTickCount64()<deadline) {
            const auto metrics=window->presentation_json();
            if ((!window->GraphicsValidationEnabled() || ProfilePresentationValue<uint64_t>(metrics,"gpu_validation_frames")) &&
                ProfilePresentationValue<uint64_t>(metrics,"displayed_frames")) break;
            Sleep(10);
        }
    }
    std::string saved_profile_sha256,profile_presentation;
    if(direct_profile) {
        if(machine->LastMainCycles()!=s.cycles || machine->ScanlineEvents()!=s.board.scanline_events ||
           (cmos_boot && !mk2::ValidCmosStartupBoundary(s,0)) ||
           ((cmos_host||cmos_live) && valid_host_admitted &&
                ((cmos_live ? !machine->ValidCmosLiveHostAdmitted() : !machine->ValidCmosHostAdmitted()) || !s.board.HostInputProviderActive())))
            throw std::runtime_error("native profile requires the synchronized genuine CPU boundary");
        if(sink && window)   // the session's timing, whatever the checks below decide
            {std::cout.flush();std::clog<<"{\"session_presentation\":"<<window->presentation_json()<<",\"underruns\":"<<sink->Underruns()<<",\"max_producer_gap_ms\":"<<sink->MaxProducerGapMs()<<"}"<<std::endl;}
        if(sink) {
            const auto blocks=s.cycles*16/2000000;
            if(source_samples()<blocks*625+1 || output_samples()<blocks*960+1 ||
               sink->Submitted()!=uint64_t(int64_t(output_samples())+state_output_offset) || sink->Completed()!=sink->Submitted() || sink->Underruns())
                throw std::runtime_error("native profile Windows PCM delivery failed or is incomplete");
        }
        if(window) {
            const bool clean_host_stop=(cmos_host||cmos_live) && stop_requested;
            const auto confirm_window=[&] {
                const bool running=window->Pump();
                window->Pump(); // Surface a failure published around the running/close observation.
                if(!running && !clean_host_stop)throw std::runtime_error("native profile window closed before successful completion");
            };
            confirm_window();profile_presentation=window->presentation_json();
            ValidateProfilePresentation(profile_presentation,clean_host_stop,!cmos_live||window->GraphicsValidationEnabled());
            confirm_window(); // Check again after the metrics snapshot, before any durable write.
        }
        saved_profile_sha256=mk2::CompareAndSwapCmosProfile(std::filesystem::path(profile_path),
            mk2::EncodeCmosImage(s.cmos),loaded_profile.sha256);
    }
    const bool extended=through!=L"first-draw";
    std::cout<<"{\"scope\":\""<<(cmos_live?"mk2-native-cmos-live-experiment":cmos_host?"mk2-native-cmos-host-check":cmos_draw?"mk2-native-cmos-draw-check":cmos_startup?"mk2-native-cmos-startup-check":cmos_boot?"mk2-native-cmos-boot-check":host_controls?"mk2-native-host-controls-check":extended?"mk2-native-translated-continuation-check":"mk2-native-first-draw-check")
             <<"\",\"gameplay_complete\":false,\"through\":\""<<std::string(through.begin(),through.end())
             <<"\",\"termination\":\""<<(cmos_live?"valid_cmos_live_stop_requested":cmos_host?(stop_requested?"finite_valid_cmos_host_stop_requested":"finite_valid_cmos_host_boundary"):cmos_draw?"finite_valid_cmos_draw_complete":cmos_startup?"finite_valid_cmos_startup_complete":cmos_boot?"finite_valid_cmos_boot_complete":host_controls?"finite_host_cooperative_boundary":extended?"translated_frontier_reached":"finite_diagnostic_complete")
             <<"\",\"execution_continues\":false,"
             <<"\"frontier_pc\":"<<s.next_pc<<",\"operator_dsw_word\":"<<s.board.dip_switches<<','
             <<"\"input\":\""<<(cmos_live?"valid-cmos-live-keyboard-xinput":cmos_host?"valid-cmos-host-keyboard-xinput-finite":cmos_draw?"qualified-valid-cmos-draw-prefix":cmos_startup?"qualified-valid-cmos-startup-prefix":cmos_boot?"qualified-valid-cmos-prefix":host_controls?"host-keyboard-xinput-finite":extended?"fixed-measured-route":"qualified-fixed-prefix")
             <<"\",\"audio_playback\":"<<(sink?"true":"false")<<",\"cpu34\":["
             <<s.next_pc<<','<<s.status<<','<<s.cycles;
    for (auto value:s.a) std::cout<<','<<value;
    for (auto value:s.b) std::cout<<','<<value;
    std::cout<<"],\"frames\":"<<scanout.frames()<<",\"width\":"<<scanout.width()
             <<",\"height\":"<<scanout.height()<<",\"frame_sha256\":[";
    for (size_t n=0;n<hashes.size();++n) std::cout<<(n?",":"")<<'"'<<hashes[n]<<'"';
    std::cout<<"],\"host_controls\":";
    if(cmos_live) {
        std::cout<<"{\"origin\":\"valid_cmos_live\",\"overall_cycle_limit\":null,\"chunk_request_cycles\":"<<mk2::live_detail::kRequestCycles
                 <<",\"chunk_deadline\":"<<(valid_host_admitted?machine->LiveHostDeadline():0)
                 <<",\"provider_admitted\":"<<(valid_host_admitted?"true":"false")<<",\"provider_requests\":"<<provider_requests
                 <<",\"stop_requested\":"<<(stop_requested?"true":"false")<<",\"last_input\":"<<last_host_input
                 <<",\"last_input_tick\":"<<last_host_input_tick<<",\"stop_observed_tick\":"<<stop_observed_tick
                 <<",\"bounded_observations\":true,\"input_event_count\":"<<s.board.LiveInputEventCount()
                 <<",\"last_frame_sha256\":\""<<live_last_frame_sha256<<"\",\"events\":[";
        for(size_t n=0;n<s.board.input_events.size();++n) {
            const auto& event=s.board.input_events[n];std::cout<<(n?",":"")<<'['<<event[0]<<','<<event[1]<<','<<event[2]<<','<<event[3]<<']';
        }
        std::cout<<"]}";
    } else if(cmos_host) {
        std::cout<<"{\"origin\":\"valid_cmos_draw\",\"start_cycle\":"<<valid_host_range.start_cycle
                 <<",\"stop_after_cycle\":"<<valid_host_range.maximum_stop_cycle
                 <<",\"maximum_cycle\":"<<valid_host_range.maximum_cycle
                 <<",\"provider_admitted\":"<<(valid_host_admitted?"true":"false")
                 <<",\"provider_requests\":"<<provider_requests
                 <<",\"stop_requested\":"<<(stop_requested?"true":"false")
                 <<",\"last_input\":"<<last_host_input<<",\"last_input_tick\":"<<last_host_input_tick
                 <<",\"stop_observed_tick\":"<<stop_observed_tick<<",\"events\":[";
        for(size_t n=0;n<s.board.input_events.size();++n) {
            const auto& event=s.board.input_events[n];
            std::cout<<(n?",":"")<<'['<<event[0]<<','<<event[1]<<','<<event[2]<<','<<event[3]<<']';
        }
        std::cout<<"]}";
    } else if(host_controls) {
        std::cout<<"{\"start_window\":"<<host_range.start_window
                 <<",\"stop_window\":"<<mk2::kFirstFightWindowCount-2
                 <<",\"start_cycle\":"<<host_range.start_cycle
                 <<",\"minimum_stop_cycle\":"<<host_range.minimum_stop_cycle
                 <<",\"maximum_cycle\":"<<host_range.maximum_cycle<<",\"events\":[";
        for(size_t n=20;n<s.board.input_events.size();++n) {
            const auto& event=s.board.input_events[n];
            std::cout<<(n==20?"":",")<<'['<<event[0]<<','<<event[1]<<','<<event[2]<<','<<event[3]<<']';
        }
        std::cout<<"]}";
    } else std::cout<<"null";
    std::cout<<",\"audio\":";
    if (sink) {
        std::cout<<"{\"source_rate\":31250,\"output_rate\":48000,\"channels\":"<<(music?2:1)<<",\"source_samples\":"<<source_samples()
                 <<",\"output_samples\":"<<output_samples()<<",\"music_mode\":\""<<(music?"remastered":"original")<<'"'
                 <<",\"music_starts\":"<<(music?music->Starts():0)<<",\"music_clipped\":"<<(music?music->Clipped():audio->Clipped())
                 <<",\"music_limited\":"<<(music?music->Limited():audio->Limited())<<",\"submitted\":"<<sink->Submitted()
                 <<",\"completed\":"<<sink->Completed()<<",\"underruns\":"<<sink->Underruns()
                 <<",\"queue_capacity_ms\":"<<sink->QueueCapacityMs()<<",\"startup_reserve_ms\":"<<sink->StartupReserveMs()
                 <<",\"max_producer_gap_ms\":"<<sink->MaxProducerGapMs()<<'}';
    } else std::cout<<"null";
    std::cout<<",\"presentation\":"<<(direct_profile && window ? profile_presentation : window?window->presentation_json():"null");
    if(direct_profile) {
        std::cout<<",\"profile\":{\"loaded_sha256\":\""<<loaded_profile.sha256
                 <<"\",\"saved_sha256\":\""<<saved_profile_sha256<<"\",\"committed\":true}";
    }
    if(persisted && !direct_profile) {
        std::cout<<",\"cmos_hex\":\"";
        constexpr char digits[]="0123456789abcdef";
        for(auto byte:mk2::EncodeCmosImage(s.cmos))std::cout<<digits[byte>>4]<<digits[byte&15];
        std::cout<<'"';
    }
    std::cout<<"}\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';replay.Write(std::cerr);return 1;} }
