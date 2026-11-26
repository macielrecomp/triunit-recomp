#include "mk5_audio_output.hpp"
#include "mk5_audio_volume.hpp"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
namespace mk5audio {
struct WaveOutSink::Impl {
 // Keep reserves in milliseconds so packet granularity does not change the latency policy.
 static constexpr size_t kSamplesPerMs=48,kMaxPacketSamples=20*kSamplesPerMs,kQueueSlots=16;
 const unsigned packet_ms;
 const size_t packet_samples;
 // The balanced reserve after the power-on transient: its target in packets while pending, 0 otherwise.
 static constexpr uint64_t kSettledFrames=5*48000;
 size_t active_slots=0,startup_target=0,policy_target=0,short_target=0;unsigned channels=1,volume=100;
 uint64_t max_producer_gap=0;
 struct Slot{std::array<int16_t,kMaxPacketSamples*2> samples{};WAVEHDR header{};bool prepared=false,inflight=false;};
 HWAVEOUT device=nullptr;HANDLE completion=nullptr;std::array<Slot,kQueueSlots> slots{};size_t cursor=0,filled=0,startup_blocks=0;bool started=false;uint64_t submitted=0,completed=0,underruns=0;ULONGLONG last_push=0;
 static void checked(MMRESULT code){if(code!=MMSYSERR_NOERROR)throw std::runtime_error("Windows PCM output error "+std::to_string(code));}
 explicit Impl(unsigned channel_count,WaveOutPacket packet):packet_ms(unsigned(packet)),packet_samples(packet_ms*kSamplesPerMs),channels(channel_count){
  if(channels!=1&&channels!=2)throw std::invalid_argument("Windows PCM channels must be 1 or 2");
  if(packet_ms!=10&&packet_ms!=20)throw std::invalid_argument("Windows PCM packet must be 10 or 20 ms");
  const auto* configured_volume=std::getenv("MK1_GAME_VOLUME");volume=ParseGameVolume(configured_volume?configured_volume:"");
  const char* policy=std::getenv("MK1_AUDIO_LATENCY");
  unsigned reserve_ms=140;
  if(policy&&std::strcmp(policy,"low")==0)reserve_ms=40;
  else if(policy&&std::strcmp(policy,"balanced")==0){
   const char* music=std::getenv("MK1_MUSIC_MODE");
   // The real remastered workload exhausted the 80ms reserve. Keep the
   // original policy intact, with a bounded 140ms floor for its stereo mixer.
   if(channels!=2||!music||std::strcmp(music,"remastered")!=0)reserve_ms=80;
   // The reserve is the latency (audio_latency_probe: push to the Windows mix = reserve + 4..10 ms). The reserve above
   // covers power-on, where the DCS delivers its first sound 62 ms late; after it the target is 60 ms and the frame
   // pacer (mk5_timing.hpp) bleeds the excess, no sample dropped. Any underrun keeps or restores the reserve above.
   short_target=60/packet_ms;
  }
  else if(policy&&std::strcmp(policy,"safe")!=0)throw std::runtime_error("MK1_AUDIO_LATENCY must be low, balanced or safe");
  startup_target=policy_target=reserve_ms/packet_ms;
  active_slots=(reserve_ms+20)/packet_ms;
  WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=WORD(channels);format.nSamplesPerSec=48000;format.wBitsPerSample=16;format.nBlockAlign=WORD(2*channels);format.nAvgBytesPerSec=96000*channels;
  completion=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completion)throw std::runtime_error("Windows PCM completion event failed");
  try{checked(waveOutOpen(&device,WAVE_MAPPER,&format,reinterpret_cast<DWORD_PTR>(completion),0,CALLBACK_EVENT));checked(waveOutPause(device));
   for(size_t i=0;i<active_slots;++i){auto& slot=slots[i];slot.header.lpData=reinterpret_cast<char*>(slot.samples.data());slot.header.dwBufferLength=DWORD(packet_samples*channels*2);checked(waveOutPrepareHeader(device,&slot.header,sizeof(WAVEHDR)));slot.prepared=true;}}
  catch(...){cleanup();throw;}
 }
 void poll(){for(size_t i=0;i<active_slots;++i){auto& slot=slots[i];if(slot.inflight && (slot.header.dwFlags&WHDR_DONE)){completed+=slot.header.dwBufferLength/2/channels;slot.inflight=false;}}}
 void wait(Slot& slot){ULONGLONG due=GetTickCount64()+2000;while(slot.inflight){poll();if(!slot.inflight)break;auto now=GetTickCount64();if(now>=due || WaitForSingleObject(completion,DWORD(due-now))!=WAIT_OBJECT_0)throw std::runtime_error("Windows PCM queue did not complete");}}
 void start(){if(!started && submitted){checked(waveOutRestart(device));started=true;}}
 void submit(){
  auto& slot=slots[cursor];slot.header.dwBufferLength=DWORD(filled*2);checked(waveOutWrite(device,&slot.header,sizeof(WAVEHDR)));slot.inflight=true;submitted+=filled/channels;filled=0;cursor=(cursor+1)%active_slots;
  if(!started && ++startup_blocks>=startup_target)start();
  poll();
 }
 void push(const int16_t* data,size_t count){
  if(count && !data)throw std::invalid_argument("null PCM buffer");
  if(count%channels)throw std::invalid_argument("partial stereo PCM frame");
  if(count){auto now=GetTickCount64();if(last_push)max_producer_gap=std::max<uint64_t>(max_producer_gap,now-last_push);poll();if(started && completed==submitted){checked(waveOutPause(device));started=false;startup_blocks=0;++underruns;startup_target=policy_target;short_target=0;
    if(underruns<=8)std::clog<<"{\"audio_underrun_sample\":"<<submitted<<",\"producer_gap_ms\":"<<(last_push?now-last_push:0)<<"}\n";}last_push=now;
   if(short_target && submitted>=kSettledFrames){startup_target=short_target;short_target=0;}
   output_reserve_percent=started?int((submitted-completed)*100/(startup_target*packet_samples)):0;}
  while(count){auto& slot=slots[cursor];if(!filled)wait(slot);size_t n=std::min(count,packet_samples*channels-filled);
   if(volume==100)std::memcpy(slot.samples.data()+filled,data,n*2);
   else std::transform(data,data+n,slot.samples.data()+filled,[this](int16_t sample){return ScaleVolume(sample,volume);});
   filled+=n;data+=n;count-=n;if(filled==packet_samples*channels)submit();}
 }
 void drain(){if(filled)submit();start();for(auto& slot:slots)wait(slot);poll();}
 void cleanup(){if(device){waveOutReset(device);for(auto& slot:slots)if(slot.prepared)waveOutUnprepareHeader(device,&slot.header,sizeof(WAVEHDR));waveOutClose(device);device=nullptr;}if(completion){CloseHandle(completion);completion=nullptr;}}
 ~Impl(){cleanup();}
};
WaveOutSink::WaveOutSink(unsigned channels,WaveOutPacket packet):impl(std::make_unique<Impl>(channels,packet)){}WaveOutSink::~WaveOutSink()=default;
void WaveOutSink::Push(const int16_t* data,size_t count){impl->push(data,count);}void WaveOutSink::Drain(){impl->drain();}
uint64_t WaveOutSink::Submitted()const{return impl->submitted;}uint64_t WaveOutSink::Completed()const{return impl->completed;}
uint64_t WaveOutSink::Underruns()const{return impl->underruns;}
unsigned WaveOutSink::QueueCapacityMs()const{return unsigned(impl->active_slots*impl->packet_ms);}
unsigned WaveOutSink::StartupReserveMs()const{return unsigned(impl->startup_target*impl->packet_ms);}
uint64_t WaveOutSink::MaxProducerGapMs()const{return impl->max_producer_gap;}
}
