// The Remastered reconstruction kernel: flat passband, images and aliases far below the tone, constants preserved.
#include "../audio/sinc_resampler.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void need(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
struct Response{double gain_db,residual_db;};   // the tone at 48 kHz; everything else, both relative to a full-scale tone
Response measure(uint32_t rate,double tone){
 const double pi=3.14159265358979323846;
 mk5audio::SincResampler r(rate);
 constexpr uint64_t skip=4800,count=48000;
 double re=0,im=0,power=0;uint64_t out=0;
 for(uint64_t n=0;out<skip+count;++n){
  r.Push(float(std::sin(2*pi*tone*double(n)/rate)));
  while(out<skip+count && out*rate<=(r.Written()-1)*48000ull){
   const double y=r.Sample(out);
   if(out>=skip){const double a=2*pi*tone*double(out)/48000;re+=y*std::cos(a);im+=y*std::sin(a);power+=y*y;}
   ++out;
  }
 }
 const double amplitude=2*std::sqrt(re*re+im*im)/count;
 const double residual=std::sqrt(std::max(0.0,power/count-amplitude*amplitude/2));
 return {20*std::log10(std::max(amplitude,1e-12)),20*std::log10(std::max(residual,1e-12)*std::sqrt(2.0))};
}
void passband(const char* source,uint32_t rate,std::initializer_list<double> tones,double tolerance_db=.05){
 for(const double tone:tones){
  const auto r=measure(rate,tone);const auto at=std::string(source)+" "+std::to_string(int(tone))+" Hz";
  need(std::abs(r.gain_db)<tolerance_db,at+": passband gain "+std::to_string(r.gain_db)+" dB");
  need(r.residual_db<-90,at+": images/aliases at "+std::to_string(r.residual_db)+" dB");
 }
}
}
int main(){try{
 passband("DCS",31250,{50,1000,5000,10000,13000});
 passband("YM2151",3579545/64,{50,1000,5000,10000,15000,20000});
 passband("OKI",1000000/132,{50,500,1000,2000,3000});
 passband("DAC latch",48000*32,{50,1000,5000},.05);
 // Averaging the 1.536 MHz latch readings in eights droops 0.04 dB at 10 kHz.
 passband("DAC latch",48000*32,{10000},.1);
 // Above the output's Nyquist nothing may fold back: 26 kHz from the YM2151 would land on 22 kHz.
 const auto folded=measure(3579545/64,26000);
 need(folded.residual_db<-80 && folded.gain_db<-80,"26 kHz folds back at "+std::to_string(folded.residual_db)+" dB");
 for(const uint32_t rate:{31250u,uint32_t(3579545/64),uint32_t(1000000/132),uint32_t(48000*32)}){
  mk5audio::SincResampler r(rate);uint64_t out=0;
  for(uint64_t n=0;out<4800;++n){
   r.Push(.5f);
   while(out<4800 && out*rate<=(r.Written()-1)*48000ull){
    const float y=r.Sample(out);
    // Before the kernel is full the missing past is silence; after it a constant is exact.
    if(n>=2*uint64_t(r.Delay()))need(std::abs(y-.5f)<1e-6f,"a constant must stay constant at "+std::to_string(rate)+" Hz");
    ++out;
   }
  }
 }
 need(mk5audio::SincResampler(31250).Delay()==33 && mk5audio::SincResampler(48000*32).Delay()==129*8,"kernel delay in source samples");
 mk5audio::SincResampler early(31250);early.Push(1);
 bool refused=false;try{(void)early.Sample(48);}catch(const std::logic_error&){refused=true;}
 need(refused,"a sample past the pushed source must be refused");
 std::cout<<"PASS sinc reconstruction: flat passband and images under -90 dB at the four board rates, no fold-back, exact constants\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
