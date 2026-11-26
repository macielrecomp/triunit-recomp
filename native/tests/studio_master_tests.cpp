// The Remastered studio chain: what must hold whatever the recipe's taste. Sound quality is judged by ear, not here.
#include "../audio/studio_master.hpp"
#include "../mk2/mk2_state_io.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void need(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
const double pi=3.14159265358979323846;
std::vector<float> noise(size_t count,uint32_t seed,double level){
 std::vector<float> out(count);
 for(auto& v:out){seed=seed*1664525u+1013904223u;v=float((double(seed>>8)/8388608.0-1)*level);}
 return out;
}
double filter_gain_db(mk5audio::studio::Biquad filter,double hz){
 double re=0,im=0;const unsigned skip=48000,count=48000;
 for(unsigned n=0;n<skip+count;++n){
  const double a=2*pi*hz*n/48000,y=filter.Run(std::sin(a));
  if(n>=skip){re+=y*std::cos(a);im+=y*std::sin(a);}
 }
 return 20*std::log10(2*std::sqrt(re*re+im*im)/count);
}
// Power above 12 kHz of the mid, through two high-pass sections.
double high_band(mk5audio::StudioSettings settings,const std::vector<float>& input){
 mk5audio::StudioMaster chain(settings);auto a=mk5audio::studio::Biquad::HighPass(12000),b=a;double sum=0;
 for(size_t n=0;n<input.size();++n){const auto out=chain.Process(input[n]);const double y=b.Run(a.Run((out[0]+out[1])/2));if(n>input.size()/2)sum+=y*y;}
 return sum;
}
}
int main(){try{
 using mk5audio::StudioSettings;using mk5audio::StudioMaster;using mk5audio::studio::Biquad;
 need(std::abs(filter_gain_db(Biquad::Shelf(95,3,true),20)-3)<.2 && std::abs(filter_gain_db(Biquad::Shelf(95,3,true),5000))<.1,"low shelf");
 need(std::abs(filter_gain_db(Biquad::Shelf(2600,3,false,.6),16000)-3)<.2 && std::abs(filter_gain_db(Biquad::Shelf(2600,3,false,.6),100))<.1,"high shelf");
 need(std::abs(filter_gain_db(Biquad::Bell(330,-1,1),330)+1)<.05 && std::abs(filter_gain_db(Biquad::Bell(330,-1,1),5000))<.05,"bell");
 need(filter_gain_db(Biquad::HighPass(250),25)<-38 && std::abs(filter_gain_db(Biquad::HighPass(250),4000))<.05,"high pass");
 const auto input=noise(96000,77,.02);
 for(const auto& settings:{StudioSettings::Mk1(),StudioSettings::Mk2(),StudioSettings::Mk3()}){
  const std::string game=settings.band_end_hz==0?"MK1":settings.band_end_hz<10000?"MK2":"MK3";
  // Silence in, silence out: no stage may invent sound.
  {StudioMaster chain(settings);for(unsigned n=0;n<48000;++n){const auto out=chain.Process(0);need(out[0]==0 && out[1]==0,game+": silence must stay silent");}}
  // Deterministic, finite, bounded and stereo.
  StudioMaster first(settings),second(settings);double peak=0,difference=0;
  for(const float x:input){
   const auto a=first.Process(x),b=second.Process(x);
   need(a==b,game+": two chains must give the same samples");
   need(std::isfinite(a[0]) && std::isfinite(a[1]),game+": output must be finite");
   peak=std::max<double>(peak,std::max(std::abs(a[0]),std::abs(a[1])));difference+=std::abs(a[0]-a[1]);
  }
  need(difference>0,game+": the master must be stereo");
  need(peak>.001 && peak<.2,game+": a -34 dBFS programme must stay near its level (peak "+std::to_string(peak)+")");
  // A save state taken mid-stream continues exactly.
  StudioMaster live(settings),restored(settings);
  for(unsigned n=0;n<30000;++n)live.Process(input[n]);
  mk2::state::Writer writer;Fields(writer,live);
  mk2::state::Reader reader{writer.bytes.data(),writer.bytes.data()+writer.bytes.size()};Fields(reader,restored);
  need(reader.at==reader.end,game+": the state must be read whole");
  for(unsigned n=30000;n<60000;++n){const auto a=live.Process(input[n]),b=restored.Process(input[n]);need(a==b,game+": a restored chain must continue sample for sample");}
  need(StudioMaster(settings).Latency()==(settings.band_end_hz==0?0u:512u),game+": declared latency");
 }
 // DCS sources gain sound above their own band; switching the replication off (-200 dB) is the control.
 {
  // 150 tones between 200 Hz and 9 kHz: nothing of its own above the MK2 band end.
  std::vector<float> banded(96000);const auto phases=noise(150,5,pi);
  for(size_t n=0;n<banded.size();++n){double sum=0;for(unsigned k=0;k<150;++k)sum+=std::sin(2*pi*(200+58.7*k)*double(n)/48000+phases[k]);banded[n]=float(sum*.0016);}
  auto off=StudioSettings::Mk2();off.air_db=-200;
  const double with=high_band(StudioSettings::Mk2(),banded),without=high_band(off,banded);
  // The calibrated copies sit about 22 dB under the source band; the control is the tones' own leakage.
  need(with>without*4,"MK2: replication must add at least 6 dB above 12 kHz (got "+std::to_string(10*std::log10(with/without))+" dB)");
 }
 std::cout<<"PASS studio master: filters, silent on silence, deterministic, finite, stereo, exact continuation after a save state, replication above the band\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
