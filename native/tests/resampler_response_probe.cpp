// Measurement, not a contract: steady-state response of both reconstruction kernels at each board rate.
// Per tone: gain of the fundamental at 48 kHz and everything else (images, aliases, kernel error) relative to it.
#include "../audio/sinc_resampler.hpp"
#include "../audio/williams_resampler.hpp"
#include <cmath>
#include <cstdio>
#include <initializer_list>
namespace {
bool first=true;
template<class Resampler> void measure(const char* kernel,const char* source,uint32_t rate,double tone){
 const double pi=3.14159265358979323846;
 Resampler r(rate);
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
 std::printf("%s{\"kernel\":\"%s\",\"source\":\"%s\",\"tone_hz\":%.0f,\"gain_db\":%.2f,\"residual_dbc\":%.1f}",first?"":",\n",kernel,source,tone,
             20*std::log10(amplitude),20*std::log10(std::max(residual,1e-12)/(amplitude/std::sqrt(2.0))));
 first=false;
}
}
int main(){
 struct Source{const char* name;uint32_t rate;std::initializer_list<double> tones;};
 const Source sources[]={
  {"dcs_31250",31250,{1000,5000,10000,13000,15000}},
  {"ym2151_55930",3579545/64,{1000,5000,10000,13000,15000,20000}},
  {"oki_7575",1000000/132,{500,1000,2000,3000,3500}}};
 std::puts("[");
 for(const auto& s:sources)for(const double tone:s.tones){
  measure<mk5audio::WilliamsResampler>("reference",s.name,s.rate,tone);
  measure<mk5audio::SincResampler>("sinc",s.name,s.rate,tone);
 }
 std::puts("\n]");
}
