#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <emmintrin.h>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace mk5stage {
inline constexpr uint32_t scenery=0x80000000;
inline constexpr unsigned scale=4;

// Rows [0, count) in contiguous bands on up to kBands threads (the caller's one included): each row is written by one band
// only, so the result is the single-threaded one, pixel for pixel. A small count stays on the caller's thread.
// The other bands run on workers that live as long as the process (started with the first call): a frame makes dozens of
// these calls, and a thread started and joined for each band of each call cost more than its rows and kept the scheduler
// busy beside the game's own thread. One call at a time uses them; a call made from inside a band runs on its thread.
inline constexpr unsigned kBands=4,kBandRows=64;
class RowWorkers{
    std::mutex call,state;std::condition_variable wake,done;
    void(*job)(void*,unsigned)=nullptr;void* context=nullptr;unsigned bands=0,generation=0,pending=0;
    void Work(unsigned band){
        std::unique_lock<std::mutex> lock(state);
        for(unsigned seen=0;;){
            wake.wait(lock,[&]{return generation!=seen;});seen=generation;
            if(band>=bands)continue;
            const auto run=job;const auto with=context;
            lock.unlock();inside()=true;run(with,band);inside()=false;lock.lock();
            if(!--pending)done.notify_one();
        }
    }
    RowWorkers(){for(unsigned band=1;band<kBands;++band)std::thread([this,band]{Work(band);}).detach();}
public:
    static bool& inside(){thread_local bool in_band=false;return in_band;}
    static RowWorkers& Get(){static RowWorkers* const workers=new RowWorkers;return *workers;}   // never destroyed: its threads end with the process
    void Run(unsigned count,void(*run)(void*,unsigned),void* with){
        std::lock_guard<std::mutex> one(call);
        {std::lock_guard<std::mutex> lock(state);job=run;context=with;bands=count;pending=count-1;++generation;}
        wake.notify_all();
        inside()=true;run(with,0);inside()=false;
        std::unique_lock<std::mutex> lock(state);done.wait(lock,[&]{return !pending;});bands=0;
    }
};
template<class Row> void ForRows(unsigned count,Row&& row){
    const unsigned bands=RowWorkers::inside()?1u:std::min({kBands,std::max(1u,std::thread::hardware_concurrency()),std::max(1u,count/kBandRows)});
    const auto run=[&](unsigned band){for(unsigned y=count*band/bands;y<count*(band+1)/bands;++y)row(y);};
    if(bands<2){run(0);return;}
    RowWorkers::Get().Run(bands,[](void* with,unsigned band){(*static_cast<decltype(run)*>(with))(band);},const_cast<void*>(static_cast<const void*>(&run)));
}

struct CubicTap {
    std::array<unsigned,4> index;
    std::array<int,4> weight;
    uint32_t first_pair=0,last_pair=0;
    bool interior=false;
};

inline std::vector<CubicTap> BicubicAxis(unsigned size){
    constexpr int precision=1<<22;
    std::vector<CubicTap> axis(size*scale);
    for(unsigned x=0;x<axis.size();++x){
        const int phase=int(x%scale);
        const int base=int(x/scale)-(phase<2?1:0);
        const int f=(2*phase+5)&7;
        // Catmull-Rom at the four quarter-pixel centres. Raw weights sum to 1024.
        std::array<int,4> weights{-f*(8-f)*(8-f),1024+f*f*(3*f-40),
                                  f*(64+f*(32-3*f)),-f*f*(8-f)};
        auto& tap=axis[x];
        int sum=0;
        for(unsigned i=0;i<4;++i){
            const int at=base+int(i)-1;
            tap.index[i]=unsigned(std::clamp(at,0,int(size)-1));
            if(at<0 || at>=int(size))weights[i]=0;
            sum+=weights[i];
        }
        for(unsigned i=0;i<4;++i){
            const auto weight=weights[i];
            tap.weight[i]=int((int64_t(weight)*precision+(weight<0?-sum/2:sum/2))/sum);
        }
        tap.interior=sum==1024;
        tap.first_pair=uint16_t(weights[0])|(uint32_t(uint16_t(weights[1]))<<16);
        tap.last_pair=uint16_t(weights[2])|(uint32_t(uint16_t(weights[3]))<<16);
    }
    return axis;
}

template<unsigned channels>
inline uint32_t BicubicPixel(uint32_t a,uint32_t b,uint32_t c,uint32_t d,const CubicTap& tap){
    static_assert(channels==3 || channels==4);
    constexpr uint32_t mask=channels==3?0x00ffffff:0xffffffff;
    if(tap.interior){
        // Exact dyadic weights fit the x64 SSE2 paired 16-bit multiply-add.
        const auto pairs=[](uint32_t x,uint32_t y){
            return _mm_unpacklo_epi8(_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(x)),_mm_cvtsi32_si128(int(y))),
                                     _mm_setzero_si128());
        };
        const auto ab=_mm_madd_epi16(pairs(a,b),_mm_set1_epi32(int(tap.first_pair)));
        const auto cd=_mm_madd_epi16(pairs(c,d),_mm_set1_epi32(int(tap.last_pair)));
        const auto result=_mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ab,cd),_mm_set1_epi32(512)),10);
        return uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(result,result),result)))&mask;
    }
    const auto& weights=tap.weight;
    uint32_t rgb=0;
    for(unsigned shift=0;shift<channels*8;shift+=8){
        const int value=(1<<21)+int((a>>shift)&255)*weights[0]+int((b>>shift)&255)*weights[1]
                             +int((c>>shift)&255)*weights[2]+int((d>>shift)&255)*weights[3];
        rgb|=uint32_t(std::clamp(value,0,255<<22)>>22)<<shift;
    }
    return rgb;
}

inline uint32_t BicubicRgb(uint32_t a,uint32_t b,uint32_t c,uint32_t d,const CubicTap& tap){
    return BicubicPixel<3>(a,b,c,d,tap);
}

template<unsigned channels>
inline __m128i BicubicTile(const uint32_t* a,const uint32_t* b,const uint32_t* c,const uint32_t* d,const CubicTap& tap){
    if(!tap.interior)
        return _mm_setr_epi32(int(BicubicPixel<channels>(a[0],b[0],c[0],d[0],tap)),
                             int(BicubicPixel<channels>(a[1],b[1],c[1],d[1],tap)),
                             int(BicubicPixel<channels>(a[2],b[2],c[2],d[2],tap)),
                             int(BicubicPixel<channels>(a[3],b[3],c[3],d[3],tap)));
    const auto zero=_mm_setzero_si128();
    const auto first=_mm_set1_epi32(int(tap.first_pair)),last=_mm_set1_epi32(int(tap.last_pair));
    const auto load=[](const uint32_t* row){return _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));};
    const auto combine=[&](const __m128i ab,const __m128i cd){
        const auto a=_mm_madd_epi16(ab,first),b=_mm_madd_epi16(cd,last);
        return _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(a,b),_mm_set1_epi32(512)),10);
    };
    const auto row_a=load(a),row_b=load(b),row_c=load(c),row_d=load(d);
    const auto pack_pair=[&](const __m128i ab,const __m128i cd){
        return _mm_packs_epi32(combine(_mm_unpacklo_epi8(ab,zero),_mm_unpacklo_epi8(cd,zero)),
                              combine(_mm_unpackhi_epi8(ab,zero),_mm_unpackhi_epi8(cd,zero)));
    };
    const auto result=_mm_packus_epi16(pack_pair(_mm_unpacklo_epi8(row_a,row_b),_mm_unpacklo_epi8(row_c,row_d)),
                                       pack_pair(_mm_unpackhi_epi8(row_a,row_b),_mm_unpackhi_epi8(row_c,row_d)));
    if constexpr(channels==3)return _mm_and_si128(result,_mm_set1_epi32(0xffffff));
    else return result;
}

// Into result (every pixel written; a result of the same size keeps its memory: a frame's buffers are reused, not faulted in
// again each frame).
inline void Bicubic4x(const std::vector<uint32_t>& original,const std::vector<uint32_t>& backdrop,unsigned width,unsigned height,
                      std::vector<uint32_t>& result){
    if(!width || !height || original.size()!=size_t(width)*height || backdrop.size()!=original.size())
        throw std::logic_error("MK5 bicubic backdrop geometry");
    const unsigned output_width=width*scale;
    const auto horizontal_taps=BicubicAxis(width);
    const auto vertical_taps=BicubicAxis(height);
    thread_local std::vector<uint32_t> scratch;   // the caller's: the bands reach it through this reference
    auto& horizontal=scratch;horizontal.resize(size_t(output_width)*height);
    ForRows(height,[&](unsigned y){
        const auto* row=backdrop.data()+size_t(y)*width;
        auto* target=horizontal.data()+size_t(y)*output_width;
        for(unsigned x=0;x<output_width;++x){
            const auto& tap=horizontal_taps[x];
            target[x]=BicubicRgb(row[tap.index[0]],row[tap.index[1]],row[tap.index[2]],row[tap.index[3]],tap);
        }
    });
    result.resize(size_t(output_width)*height*scale);
    ForRows(height*scale,[&](unsigned y){
        const auto& tap=vertical_taps[y];
        const auto* a=horizontal.data()+size_t(tap.index[0])*output_width;
        const auto* b=horizontal.data()+size_t(tap.index[1])*output_width;
        const auto* c=horizontal.data()+size_t(tap.index[2])*output_width;
        const auto* d=horizontal.data()+size_t(tap.index[3])*output_width;
        const auto base=size_t(y/scale)*width;
        auto* target=result.data()+size_t(y)*output_width;
        for(unsigned x=0;x<width;++x){
            const unsigned left=x*scale;
            if(backdrop[base+x]&scenery){
                const auto rgb=BicubicTile<3>(a+left,b+left,c+left,d+left,tap);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(target+left),_mm_or_si128(rgb,_mm_set1_epi32(int(scenery))));
            }else{
                std::fill_n(target+left,scale,original[base+x]&0xffffff);
            }
        }
    });
}
inline std::vector<uint32_t> Bicubic4x(const std::vector<uint32_t>& original,
        const std::vector<uint32_t>& backdrop,unsigned width,unsigned height){
    std::vector<uint32_t> result;Bicubic4x(original,backdrop,width,height,result);return result;
}
}
