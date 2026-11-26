#include "mk5_audio.hpp"
#include "mk5_audio_link.hpp"
#include "mk5_audio_output.hpp"
#include "mk5_boot_flow.hpp"
#include "../audio/host_gain.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cstdlib>
namespace {std::vector<uint8_t> read(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("original sound ROM unavailable");return {std::istreambuf_iterator<char>(f),{}};}}
namespace {
// Passive measurements of the exact outgoing guest samples, including the
// transition between separate producer buffers. Large steps can be original
// percussion; these counters never identify a click or alter the PCM.
struct PcmDiagnostics {
 static constexpr uint64_t window_size=96000;
 struct Window{uint64_t start=0,count=0,clipped=0,delta_sample=0;uint32_t peak=0,delta=0;uint16_t command=0;};
 struct Step{uint64_t sample=0;uint32_t delta=0;int16_t before=0,after=0;};
 std::array<Window,60> windows{};size_t window_count=0;std::array<Step,8> steps{};
 uint64_t samples=0,clipped=0,max_delta_sample=0,large_steps=0,window_start=0,window_samples=0,window_clipped=0,window_delta_sample=0;
 uint32_t peak=0,max_delta=0,window_peak=0,window_delta=0;
 int32_t previous=0;bool have_previous=false;uint16_t last_command=0;
 void flush(){
  if(!window_samples)return;
  if(window_count<windows.size())windows[window_count++]={window_start,window_samples,window_clipped,window_delta_sample,window_peak,window_delta,last_command};
  window_start=samples;window_samples=window_clipped=0;window_peak=window_delta=0;window_delta_sample=samples;
 }
 void observe(const int16_t* data,size_t count,const mk5audio::WilliamsAudio& audio){
  if(!audio.Commands().empty())last_command=audio.Commands().back().value;
  for(size_t i=0;i<count;++i){
   const int32_t value=data[i];const uint32_t magnitude=uint32_t(value<0?-value:value);
   peak=std::max(peak,magnitude);window_peak=std::max(window_peak,magnitude);
   if(value==-32768 || value==32767){++clipped;++window_clipped;}
   if(have_previous){const int32_t difference=value-previous;const uint32_t delta=uint32_t(difference<0?-difference:difference);
    if(delta>=8192)++large_steps;
    if(delta>steps.back().delta){size_t at=steps.size()-1;while(at && delta>steps[at-1].delta){steps[at]=steps[at-1];--at;}steps[at]={samples,delta,int16_t(previous),int16_t(value)};}
    if(delta>max_delta){max_delta=delta;max_delta_sample=samples;}
    if(delta>window_delta){window_delta=delta;window_delta_sample=samples;}}
   previous=value;have_previous=true;++samples;++window_samples;
   if(window_samples==window_size)flush();
  }
 }
 void write(std::ostream& out)const{
  out<<",\"pcm_peak_abs\":"<<peak<<",\"pcm_clipped\":"<<clipped<<",\"pcm_max_delta\":"<<max_delta<<",\"pcm_max_delta_sample\":"<<max_delta_sample<<",\"pcm_steps_ge_8192\":"<<large_steps<<",\"pcm_windows_2s_first_120s\":[";
  for(size_t i=0;i<window_count;++i){const auto&w=windows[i];if(i)out<<',';out<<"{\"from_sample\":"<<w.start<<",\"samples\":"<<w.count<<",\"peak_abs\":"<<w.peak<<",\"clipped\":"<<w.clipped<<",\"max_delta\":"<<w.delta<<",\"max_delta_sample\":"<<w.delta_sample<<",\"last_sound_command\":"<<w.command<<'}';}
  out<<"],\"pcm_largest_steps\":[";bool comma=false;for(const auto&s:steps)if(s.delta){if(comma)out<<',';comma=true;out<<"{\"sample\":"<<s.sample<<",\"delta\":"<<s.delta<<",\"before\":"<<s.before<<",\"after\":"<<s.after<<'}';}out<<']';
 }
};
}
int wmain(int argc,wchar_t**argv){try{
 std::filesystem::path roms;
 for(int i=1;i+1<argc;++i)if(std::wstring(argv[i])==L"--roms")roms=argv[i+1];
 // Setup step: the CMOS seed is written by two silent power-ons, without the sound board or an audio device.
 for(int i=1;i+1<argc;++i)if(std::wstring(argv[i])==L"--stage" && std::wstring(argv[i+1])==L"factory-cmos")return Mk5BootFlowCli(argc,argv,false);
 if(roms.empty())throw std::runtime_error("--roms required");
 mk5audio::WilliamsAudio audio(read(roms/"mk1/mks-u3.rom"),read(roms/"mk1/mks-u12.rom"),read(roms/"mk1/mks-u13.rom"));
 const char* mode=std::getenv("MK1_MUSIC_MODE");const bool remastered=mode&&std::string(mode)=="remastered";
 if(mode&&std::string(mode)!="original"&&!remastered)throw std::runtime_error("MK1_MUSIC_MODE must be original or remastered");
 if(remastered){const char* directory=std::getenv("MK1_MUSIC_DIRECTORY");if(!directory||!*directory)throw std::runtime_error("remastered music directory missing");audio.EnableRemasteredMusic(std::filesystem::u8path(directory));}
 // 10 ms packets: the 60 ms reserve underruns with 20 ms ones (audio_latency_probe).
 mk5audio::WaveOutSink output(remastered?2:1,mk5audio::WaveOutPacket::Ms10);mk5audio::GameAudioScope scope(audio);
 PcmDiagnostics pcm_diagnostics;
 // D5: master gain after the mix, before the user volume (WaveOutSink); the canonical PCM above is unchanged.
 mk5audio::HostGain host_gain(remastered?mk5audio::master_db::mk1_remastered:mk5audio::master_db::mk1_original);std::vector<int16_t> gained;
 mk5audio::SetGameAudioConsumer([&](const int16_t* data,size_t count){if(remastered){const auto& p=audio.Pcm();const size_t frames=count/2;pcm_diagnostics.observe(p.data()+p.size()-frames,frames,audio);}else pcm_diagnostics.observe(data,count,audio);gained.resize(count);host_gain.Process(data,gained.data(),count,remastered?2:1);output.Push(gained.data(),count);});
 int result=Mk5BootFlowCli(argc,argv,remastered,{&audio,&host_gain});output.Drain();pcm_diagnostics.flush();
 const auto counts=mk5audio::GameAudioDiagnosticCounts();
 std::cout<<"{\"audio\":\"original-Williams-ADPCM\",\"native_6809\":true,\"command_count\":"<<counts.commands<<",\"pcm_48k_s16_mono\":"<<counts.samples<<",\"submitted\":"<<output.Submitted()<<",\"completed\":"<<output.Completed()<<",\"underruns\":"<<output.Underruns();std::cout<<","<<audio.RemasteredDiagnostics();std::cout<<",\"host_gain_db\":"<<20*std::log10(host_gain.Gain())<<",\"host_limited\":"<<host_gain.Limited()<<",\"host_clipped\":"<<host_gain.Clipped();std::cout<<",\"host_audio_capacity_ms\":"<<output.QueueCapacityMs()<<",\"host_audio_startup_ms\":"<<output.StartupReserveMs()<<",\"max_producer_gap_ms\":"<<output.MaxProducerGapMs();pcm_diagnostics.write(std::cout);std::cout<<",\"sound_frontier\":\""<<audio.Frontier()<<"\",\"physical_status_verified\":false,\"human_listening_verified\":false}\n";
 return result;
}catch(const std::exception&e){std::cerr<<"Original audio: "<<e.what()<<'\n';return 4;}}
