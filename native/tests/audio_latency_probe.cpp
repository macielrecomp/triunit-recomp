// Measurement, not a contract: time from the sink's Push of a faint tick to that tick reaching the Windows mix of the
// default output device (WASAPI loopback). Covers the sink's queue and the Windows audio path; not the device itself
// (DAC, Bluetooth). The producer here pushes one arcade frame of PCM per frame period, like the games.
// Needs a quiet default output device; plays a 1 ms tick at -20 dBFS every 0.4 s. MK1_AUDIO_LATENCY selects the policy.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "mk5_audio_output.hpp"
#include "mk5_timing.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
namespace {
void ok(HRESULT result,const char* what){if(FAILED(result))throw std::runtime_error(what);}
double now(){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return double(t.QuadPart)/double(f.QuadPart);}
}
int main(int argc,char** argv){try{
 ok(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
 IMMDeviceEnumerator* devices=nullptr;IMMDevice* device=nullptr;IAudioClient* client=nullptr;IAudioCaptureClient* capture=nullptr;WAVEFORMATEX* format=nullptr;
 ok(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&devices)),"device enumerator");
 ok(devices->GetDefaultAudioEndpoint(eRender,eConsole,&device),"default output device");
 ok(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client)),"loopback client");
 ok(client->GetMixFormat(&format),"mix format");
 const bool is_float=format->wBitsPerSample==32 && (format->wFormatTag==WAVE_FORMAT_IEEE_FLOAT || format->wFormatTag==WAVE_FORMAT_EXTENSIBLE);
 if(!is_float)throw std::runtime_error("loopback mix format is not 32-bit float");
 ok(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,10000000,0,format,nullptr),"loopback stream");
 ok(client->GetService(__uuidof(IAudioCaptureClient),reinterpret_cast<void**>(&capture)),"capture service");
 ok(client->Start(),"loopback start");
 const unsigned channels=format->nChannels;const double rate=format->nSamplesPerSec;
 // The loopback follows the device volume: the threshold is set from the noise heard during the silent warm-up.
 constexpr float tick=.1f;float threshold=1;
 double heard=-1;float floor_peak=0;uint64_t captured=0;
 // First loopback sample over the threshold, on the QPC clock; everything below it counts as the noise floor.
 const auto listen=[&]{
  for(UINT32 size=0;SUCCEEDED(capture->GetNextPacketSize(&size)) && size;){
   BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;UINT64 position=0,qpc=0;
   ok(capture->GetBuffer(&data,&frames,&flags,&position,&qpc),"loopback read");captured+=frames;
   if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT)){
    const auto* samples=reinterpret_cast<const float*>(data);
    for(UINT32 n=0;n<frames;++n)for(unsigned c=0;c<channels;++c){
     const float value=std::fabs(samples[n*channels+c]);
     if(value>=threshold){if(heard<0)heard=double(qpc)/1e7+n/rate;}else floor_peak=std::max(floor_peak,value);
    }
   }
   ok(capture->ReleaseBuffer(frames),"loopback release");
  }
 };
 // probe [channels 1|2] [packet ms 10|20] [seconds] [late ms]: every 3 s one frame arrives that many ms late.
 const unsigned channels_out=argc>1?unsigned(std::atoi(argv[1])):1,packet=argc>2?unsigned(std::atoi(argv[2])):20;
 const unsigned frames=unsigned((argc>3?std::atof(argv[3]):6.5)*54.7069),late_ms=argc>4?unsigned(std::atoi(argv[4])):0;
 std::vector<double> latencies;uint64_t underruns=0;unsigned reserve=0,capacity=0,final_reserve=0;int lowest=1000;
 {
  mk5audio::WaveOutSink sink(channels_out,packet==10?mk5audio::WaveOutPacket::Ms10:mk5audio::WaveOutPacket::Ms20);
  reserve=sink.StartupReserveMs();capacity=sink.QueueCapacityMs();
  // The games' own frame pacer (mk5_window.cpp Present): deadlines trimmed by the queue's fill.
  const double frame=1/54.7069;double due=0,samples=0,pushed_at=-1;
  std::vector<int16_t> pcm;
  for(unsigned n=0;n<frames;++n){
   samples+=48000*frame;pcm.assign(size_t(samples)*channels_out,0);samples-=double(pcm.size()/channels_out);
   const bool mark=n>=82 && (n-82)%22==0;
   if(n==82)threshold=std::max(floor_peak*8,1e-6f);
   if(mark){listen();if(heard>=0)throw std::runtime_error("the output device is not quiet: close other audio and run again");
    for(size_t k=0;k<48*channels_out;++k)pcm[k]=int16_t((k/channels_out/12%2?-1:1)*tick*32767);}
   if(late_ms && n>82 && n%164==11)Sleep(late_ms);
   const double before=now();sink.Push(pcm.data(),pcm.size());
   if(mark)pushed_at=before;
   const int fill=mk5audio::output_reserve_percent;if(n>82 && fill>=0)lowest=std::min(lowest,fill);
   const double period=double(mk5timing::ProducerInterval(1000000000,true,fill))/1e9*frame;
   due=double(mk5timing::ProducerDeadline(uint64_t(due*1e9),uint64_t(now()*1e9),uint64_t(period*1e9),true,fill>=100))/1e9;
   while(now()<due){listen();Sleep(1);}
   listen();
   if(heard>=0 && pushed_at>=0){latencies.push_back((heard-pushed_at)*1000);pushed_at=-1;}
   if(pushed_at<0)heard=-1;
  }
  sink.Drain();underruns=sink.Underruns();final_reserve=sink.StartupReserveMs();
 }
 client->Stop();capture->Release();client->Release();device->Release();devices->Release();CoTaskMemFree(format);CoUninitialize();
 if(latencies.size()<6)throw std::runtime_error("ticks not found in the loopback (loudest sample "+std::to_string(floor_peak)+
                                                 ", "+std::to_string(captured)+" frames): is the output device muted or at zero volume?");
 std::sort(latencies.begin(),latencies.end());
 const char* policy=std::getenv("MK1_AUDIO_LATENCY");
 std::printf("{\"policy\":\"%s\",\"channels\":%u,\"packet_ms\":%u,\"late_frame_ms\":%u,\"startup_reserve_ms\":%u,\"queue_capacity_ms\":%u,"
             "\"final_reserve_ms\":%u,\"lowest_fill_percent\":%d,\"ticks\":%zu,\"push_to_mix_ms\":{\"min\":%.1f,\"median\":%.1f,\"max\":%.1f},"
             "\"underruns\":%llu}\n",policy?policy:"",channels_out,packet,late_ms,reserve,capacity,final_reserve,lowest,latencies.size(),
             latencies.front(),latencies[latencies.size()/2],latencies.back(),static_cast<unsigned long long>(underruns));
 return 0;
}catch(const std::exception& e){std::fprintf(stderr,"latency probe: %s\n",e.what());return 1;}}
