#pragma once
#include "mk2_adsp.hpp"
#include "mk2_audio.hpp"
#include "../audio/host_gain.hpp"
#include "../audio/studio_master.hpp"
#include <filesystem>

namespace mk2 {
namespace state { struct Writer; struct Reader; }
int MusicTrack(unsigned command);
class RemasteredMix {
    mk5audio::HostGain host_{mk5audio::master_db::mk2_remastered};
    template<class A> friend void Fields(A& a,RemasteredMix& m){a(m.host_);}
public:
    // Median stereo R128 difference of eight matched excerpts; see music_normalization.py.
    static constexpr float original_gain=5.27837207f; // +14.45 dB
    std::array<int16_t,2> Render(float original,std::array<int16_t,2> music,float music_gain=.25f,
                                 std::array<double,2> release={}) {
        return Render(std::array<float,2>{original,original},music,music_gain,release);
    }
    // The board's sound already mastered to stereo (mk5audio::StudioMaster).
    std::array<int16_t,2> Render(std::array<float,2> original,std::array<int16_t,2> music,float music_gain=.25f,
                                 std::array<double,2> release={}) {
        if(!std::isfinite(original[0]) || !std::isfinite(original[1]) || !std::isfinite(music_gain) || music_gain<0 ||
           !std::isfinite(release[0]) || !std::isfinite(release[1]))throw std::invalid_argument("MK2 mix is not finite");
        const double scale=32768.0*original_gain;
        return host_.Render<2>({original[0]*scale+music[0]*double(music_gain)+release[0],original[1]*scale+music[1]*double(music_gain)+release[1]});
    }
    uint64_t Limited()const{return host_.Limited();}
    uint64_t Clipped()const{return host_.Clipped();}
};

class OriginalAudio {
    mk5audio::HostGain host_{mk5audio::master_db::mk2_original};
    DcsFloatAudio audio_;
    template<class A> friend void Fields(A& a,OriginalAudio& o){a(o.host_,o.audio_);}
public:
    explicit OriginalAudio(mk5audio::AudioConsumer consumer)
        :audio_([this,consumer](const float* samples,size_t count) {
            std::array<int16_t,1024> mono{};
            for(size_t n=0;n<count;++n)mono[n]=host_.Render<1>({double(samples[n])*32768.0})[0];
            consumer(mono.data(),count);
        }) {
        if(!consumer)throw std::invalid_argument("original DCS requires a consumer");
    }
    OriginalAudio(const OriginalAudio&)=delete;
    OriginalAudio& operator=(const OriginalAudio&)=delete;
    void Feed(uint64_t first,const float* samples,size_t count){audio_.Feed(first,samples,count);}
    uint64_t SourceSamples()const{return audio_.SourceSamples();}
    uint64_t OutputSamples()const{return audio_.OutputSamples();}
    uint64_t Clipped()const{return host_.Clipped();}
    uint64_t Limited()const{return host_.Limited();}
};

// Optional presentation sidecar. Canonical registers, memories and PCM are read-only.
class DcsEffects {
    AdspBoot& dsp_;
    std::array<uint16_t,258> before_{},spectrum_{};
    std::array<uint32_t,16> overlap_{};
    std::unique_ptr<AdspBoot> transform_;
    std::array<int16_t,480> output_{};
    std::array<uint64_t,480> output_tags_{};
    std::array<float,480> output_gains_{};
    std::array<int16_t,32768> ring_{};
    std::array<uint64_t,32768> tags_{};
    std::array<float,32768> gains_{};
    float channel_gain_=0;
    uint64_t generation_=0,tag_=0;
    bool overlap_valid_=false;
    unsigned channel_=0;
    void Instruction(const AdspBoot&);
    void Sport(const AdspBoot&,uint16_t);
    void TransformUntil(uint16_t);
    AdspBoot& NewTransform();
    // Save-state value fields. The transform copy exists only between DSP PCs 0xaa and 0x13b: a load rebuilds it
    // from the live DSP (its link and ROMs) and then restores its own value fields.
    template<class A> friend void Fields(A& a,DcsEffects& e){
        a(e.before_,e.spectrum_,e.overlap_,e.output_,e.output_tags_,e.output_gains_,e.ring_,e.tags_,e.gains_,
          e.channel_gain_,e.generation_,e.tag_,e.overlap_valid_,e.channel_);
        bool transform=e.transform_!=nullptr;a(transform);
        if constexpr(A::reading){if(transform)e.NewTransform();else e.transform_.reset();}
        if(transform)a(*e.transform_);
    }
public:
    explicit DcsEffects(AdspBoot&);
    ~DcsEffects();
    DcsEffects(const DcsEffects&)=delete;
    DcsEffects& operator=(const DcsEffects&)=delete;
    void Read(uint64_t first,size_t count,float* effects,uint64_t* tags,float* gains=nullptr) const;
};

class RemasteredAudio {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Save-state value fields (mk2_music.cpp). The user's tracks are reloaded from their files, never serialized.
    friend void Fields(state::Writer&,RemasteredAudio&);
    friend void Fields(state::Reader&,RemasteredAudio&);
public:
    RemasteredAudio(AdspBoot&,const std::filesystem::path&,mk5audio::AudioConsumer);
    ~RemasteredAudio();
    // Identity of the loaded user tracks (slot, PCM, window, loop and gain), computed with the load; binds a save state.
    const std::string& MusicFingerprint() const;
    void Feed(uint64_t,const float*,size_t);
    uint64_t SourceSamples()const;
    uint64_t OutputSamples()const;
    uint64_t Starts()const;
    uint64_t Clipped()const;
    uint64_t Limited()const;
};
}
