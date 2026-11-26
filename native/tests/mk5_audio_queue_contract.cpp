// Exercise the real sink against asynchronous playback on a synthetic clock.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>
#include <iostream>
#include <stdexcept>
namespace {
struct Playback {WAVEHDR* header;size_t offset=0;};
std::deque<Playback> pending;
std::vector<int16_t> played;
std::vector<DWORD> packets;
uint64_t clock_samples=48000,starved_samples=0;
bool paused=false;
unsigned restarts=0;
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void elapse_samples(uint64_t count){
 clock_samples+=count;
 if(paused)return;
 while(count && !pending.empty()){
  auto& packet=pending.front();const auto size=packet.header->dwBufferLength/2;
  const auto n=std::min<uint64_t>(count,size-packet.offset);
  auto* data=reinterpret_cast<const int16_t*>(packet.header->lpData);
  played.insert(played.end(),data+packet.offset,data+packet.offset+n);
  packet.offset+=size_t(n);count-=n;
  if(packet.offset==size){packet.header->dwFlags|=WHDR_DONE;pending.pop_front();}
 }
 starved_samples+=count;
}
void elapse_ms(unsigned milliseconds){elapse_samples(uint64_t(milliseconds)*48);}
void reset_driver(){
 need(pending.empty(),"previous sink must release every driver packet");
 played.clear();packets.clear();clock_samples=48000;starved_samples=0;paused=false;restarts=0;
}
ULONGLONG fake_ticks(){return clock_samples/48;}
DWORD fake_wait(HANDLE,DWORD timeout){
 if(paused || pending.empty()){elapse_ms(timeout);return WAIT_TIMEOUT;}
 const auto& next=pending.front();const uint64_t remaining=next.header->dwBufferLength/2-next.offset;
 if(remaining>uint64_t(timeout)*48){elapse_ms(timeout);return WAIT_TIMEOUT;}
 elapse_samples(remaining);return WAIT_OBJECT_0;
}
MMRESULT fake_open(LPHWAVEOUT device,UINT_PTR,LPCWAVEFORMATEX format,DWORD_PTR,DWORD_PTR,DWORD){
 if(format->nSamplesPerSec!=48000 || (format->nChannels!=1 && format->nChannels!=2) || format->wBitsPerSample!=16)return WAVERR_BADFORMAT;
 *device=reinterpret_cast<HWAVEOUT>(1);return 0;
}
MMRESULT fake_header(HWAVEOUT,LPWAVEHDR,UINT){return 0;}
MMRESULT fake_write(HWAVEOUT,LPWAVEHDR header,UINT){
 for(const auto& packet:pending)need(packet.header!=header,"inflight storage must not be overwritten");
 header->dwFlags&=~WHDR_DONE;pending.push_back({header,0});packets.push_back(header->dwBufferLength/2);return 0;
}
MMRESULT fake_pause(HWAVEOUT){paused=true;return 0;}
MMRESULT fake_restart(HWAVEOUT){paused=false;++restarts;return 0;}
MMRESULT fake_reset(HWAVEOUT){for(auto& packet:pending)packet.header->dwFlags|=WHDR_DONE;pending.clear();return 0;}
MMRESULT fake_close(HWAVEOUT){return 0;}
std::vector<int16_t> input_samples(size_t size){
 std::vector<int16_t> input(size);for(size_t n=0;n<size;++n)input[n]=int16_t(n*17);return input;
}
}
#define GetTickCount64 fake_ticks
#define WaitForSingleObject fake_wait
#define waveOutOpen fake_open
#define waveOutPrepareHeader fake_header
#define waveOutUnprepareHeader fake_header
#define waveOutWrite fake_write
#define waveOutPause fake_pause
#define waveOutRestart fake_restart
#define waveOutReset fake_reset
#define waveOutClose fake_close
#include "../src/mk5_audio_output.cpp"
static void bounded_jitter(){
 reset_driver();const auto input=input_samples(8*960+117);mk5audio::WaveOutSink sink;
 // Begin filling the next slot, so the real sink has already waited for one
 // completed block. Exercise the remaining reserve, not just a full queue.
 sink.Push(input.data(),8*960+1);elapse_ms(109);
 sink.Push(input.data()+8*960+1,116);
 need(sink.Underruns()==0 && starved_samples==0,"109ms producer pause must fit the bounded playback reserve");
 sink.Drain();need(played==input,"jitter buffering must preserve every PCM sample and the partial tail");
 need(sink.Submitted()==input.size() && sink.Completed()==input.size(),"jitter case must complete all original samples");
 sink.Drain();need(played==input,"repeated drain must not duplicate the tail");
}
static void startup_and_tail(){
 reset_driver();const auto input=input_samples(7*960+117);mk5audio::WaveOutSink sink;
 for(size_t n=0;n<959;++n)sink.Push(&input[n],1);
 need(sink.Submitted()==0,"sub-block producer writes must accumulate before reaching Windows");
 sink.Push(&input[959],1);
 need(sink.Submitted()==960 && packets.size()==1 && packets[0]==960,"first packet must contain exactly 20ms of original PCM");
 sink.Push(input.data()+960,5*960);
 need(paused && restarts==0 && played.empty(),"six startup blocks must remain paused until the declared reserve is ready");
 sink.Push(input.data()+6*960,960+117);
 need(restarts==1 && !paused,"playback starts once after seven complete blocks");
 need(sink.Submitted()==7*960,"partial tail must remain buffered until drain");
 sink.Drain();need(played==input,"packetization and final drain must preserve byte-exact PCM");
 need(sink.Submitted()==input.size() && sink.Completed()==input.size(),"clean drain must play the entire partial tail");
 need(packets.size()==8 && packets.back()==117,"only final drain may submit a short block");
}
static void measured_underrun_and_recovery(){
 reset_driver();const auto input=input_samples(15*960+117);mk5audio::WaveOutSink sink;
 sink.Push(input.data(),8*960+1);elapse_ms(250);
 need(starved_samples>0,"over-budget pause must cause actual simulated driver starvation");
 sink.Push(input.data()+8*960+1,6*960-1);
 need(sink.Underruns()==1 && paused && restarts==1,"one real underrun must pause and rebuild the recovery reserve");
 const auto already_played=played.size();elapse_ms(109);
 need(played.size()==already_played,"six recovery blocks must remain paused");
 sink.Push(input.data()+14*960,960);
 need(!paused && restarts==2,"recovery restarts only after seven complete blocks");
 const auto starvation_before=starved_samples;elapse_ms(109);
 sink.Push(input.data()+15*960,117);
 need(sink.Underruns()==1 && starved_samples==starvation_before,"recovered reserve must withstand another bounded pause without a second underrun");
 sink.Drain();need(played==input,"recovery must not insert, drop, repeat or reorder original samples");
 need(sink.Submitted()==input.size() && sink.Completed()==input.size(),"recovery must complete every sample including its partial tail");
}
static void short_stream_drain(){
 reset_driver();const auto input=input_samples(117);mk5audio::WaveOutSink sink;
 sink.Push(input.data(),input.size());need(paused && restarts==0,"short streams must stay buffered until drain");
 sink.Drain();need(played==input && sink.Completed()==input.size(),"drain must start a stream shorter than the startup reserve");
}
static void mk2_original_packets(){
 reset_driver();
 const auto* previous=std::getenv("MK1_AUDIO_LATENCY");
 const std::string old_policy=previous?previous:"";
 need(_putenv_s("MK1_AUDIO_LATENCY","balanced")==0,"select balanced audio policy");
 constexpr size_t packet=480;
 const auto input=input_samples(10*packet+117);
 mk5audio::WaveOutSink sink(1,mk5audio::WaveOutPacket::Ms10);
 need(sink.QueueCapacityMs()==100 && sink.StartupReserveMs()==80,"MK2 Original retains the balanced queue budget");
 sink.Push(input.data(),packet-1);
 need(sink.Submitted()==0,"incomplete MK2 Original packet must stay buffered");
 sink.Push(input.data()+packet-1,1);
 need(sink.Submitted()==packet && packets==std::vector<DWORD>{packet},"MK2 Original submits 10 ms packets");
 sink.Push(input.data()+packet,6*packet);
 need(paused && restarts==0,"seven packets must stay paused until the 80 ms reserve is ready");
 sink.Push(input.data()+7*packet,packet);
 need(!paused && restarts==1,"eight packets start playback with the balanced reserve");
 sink.Push(input.data()+8*packet,input.size()-8*packet);
 sink.Drain();
 need(played==input && sink.Submitted()==input.size() && sink.Completed()==input.size(),"MK2 Original must preserve all PCM including the partial tail");
 need(sink.Underruns()==0 && packets.size()==11 && packets.back()==117,"MK2 Original drains cleanly with a short final packet");
 need(_putenv_s("MK1_AUDIO_LATENCY",old_policy.c_str())==0,"restore audio policy");
}
// Balanced: the power-on reserve (80 ms, 140 ms for the Remastered stereo mix) becomes a 60 ms target after 5 s of sound.
// An underrun after that restores it for the session; one before it keeps it.
static void balanced_short_reserve(){
 const auto* previous=std::getenv("MK1_AUDIO_LATENCY");
 const std::string old_policy=previous?previous:"";
 need(_putenv_s("MK1_AUDIO_LATENCY","balanced")==0 && _putenv_s("MK1_MUSIC_MODE","remastered")==0,"select balanced Remastered");
 for(const unsigned channels:{1u,2u})for(const bool early:{false,true}){
  reset_driver();
  const size_t packet=480*channels,reserve=channels==2?140:80,before=early?100:510;
  const auto input=input_samples((before+520)*packet+118*channels);
  mk5audio::WaveOutSink sink(channels,mk5audio::WaveOutPacket::Ms10);
  need(sink.StartupReserveMs()==reserve && sink.QueueCapacityMs()==reserve+20,"power-on keeps the policy reserve");
  size_t at=0;
  // A steady producer: the driver plays one packet per packet pushed.
  const auto feed=[&](size_t end){while(at<end){const auto n=std::min(packet,end-at);sink.Push(input.data()+at,n);at+=n;if(!paused)elapse_samples(n);}};
  feed(before*packet);
  need(sink.Underruns()==0 && sink.StartupReserveMs()==(early?reserve:60) && sink.QueueCapacityMs()==reserve+20,
       "the 60 ms target starts only after 5 s of sound, in the same ring");
  elapse_ms(400);
  need(starved_samples>0,"the pause must starve the simulated driver");
  sink.Push(input.data()+at,packet);at+=packet;
  need(sink.Underruns()==1 && paused && sink.StartupReserveMs()==reserve,"an underrun keeps or restores the policy reserve");
  sink.Push(input.data()+at,(reserve/10-2)*packet);at+=(reserve/10-2)*packet;
  need(paused,"playback waits for the whole policy reserve");
  sink.Push(input.data()+at,packet);at+=packet;
  need(!paused,"playback restarts with the policy reserve");
  feed(input.size());
  need(sink.Underruns()==1 && sink.StartupReserveMs()==reserve,"after an underrun the session never returns to the 60 ms target");
  sink.Drain();
  need(played==input && sink.Submitted()==input.size()/channels && sink.Completed()==input.size()/channels,
       "changing the target must not insert, drop, repeat or reorder samples");
 }
 need(_putenv_s("MK1_AUDIO_LATENCY",old_policy.c_str())==0 && _putenv_s("MK1_MUSIC_MODE","")==0,"restore audio policy");
}
int main(){try{
 bounded_jitter();startup_and_tail();measured_underrun_and_recovery();short_stream_drain();mk2_original_packets();
 balanced_short_reserve();
 std::cout<<"PASS asynchronous playback: startup/recovery reserves, balanced 60 ms target after power-on and its undo, byte-exact PCM and partial drain for 20ms and 10ms packets\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
