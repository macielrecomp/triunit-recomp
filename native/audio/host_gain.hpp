#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mk5audio {
// D5: one master gain per version, only in the host, after the game's mix and before the user volume (the cabinet
// volume knob). Emulated PCM, clocks and game state never see it. Values (dB) match the integrated loudness of the
// first fight with music to L3 = -23.62 LUFS (build/native/mk3-product/audio-measure.json, F5).
namespace master_db {
// Remastered since 26/11/2026 (studio master): the loudness of three music renders per game matched to the Original's
// (build/native/audio-audition/scripts/game_render.py, BS.1770); effects and voices were not part of that measure.
constexpr double mk1_original=0.27,mk1_remastered=-0.45,mk2_original=15.92,mk2_remastered=0.31;
}

// Gain plus safety limiter (D3: it must not act on any reference route; it guards sound no route played).
class HostGain {
    double gain_,envelope_=1.0;
    uint64_t limited_=0,clipped_=0;
    template<class A> friend void Fields(A& a,HostGain& h){a(h.envelope_,h.limited_,h.clipped_);}
public:
    // -1.0 dBTP less the largest true-peak excess over sample peak measured (0.049 dB on 134 routes), rounded up: -1.1 dBFS.
    // shortcut: sample-peak detection; a near-Nyquist waveform held at the ceiling can still pass -1 dBTP; upgrade to
    // oversampled detection if the limiter counter fires in the field.
    static constexpr double ceiling=28870.0;
    explicit HostGain(double decibels):gain_(std::pow(10.0,decibels/20.0)) {
        if(!std::isfinite(gain_))throw std::invalid_argument("host gain is not finite");
    }
    double Gain()const{return gain_;}
    // Values in int16 units before the gain. A common gain keeps the stereo balance; release is 80 ms at 48 kHz.
    template<size_t N> std::array<int16_t,N> Render(std::array<double,N> mix) {
        double peak=0;
        for(auto& v:mix){
            if(!std::isfinite(v))throw std::invalid_argument("host mix is not finite");
            v*=gain_;peak=std::max(peak,std::abs(v));
        }
        const double target=peak>ceiling?ceiling/peak:1.0;
        envelope_=std::min(target,1.0-(1.0-envelope_)*.9997396172388102);
        if(target==1.0 && envelope_>1.0-1e-6)envelope_=1.0;
        limited_+=envelope_<1.0;
        std::array<int16_t,N> out{};
        for(size_t c=0;c<N;++c){
            const double value=mix[c]*envelope_;
            clipped_+=value<=-32768.0 || value>=32767.0;
            out[c]=int16_t(std::clamp(value,-ceiling,ceiling));
        }
        return out;
    }
    // int16 input was already cut at full scale upstream (MK1's int16 clamp): those samples count as saturated too.
    void Process(const int16_t* in,int16_t* out,size_t count,unsigned channels) {
        if((channels!=1 && channels!=2) || count%channels || (count && (!in || !out)))throw std::invalid_argument("host gain block");
        for(size_t n=0;n<count;n+=channels){
            for(unsigned c=0;c<channels;++c)clipped_+=in[n+c]==32767 || in[n+c]==-32768;
            if(channels==1)out[n]=Render<1>({double(in[n])})[0];
            else{const auto s=Render<2>({double(in[n]),double(in[n+1])});out[n]=s[0];out[n+1]=s[1];}
        }
    }
    uint64_t Limited()const{return limited_;}
    uint64_t Clipped()const{return clipped_;}
};
}
