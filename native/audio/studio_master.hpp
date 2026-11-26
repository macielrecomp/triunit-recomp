#pragma once
// Remastered presentation only: the board's own mono mix at 48 kHz -> a stereo master. The recipe is the one the user
// approved by ear on 26/11/2026 ("estudio amplo"); the Arcade Original never constructs this.
// Stages, in order: spectral gate against the DCS output floor and band replication above the stream's own band
// (DCS sources only, one 512-point STFT: 10.7 ms of delay), bass/tilt/mud equalizer, mid/side widening with a
// velvet-noise decorrelator (left + right stays the mono), a short room by partitioned convolution (its pre-delay
// absorbs the block), and a bus compressor that follows the programme's own level. Input in full-scale units (+-1).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace mk5audio {
struct StudioSettings {
    double band_end_hz;        // where the source's own band ends; 0: already full band (no gate, no replication)
    double noise_db;           // DCS output floor: power per source sample, dB re full scale
    double air_slope_db_khz,air_db,bass_db,tilt_db;
    double side_db=-8,wet_db=-15;   // stereo side and room levels, as approved for music
    // Measured on the user's ROM renders (docs/DISPLAY_INPUT_SETUP.md): band ends and floors of the DCS streams.
    static StudioSettings Mk1(){return {0,0,0,0,2.0,1.0};}
    static StudioSettings Mk2(){return {9354,-99.0,0.6,-12.0,3.0,3.0};}
    static StudioSettings Mk3(){return {11284,-93.3,0.6,-12.0,3.0,3.0};}
};
namespace studio {
inline constexpr double kPi=3.14159265358979323846,kRate=48000;
class Fft {
    unsigned n_;std::vector<float> cos_,sin_;std::vector<unsigned> reverse_;
public:
    explicit Fft(unsigned n):n_(n),cos_(n/2),sin_(n/2),reverse_(n) {
        unsigned bits=0;while((1u<<bits)<n)++bits;
        for(unsigned i=0;i<n;++i){unsigned r=0;for(unsigned b=0;b<bits;++b)if(i&(1u<<b))r|=1u<<(bits-1-b);reverse_[i]=r;}
        for(unsigned i=0;i<n/2;++i){cos_[i]=float(std::cos(2*kPi*i/n));sin_[i]=float(std::sin(2*kPi*i/n));}
    }
    void Run(float* re,float* im,bool inverse)const {
        for(unsigned i=0;i<n_;++i)if(i<reverse_[i]){std::swap(re[i],re[reverse_[i]]);std::swap(im[i],im[reverse_[i]]);}
        for(unsigned length=2;length<=n_;length<<=1){
            const unsigned half=length/2,step=n_/length;
            for(unsigned i=0;i<n_;i+=length)for(unsigned j=0;j<half;++j){
                const float wr=cos_[j*step],wi=inverse?sin_[j*step]:-sin_[j*step];
                const float xr=re[i+j+half]*wr-im[i+j+half]*wi,xi=re[i+j+half]*wi+im[i+j+half]*wr;
                re[i+j+half]=re[i+j]-xr;im[i+j+half]=im[i+j]-xi;re[i+j]+=xr;im[i+j]+=xi;
            }
        }
        if(inverse){const float k=1.f/float(n_);for(unsigned i=0;i<n_;++i){re[i]*=k;im[i]*=k;}}
    }
};
struct Biquad {
    double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    template<class A> friend void Fields(A& a,Biquad& q){a(q.z1,q.z2);}
    double Run(double x){const double y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
    // RBJ cookbook forms.
    static Biquad From(const double b[3],const double d[3]){Biquad q;q.b0=b[0]/d[0];q.b1=b[1]/d[0];q.b2=b[2]/d[0];q.a1=d[1]/d[0];q.a2=d[2]/d[0];return q;}
    static Biquad Shelf(double hz,double db,bool low,double q=.707){
        const double a=std::pow(10,db/40),w=2*kPi*hz/kRate,c=std::cos(w),s=2*std::sqrt(a)*std::sin(w)/(2*q),m=low?-1:1;
        const double b[3]={a*((a+1)+m*(a-1)*c+s),-m*2*a*((a-1)+m*(a+1)*c),a*((a+1)+m*(a-1)*c-s)};
        const double d[3]={(a+1)-m*(a-1)*c+s,m*2*((a-1)-m*(a+1)*c),(a+1)-m*(a-1)*c-s};
        return From(b,d);
    }
    static Biquad Bell(double hz,double db,double q){
        const double a=std::pow(10,db/40),w=2*kPi*hz/kRate,al=std::sin(w)/(2*q),c=std::cos(w);
        const double b[3]={1+al*a,-2*c,1-al*a},d[3]={1+al/a,-2*c,1-al/a};return From(b,d);
    }
    static Biquad HighPass(double hz,double q=.7071){
        const double w=2*kPi*hz/kRate,al=std::sin(w)/(2*q),c=std::cos(w);
        const double b[3]={(1+c)/2,-(1+c),(1+c)/2},d[3]={1+al,-2*c,1-al};return From(b,d);
    }
};
// Deterministic noise for the decorrelator and the room: the tables are rebuilt identically at every construction.
struct Lcg {
    uint32_t state;
    double Uniform(){state=state*1664525u+1013904223u;return double(state>>8)/16777216.0;}
    double Gauss(){return (Uniform()+Uniform()+Uniform()+Uniform()-2)*1.7320508;}
};
}
class StudioMaster {
    // One STFT for the gate and the band replication.
    // 44 bins = 4.1 kHz of source band, copied 40, 84 and 128 bins up. Every shift is a multiple of kFrame/kHop bins:
    // any other shift turns a copied bin's phase between overlapping frames and they cancel (measured: -8 dB at the
    // first copy, nothing left at the third).
    static constexpr unsigned kFrame=512,kHop=128,kBins=kFrame/2+1,kBand=44,kFade=4;
    // Widening and room, as approved.
    static constexpr unsigned kVelvet=1440,kBlock=256;
    static constexpr double kRt60=.45,kPreDelay=.012;
    StudioSettings settings_;
    studio::Fft frame_fft_{kFrame},block_fft_{2*kBlock};
    std::array<float,kFrame> window_{};
    // Spectral stage state.
    std::array<float,kFrame> input_{},overlap_{};
    std::array<float,kHop> ready_{};
    std::array<float,kBins> smooth_{};
    std::array<float,kBand> level_{},capped_level_{};
    uint32_t hop_at_=0,frames_=0;
    // Equalizer and widening state.
    // dc_: the MK1 board idles its DAC at -20 dBFS of DC, which would sit in the compressor's level (the cabinet is AC-coupled).
    studio::Biquad dc_,bass_,tilt_,mud_,side_pass_;
    std::array<float,kVelvet> side_line_{};
    uint32_t side_at_=0;
    std::vector<uint16_t> velvet_at_;std::vector<float> velvet_gain_;
    // Room state: the last 2*kBlock inputs, a ring of their spectra, and the wet block being played.
    unsigned partitions_=0;
    std::vector<float> room_re_[2],room_im_[2];   // per channel: partitions x (kBlock+1) spectra of the impulse response
    std::array<float,2*kBlock> room_input_{};
    std::vector<float> ring_re_,ring_im_;
    std::array<std::array<float,kBlock>,2> wet_{};
    uint32_t block_at_=0,ring_at_=0;
    // Compressor state.
    double detector_=0,programme_=0,reduction_db_=0;
    uint32_t programme_count_=0;
    template<class A> friend void Fields(A& a,StudioMaster& s){
        a(s.input_,s.overlap_,s.ready_,s.smooth_,s.level_,s.capped_level_,s.hop_at_,s.frames_,s.dc_,s.bass_,s.tilt_,s.mud_,s.side_pass_,
          s.side_line_,s.side_at_,s.room_input_,s.ring_re_,s.ring_im_,s.wet_,s.block_at_,s.ring_at_,s.detector_,s.programme_,
          s.reduction_db_,s.programme_count_);
    }
    void Frame() {
        std::array<float,kFrame> re{},im{};
        for(unsigned n=0;n<kFrame;++n)re[n]=input_[n]*window_[n];
        frame_fft_.Run(re.data(),im.data(),false);
        const double hz=studio::kRate/kFrame;
        // Gate: each bin against the DCS floor (white up to the source's Nyquist), on a power smoothed over 12 ms and 3 bins.
        const float noise=float(std::pow(10,settings_.noise_db/10)*(48000.0/31250.0)*(kFrame/2)*3.0);
        const float keep=float(std::exp(-double(kHop)/(studio::kRate*.012)));
        std::array<float,kBins> power{};
        for(unsigned k=0;k<kBins;++k)power[k]=re[k]*re[k]+im[k]*im[k];
        for(unsigned k=0;k<kBins;++k){
            const float local=(power[k?k-1:k]+power[k]+power[k+1<kBins?k+1:k])/3;
            smooth_[k]=smooth_[k]*keep+local*(1-keep);
            const float gain=std::sqrt(std::clamp(1-noise/std::max(smooth_[k],1e-30f),.0631f,1.f));   // floor -12 dB
            re[k]*=gain;im[k]*=gain;
        }
        // Replication: the band under band_end, tonal peaks capped (a copied tone would be inharmonic) and flattened to
        // its own long-term level, tiled up to 20 kHz under one slope. Each frame keeps the source band's own motion.
        const unsigned k0=unsigned(std::lround(settings_.band_end_hz/hz)),fade=kFade,start=k0-fade,end=unsigned(20000/hz);
        std::array<float,kBand> sr{},si{},magnitude{};
        const double average=1.0/std::min<double>(frames_+1,2.0*studio::kRate/kHop);   // cumulative at first, then 2 s
        for(unsigned j=0;j<kBand;++j){
            sr[j]=re[k0-kBand+j];si[j]=im[k0-kBand+j];magnitude[j]=std::sqrt(sr[j]*sr[j]+si[j]*si[j]);
            level_[j]+=float((double(magnitude[j])*magnitude[j]-level_[j])*average);
        }
        for(unsigned j=0;j<kBand;++j){
            std::array<float,5> around{};
            for(int d=-2;d<=2;++d)around[size_t(d+2)]=magnitude[size_t(std::clamp(int(j)+d,0,int(kBand)-1))];
            std::nth_element(around.begin(),around.begin()+2,around.end());
            const float cap=std::min(1.f,4*around[2]/std::max(magnitude[j],1e-30f));
            sr[j]*=cap;si[j]*=cap;
            capped_level_[j]+=float((double(sr[j])*sr[j]+double(si[j])*si[j]-capped_level_[j])*average);
        }
        const auto across=[](const std::array<float,kBand>& power_of,unsigned j){   // amplitude, smoothed over 7 bins
            float sum=0;for(int d=-3;d<=3;++d)sum+=std::sqrt(power_of[size_t(std::clamp(int(j)+d,0,int(kBand)-1))]);return sum/7;};
        float reference=0;unsigned counted=0;
        for(unsigned j=kBand*45/100;j<kBand*80/100;++j,++counted)reference+=across(level_,j);
        reference/=float(counted);
        for(unsigned k=start;k<end;++k){
            const unsigned j=(k-start)%kBand;const float own=across(capped_level_,j);
            if(own<1e-12f)continue;
            const double db=settings_.air_db-settings_.air_slope_db_khz*(double(k)-k0)*hz/1000;
            const float gain=float(std::pow(10,db/20)*std::min(1.0,double(k-start)/fade))*reference/own;
            re[k]+=sr[j]*gain;im[k]+=si[j]*gain;
        }
        for(unsigned k=end;k<kBins;++k)re[k]=im[k]=0;
        for(unsigned k=1;k<kFrame/2;++k){re[kFrame-k]=re[k];im[kFrame-k]=-im[k];}
        frame_fft_.Run(re.data(),im.data(),true);
        // Square-root Hann on both sides at 75% overlap sums to 2.
        for(unsigned n=0;n<kFrame;++n)overlap_[n]+=re[n]*window_[n]*.5f;
        std::copy_n(overlap_.begin(),kHop,ready_.begin());
        std::move(overlap_.begin()+kHop,overlap_.end(),overlap_.begin());
        std::fill(overlap_.end()-kHop,overlap_.end(),0.f);
        ++frames_;
    }
    double Spectral(float sample) {
        const float out=ready_[hop_at_];
        std::move(input_.begin()+1,input_.end(),input_.begin());input_.back()=sample;
        if(++hop_at_==kHop){hop_at_=0;Frame();}
        return out;
    }
    void RoomBlock() {
        std::array<float,2*kBlock> re{},im{};
        std::copy(room_input_.begin(),room_input_.end(),re.begin());
        block_fft_.Run(re.data(),im.data(),false);
        const unsigned bins=kBlock+1;
        ring_at_=(ring_at_+partitions_-1)%partitions_;
        std::copy_n(re.begin(),bins,ring_re_.begin()+ring_at_*bins);std::copy_n(im.begin(),bins,ring_im_.begin()+ring_at_*bins);
        for(unsigned channel=0;channel<2;++channel){
            std::array<float,2*kBlock> yr{},yi{};
            for(unsigned p=0;p<partitions_;++p){
                const float* xr=ring_re_.data()+((ring_at_+p)%partitions_)*bins;const float* xi=ring_im_.data()+((ring_at_+p)%partitions_)*bins;
                const float* hr=room_re_[channel].data()+p*bins;const float* hi=room_im_[channel].data()+p*bins;
                for(unsigned k=0;k<bins;++k){yr[k]+=xr[k]*hr[k]-xi[k]*hi[k];yi[k]+=xr[k]*hi[k]+xi[k]*hr[k];}
            }
            for(unsigned k=1;k<kBlock;++k){yr[2*kBlock-k]=yr[k];yi[2*kBlock-k]=-yi[k];}
            block_fft_.Run(yr.data(),yi.data(),true);
            std::copy_n(yr.begin()+kBlock,kBlock,wet_[channel].begin());   // overlap-save: the second half is valid
        }
        std::move(room_input_.begin()+kBlock,room_input_.end(),room_input_.begin());
    }
public:
    explicit StudioMaster(const StudioSettings& settings):settings_(settings) {
        if(settings_.band_end_hz && (settings_.band_end_hz<6000 || settings_.band_end_hz>16000))throw std::invalid_argument("studio band end");
        for(unsigned n=0;n<kFrame;++n)window_[n]=float(std::sqrt(.5*(1-std::cos(2*studio::kPi*n/kFrame))));
        bass_=studio::Biquad::Shelf(95,settings_.bass_db,true);tilt_=studio::Biquad::Shelf(2600,settings_.tilt_db,false,.6);
        mud_=studio::Biquad::Bell(330,-1.0,1.0);side_pass_=studio::Biquad::HighPass(250);dc_=studio::Biquad::HighPass(20);
        // Velvet noise: one +-1 impulse per 26.7-sample cell over 30 ms, decaying, unit energy.
        studio::Lcg random{7};const double cell=studio::kRate/1800;double energy=0;
        for(unsigned m=0;m<unsigned(kVelvet/cell);++m){
            const auto at=unsigned(m*cell+random.Uniform()*(cell-1));const double gain=(random.Uniform()<.5?1:-1)*std::exp(-3.0*at/kVelvet);
            velvet_at_.push_back(uint16_t(at));velvet_gain_.push_back(float(gain));energy+=gain*gain;
        }
        for(auto& gain:velvet_gain_)gain=float(gain/std::sqrt(energy)*std::pow(10,settings_.side_db/20));
        // Room: seven early reflections and a damped diffuse tail per channel, unit energy; the block delay of the
        // convolution is taken out of the pre-delay.
        const unsigned tail=unsigned((kRt60*1.3+kPreDelay)*studio::kRate),lead=unsigned(kPreDelay*studio::kRate)-kBlock;
        partitions_=(lead+tail+kBlock-1)/kBlock;
        const unsigned bins=kBlock+1;
        ring_re_.assign(size_t(partitions_)*bins,0);ring_im_.assign(size_t(partitions_)*bins,0);
        for(unsigned channel=0;channel<2;++channel){
            studio::Lcg noise{11+channel*101};std::vector<double> h(size_t(partitions_)*kBlock,0);
            double low=0,high=0,previous=0;const double low_keep=std::exp(-2*studio::kPi*6500/studio::kRate),high_keep=std::exp(-2*studio::kPi*180/studio::kRate);
            for(unsigned n=0;n<tail;++n){
                double value=noise.Gauss()*std::pow(10,-3.0*n/(kRt60*studio::kRate))*std::min(1.0,n/(.025*studio::kRate));
                low=low*low_keep+value*(1-low_keep);high=high_keep*(high+low-previous);previous=low;   // 180 Hz..6.5 kHz
                h[lead+n]=high*.9;
            }
            for(unsigned e=0;e<7;++e){
                const auto at=unsigned((.004+noise.Uniform()*.024)*studio::kRate);
                h[lead+at]+=(.8-.55*e/6)*(noise.Uniform()<.5?1:-1);
            }
            double sum=0;for(const double v:h)sum+=v*v;
            const double scale=std::pow(10,settings_.wet_db/20)/std::sqrt(sum);
            room_re_[channel].assign(size_t(partitions_)*bins,0);room_im_[channel].assign(size_t(partitions_)*bins,0);
            for(unsigned p=0;p<partitions_;++p){
                std::array<float,2*kBlock> re{},im{};
                for(unsigned n=0;n<kBlock;++n)re[n]=float(h[size_t(p)*kBlock+n]*scale);
                block_fft_.Run(re.data(),im.data(),false);
                std::copy_n(re.begin(),bins,room_re_[channel].begin()+size_t(p)*bins);std::copy_n(im.begin(),bins,room_im_[channel].begin()+size_t(p)*bins);
            }
        }
    }
    // Samples by which the output trails the input (the STFT of the DCS sources).
    unsigned Latency()const{return settings_.band_end_hz?kFrame:0;}
    std::array<float,2> Process(float sample) {
        const float input=float(dc_.Run(sample));
        double mid=settings_.band_end_hz?Spectral(input):input;
        mid=mud_.Run(tilt_.Run(bass_.Run(mid)));
        // Side: the band above 250 Hz through the velvet decorrelator.
        side_line_[side_at_]=float(side_pass_.Run(mid));
        double side=0;
        for(size_t t=0;t<velvet_at_.size();++t)side+=side_line_[(side_at_+kVelvet-velvet_at_[t])%kVelvet]*velvet_gain_[t];
        side_at_=(side_at_+1)%kVelvet;
        // Room: this block's wet was computed from the blocks before it.
        const double wet_left=wet_[0][block_at_],wet_right=wet_[1][block_at_];
        room_input_[kBlock+block_at_]=float(mid);
        if(++block_at_==kBlock){block_at_=0;RoomBlock();}
        double left=mid+side+wet_left,right=mid-side+wet_right;
        // Bus compressor: 2:1 from 3 dB above the programme's own level (3 s average), 6 dB knee, 30 ms / 250 ms.
        static const double detector_keep=std::exp(-1/(studio::kRate*.010)),attack=std::exp(-1/(studio::kRate*.030)),release=std::exp(-1/(studio::kRate*.250));
        detector_=detector_*detector_keep+(left*left+right*right)/2*(1-detector_keep);
        if(detector_>1e-10){programme_count_=std::min<uint32_t>(programme_count_+1,uint32_t(3*studio::kRate));programme_+=(detector_-programme_)/programme_count_;}
        double wanted=0;
        if(programme_>0 && detector_>0){
            const double over=10*std::log10(detector_/programme_)-3.0,knee=6.0;
            wanted=over<=-knee/2?0:over>=knee/2?over*.5:(over+knee/2)*(over+knee/2)/(2*knee)*.5;
        }
        reduction_db_=wanted>reduction_db_?attack*reduction_db_+(1-attack)*wanted:release*reduction_db_+(1-release)*wanted;
        const double gain=std::pow(10,-reduction_db_/20);
        return {float(left*gain),float(right*gain)};
    }
};
}
