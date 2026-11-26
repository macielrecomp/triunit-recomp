#pragma once
#include "../audio/williams_resampler.hpp"
#include "../audio/sinc_resampler.hpp"
#include "mk5_audio_output.hpp"
#include <algorithm>
#include <cmath>
#include <utility>
#include <type_traits>

namespace mk2 {
template<class Sample,class Resampler=mk5audio::WilliamsResampler> class DcsAudioStream {
    Resampler resampler_{31250};
    std::function<void(const Sample*,size_t)> consumer_;
    uint64_t output_samples_=0;
    // Save-state fields; the consumer stays with the live session so Feed continues after a load.
    template<class A> friend void Fields(A& a,DcsAudioStream& s){a(s.resampler_,s.output_samples_);}
public:
    explicit DcsAudioStream(std::function<void(const Sample*,size_t)> consumer):consumer_(std::move(consumer)) {
        if (!consumer_) throw std::invalid_argument("DCS PCM requires an output consumer");
    }
    uint64_t SourceSamples() const {return resampler_.Written();}
    uint64_t OutputSamples() const {return output_samples_;}
    void Feed(uint64_t first,const float* samples,size_t count) {
        if (first!=SourceSamples() || count>512 || (count && !samples))
            throw std::invalid_argument("DCS PCM discontinuity or invalid source block");
        for (size_t n=0;n<count;++n)
            if (!std::isfinite(samples[n])) throw std::invalid_argument("DCS PCM is not finite");
        std::array<Sample,1024> output{};
        size_t produced=0;
        for (size_t n=0;n<count;++n) {
            resampler_.Push(samples[n]);
            while (output_samples_*31250<=(SourceSamples()-1)*48000) {
                const float value=resampler_.Sample(output_samples_++);
                // sound.cpp::output_push truncates float*32768 then clips.
                if constexpr(std::is_same_v<Sample,float>)output[produced++]=value;
                else output[produced++]=value>=1.0f?32767:value<=-1.0f?-32768:int16_t(int(value*32768));
            }
        }
        if (produced) consumer_(output.data(),produced);
    }
};
using DcsAudio=DcsAudioStream<int16_t>;
using DcsFloatAudio=DcsAudioStream<float>;
// Remastered presentation: the same DCS samples through the band-limited kernel; its output trails by kCleanDelay source samples.
using DcsCleanAudio=DcsAudioStream<float,mk5audio::SincResampler>;
inline const uint32_t kCleanDelay=mk5audio::SincResampler(31250).Delay();
}
