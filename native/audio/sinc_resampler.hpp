#pragma once
// Remastered presentation only: band-limited reconstruction of one board source at 48 kHz (Kaiser-windowed sinc).
// Same streaming shape as WilliamsResampler, which stays the Arcade Original's reference kernel
// (native/tests/resampler_response_probe.cpp measures both).
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace mk5audio {
class SincResampler {
    static constexpr int kZero=32,kPhases=512;   // zero crossings per side; table steps per crossing
    // -6 dB point relative to the lower of the source's and the output's Nyquist; Kaiser beta 9 is about -90 dB.
    static constexpr double kCutoff=.96,kBeta=9;
    std::array<float,1024> samples{};   // the source, averaged in groups of `divide`
    uint64_t written=0;
    float group_sum=0;
    uint32_t rate,divide;
    int half;
    double scale;
    template<class A> friend void Fields(A& a,SincResampler& r){a(r.samples,r.written,r.group_sum);}
    static const std::vector<float>& Table(){
        static const auto table=[] {
            const auto bessel=[](double x){double sum=1,term=1;for(int k=1;k<64;++k){term*=x/(2*k);sum+=term*term;}return sum;};
            const double pi=3.14159265358979323846;
            std::vector<float> t(size_t(kZero)*kPhases+2);
            for(int j=0;j<kZero*kPhases;++j){
                const double x=double(j)/kPhases,phase=pi*kCutoff*x,edge=x/kZero;
                t[size_t(j)]=float((j?std::sin(phase)/phase:1.0)*bessel(kBeta*std::sqrt(1-edge*edge))/bessel(kBeta));
            }
            return t;
        }();
        return table;
    }
    float At(int64_t group)const{
        if(group<0)return 0;
        const uint64_t groups=written/divide;
        if(uint64_t(group)>=groups || groups-uint64_t(group)>samples.size())throw std::logic_error("audio reconstruction history range");
        return samples[size_t(group)%samples.size()];
    }
public:
    // Sources far above the output rate are first averaged down to at most 192 kHz (the DAC latch is read at 1.536 MHz).
    explicit SincResampler(uint32_t source_rate):rate(source_rate),divide(rate>192000?rate/192000:1),
        half(0),scale(std::min(1.0,48000.0*divide/std::max<uint32_t>(rate,1))) {
        if(!rate)throw std::invalid_argument("audio reconstruction rate");
        half=int(std::ceil(kZero/scale));
        if(size_t(2*half+2)>samples.size())throw std::invalid_argument("audio reconstruction rate");
    }
    uint64_t Written()const{return written;}
    // Source samples by which the output trails the source: the kernel is centred that far behind the newest sample.
    uint32_t Delay()const{return uint32_t(half+1)*divide;}
    void Push(float sample){group_sum+=sample;++written;if(written%divide==0){samples[size_t(written/divide-1)%samples.size()]=group_sum/float(divide);group_sum=0;}}
    // Needs the source up to floor(output_index*rate/48000), like the reference kernel.
    float Sample(uint64_t output_index)const{
        const uint64_t unit=uint64_t(48000)*divide,position=output_index*rate;
        const int64_t group=int64_t(position/unit);
        const double fraction=double(position%unit)/double(unit);
        const auto& table=Table();
        double sum=0,weights=0;
        // 2*half+1 groups ending at group-1, around group-half-1+fraction. Weights are normalized: a constant stays a constant.
        for(int k=0;k<=2*half;++k){
            const double x=std::abs(k-half-fraction)*scale*kPhases;
            if(x>=double(kZero)*kPhases)continue;
            const auto index=size_t(x);const double weight=table[index]+(table[index+1]-table[index])*(x-double(index));
            sum+=weight*At(group-2*half-1+k);weights+=weight;
        }
        return float(sum/weights);
    }
};
}
