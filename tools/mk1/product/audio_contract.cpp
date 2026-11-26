#define main historical_audio_queue_main
#include "../../native/tests/mk5_audio_queue_contract.cpp"
#undef main
#include <cstdlib>
int main(){try{
 _putenv_s("MK1_AUDIO_LATENCY","safe");need(historical_audio_queue_main()==0,"historical asynchronous audio contract");
 for(const auto policy:{"low","balanced","safe"}){
  _putenv_s("MK1_AUDIO_LATENCY",policy);reset_driver();const auto input=input_samples(12*960+117);
  mk5audio::WaveOutSink sink(1,mk5audio::WaveOutPacket::Ms10);
  const unsigned expected=std::string(policy)=="low"?40:(std::string(policy)=="balanced"?80:140);
  need(sink.StartupReserveMs()==expected&&sink.QueueCapacityMs()==expected+20,"audio reserve/capacity must match policy");
  for(size_t at=0;at<input.size();){const auto count=std::min<size_t>(791,input.size()-at);sink.Push(input.data()+at,count);at+=count;}
  sink.Drain();need(played==input&&sink.Submitted()==input.size()&&sink.Completed()==input.size(),"latency settings must preserve every original PCM sample in order");
 }
 _putenv_s("MK1_AUDIO_LATENCY","");std::cout<<"PASS all three audio reserves, byte-exact PCM, historical starvation/recovery\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
