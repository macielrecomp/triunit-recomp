#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <deque>
#include <vector>
#include <iostream>
#include <stdexcept>
namespace {
std::deque<WAVEHDR*> queue;std::vector<int16_t> played;std::vector<unsigned> packet_frames;bool paused=false;unsigned starts=0;
void need(bool v,const char*m){if(!v)throw std::runtime_error(m);}
MMRESULT open_stereo(LPHWAVEOUT out,UINT_PTR,LPCWAVEFORMATEX f,DWORD_PTR,DWORD_PTR,DWORD){need(f->nChannels==2&&f->nBlockAlign==4&&f->nSamplesPerSec==48000&&f->nAvgBytesPerSec==192000,"real sink must request stereo PCM");*out=reinterpret_cast<HWAVEOUT>(1);return 0;}
MMRESULT header(HWAVEOUT,LPWAVEHDR,UINT){return 0;}
MMRESULT write(HWAVEOUT,LPWAVEHDR h,UINT){for(auto* p:queue)need(p!=h,"inflight stereo packet overwritten");h->dwFlags&=~WHDR_DONE;queue.push_back(h);packet_frames.push_back(h->dwBufferLength/4);return 0;}
MMRESULT pause(HWAVEOUT){paused=true;return 0;}MMRESULT stereo_start(HWAVEOUT){paused=false;++starts;return 0;}
MMRESULT reset(HWAVEOUT){for(auto*h:queue)h->dwFlags|=WHDR_DONE;queue.clear();return 0;}MMRESULT close(HWAVEOUT){return 0;}
DWORD stereo_wait(HANDLE,DWORD){need(!paused&&!queue.empty(),"stereo queue should play before waiting");auto*h=queue.front();queue.pop_front();auto*p=reinterpret_cast<int16_t*>(h->lpData);played.insert(played.end(),p,p+h->dwBufferLength/2);h->dwFlags|=WHDR_DONE;return WAIT_OBJECT_0;}
ULONGLONG ticks(){return 1000;}
}
#define waveOutOpen open_stereo
#define waveOutPrepareHeader header
#define waveOutUnprepareHeader header
#define waveOutWrite write
#define waveOutPause pause
#define waveOutRestart stereo_start
#define waveOutReset reset
#define waveOutClose close
#define WaitForSingleObject stereo_wait
#define GetTickCount64 ticks
#include "../src/mk5_audio_output.cpp"
int main(){try{
 _putenv_s("MK1_AUDIO_LATENCY","balanced");for(const auto mode:{"original","remastered"}){_putenv_s("MK1_MUSIC_MODE",mode);played.clear();packet_frames.clear();mk5audio::WaveOutSink sink(2,mk5audio::WaveOutPacket::Ms10);std::vector<int16_t> input(11*1920+46);for(size_t i=0;i<input.size();i+=2){input[i]=int16_t(i*13);input[i+1]=int16_t(-int32_t(i*17));}
 bool refused=false;try{sink.Push(input.data(),1);}catch(const std::invalid_argument&){refused=true;}need(refused,"incomplete stereo frame rejected");
 for(size_t i=0;i<input.size();){const size_t n=std::min<size_t>(578,input.size()-i);sink.Push(input.data()+i,n);i+=n;}
 sink.Drain();need(played==input,"all left/right samples and final partial packet preserved exactly");need(sink.Submitted()==input.size()/2&&sink.Completed()==input.size()/2,"stereo counts expressed in frames");const unsigned expected=std::string(mode)=="remastered"?140:80;need(sink.StartupReserveMs()==expected&&sink.QueueCapacityMs()==expected+20&&sink.Underruns()==0,"remastered reserve floor without changing original balanced reserve");for(size_t i=0;i+1<packet_frames.size();++i)need(packet_frames[i]==480,"stereo packets must be 10ms");need(packet_frames.back()==23,"stereo tail frame count");}std::cout<<"PASS stereo WAV format, exact L/R ordering, 10ms packets, Original80ms/Remastered140ms power-on reserve, frame accounting, partial tail, rejected incomplete frame\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
