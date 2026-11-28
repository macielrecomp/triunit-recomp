#pragma once
// Widescreen (Remastered presentation only) shared by the games whose VRAM line has 512 columns with the 400 visible ones
// from column 56 (MK1 rev 5.0, MK2 L3.1). The game runs unchanged: a shadow pass, run by the generated code on journaled
// RAM with no cycles, runs the frame's display-list walk again with the game's clip widened, and its draws land at their
// own signed screen x in a 16:9 layer of its own (534 columns: the arcade's 400 and 67 on each side, more than a VRAM line
// holds; no column wraps to the other side). The arcade's 400 columns stay the arcade's own pixels. A margin shows only
// what the game's draws put there, and the page's background in a picture whose arcade columns show it (a sky); where a
// scenery layer has no art the margin is masked (black), never filled.
// Game facts (addresses) stay in each game's table (mk5_widescreen.hpp, mk2/mk2_widescreen.hpp).
// Layer index: VRAM row (both 256-line pages) * kLayerWidth + layer column; layer column L shows screen x L-67.
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace mk5wide {
constexpr int kMargin=56;      // VRAM columns beyond each side of the visible ones
constexpr int kVisible=400;
constexpr int kWidth=512;      // a VRAM line
constexpr int kWideMargin=67;  // layer columns beyond each side of the visible ones: 534/400 x 4:3 = 16:9
constexpr int kLayerWidth=kVisible+2*kWideMargin;
constexpr int kExtra=kWideMargin-kMargin;   // VRAM column c is layer column c+kExtra
constexpr unsigned kAcross=16,kDown=9;      // the wide frame's physical aspect
constexpr size_t kLayerSize=size_t(512)*kLayerWidth;
constexpr int kReach=112;      // the shadow's clip reaches this far past the layer: objects partly inside it are built
// A draw's x register in layer columns: the shadow renders its draws with this x, the layer's stride and its clip.
constexpr uint16_t LayerX(uint16_t x){return uint16_t((x+kExtra)&0x3ff);}
// A DMA draw's columns [first, last) from its x register (the board reads 10 bits, here as -256..767: the bits above them
// are the game's flags), width, X step and flip, as the columns follow one another (the board's blitter wraps them at 1024).
inline std::pair<int,int> Span(uint16_t x_register,uint16_t width_register,uint16_t xstep_register,bool flip){
    int x=x_register&0x3ff;if(x>=768)x-=1024;
    const unsigned xstep=xstep_register?xstep_register:0x100;   // a source column every xstep/256: ceil(width*256/xstep) of them
    const int w=int((((width_register&0x3ffu)<<8)+xstep-1)/xstep),lo=flip?x-w+1:x;
    return {lo,lo+w};
}
// A DMA draw's rows [first, last) of the 512 (both pages) from its y register, height, Y step and vertical flip, within its
// clip rows; the rows the board's blitter visits (Blit/Render: y steps by one row per source row, modulo 512).
inline std::pair<int,int> Rows(const std::array<uint16_t,18>& r){
    const unsigned ystep=r[11]?r[11]:0x100;const int h=int(((r[7]&0x3ffu)*256+ystep-1)/ystep),y=r[5]&0x1ff;
    const int lo=(r[1]&0x20)?y-h+1:y;return {std::max(lo,int(r[12]&0x1ff)),std::min(lo+h,int(r[13]&0x1ff)+1)};
}
// The wide window's offset d at an arena's ends: the wide screen shows the world as the arcade would at camera x + d, and
// its camera is the arcade's limited to [minimum+67, maximum-67], so the 16:9 picture never passes the world's ends (the
// arcade screen stands at them at each camera limit). No ramp, no lag: d depends on the camera alone. An arena narrower
// than that keeps its world centred. The arcade's camera passes its limits by a few columns (3 seen), its own screen then
// showing the page's background past the art's end: the wide camera stays on its limit meanwhile (kOvershoot columns; a
// camera further out takes the window along).
constexpr int kOvershoot=8;
inline int WindowShift(int camera,int minimum,int maximum){
    if(minimum>maximum)return 0;
    const int low=minimum+kWideMargin,high=maximum-kWideMargin;
    if(low>high)return (minimum+maximum)/2-std::clamp(camera,minimum,maximum);
    return std::clamp(std::clamp(camera,low,high)-camera,-kWideMargin-kOvershoot,kWideMargin+kOvershoot);
}
constexpr int kWorldMax=4096;  // camera limits wider than this (a finisher frees the camera) keep the arena's last ones
// A far plane's parallax: its velocity and the world's in a frame where the world moved (world 0: not known yet).
struct Ratio{int64_t plane=0,world=0;
    template<class A> friend void Fields(A& a,Ratio& r){a(r.plane,r.world);}};
inline Ratio MeasuredRatio(int32_t velocity,int32_t world_velocity){
    return world_velocity>0?Ratio{velocity,world_velocity}:Ratio{-int64_t(velocity),-int64_t(world_velocity)};
}
// The window offset as the world scroll's move (16.16): with offset d the shadow's world camera is the whole column
// camera + d, whatever fraction the arcade's scroll carries (so nothing trembles while the arcade's camera moves under a
// wide camera that stands at its limit). whole: the arcade's camera stands on a limit of the wide one, with no offset
// yet (Layer::AtLimit): a column further on the offset drops the fraction, and a far plane would move by its share of it
// under a wide camera that stays on the same column.
inline int64_t ScrollOffset(int32_t world_scroll,int d,bool whole=false){return d||whole?(int64_t((world_scroll>>16)+d)<<16)-world_scroll:0;}
// Where a pixel of the wide layer came from (diagnostics, Layer::debug): the kind of write and the columns its draw covers
// in layer columns as they follow one another from its x (Span): a pixel outside them got there by a wrap.
enum class Kind:uint8_t{none,real,shadow,cpu,clear,generated,continued};   // continued: Layer::MaskBands
struct Origin{int16_t lo=0,hi=0;Kind kind=Kind::none;uint8_t page=0;
    bool Covers(int column)const{return column>=lo&&column<hi;}
    template<class A> friend void Fields(A& a,Origin& o){a(o.lo,o.hi,o.kind,o.page);}};
constexpr uint32_t kSentinel=0xffff00ff;   // magenta: a margin pixel no draw wrote (Layer::debug)
constexpr uint32_t kMasked=0xff000000;     // black: a margin pixel without art (Layer::Shown)
// A shown frame's margin pixels audited (Layer::AuditRow, debug): shown (all of them), sentinel (no draw wrote it), masked
// (no art: Layer::Shown), outside (the draw that wrote it never meant to cover its column: a wrap, a copy, a move),
// generated (written by no draw of the game), stale (on a line the frame rewrote across the arcade's 400 columns, written
// before the frame).
// filled (the scanouts): a pixel of the arcade's columns whose pen is not the arcade's, written this frame by a draw of the
// shadow pass meant to cover it (an object the arcade has not built yet: the real walk never drew it).
struct Audit{uint64_t shown=0,sentinel=0,masked=0,outside=0,outside_real=0,generated=0,stale=0,filled=0;   // outside_real: of outside, a real DMA's
    void Add(const Audit& o){shown+=o.shown;sentinel+=o.sentinel;masked+=o.masked;outside+=o.outside;outside_real+=o.outside_real;generated+=o.generated;stale+=o.stale;filled+=o.filled;}};
// A scenery layer's draw in a build (debug, the parallax check): its layer, image source and row, its columns (layer
// columns) and its list's camera x (scenery moves by minus the camera's move; an object with a velocity of its own does not).
struct Note{uint16_t layer;uint32_t source;uint16_t row;int16_t lo,hi,camera;};
// A staged draw as placed on the layer (debug): its columns (layer columns), first row and rows, image source, control
// word, whether it is screen-fixed and its scenery layer (-1: none).
struct Placed{int16_t lo,hi;uint16_t row,rows;uint32_t source;uint16_t control;bool fixed;int16_t layer;};
// A scenery layer's art in a build (Layer::Band): per line its pixels cover all of the arcade's 400 columns on, the columns
// of the margins without its art (left: [0, left), right: [right, kLayerWidth)); a side whose draws reach past the layer
// keeps all of it. first: the serial of the layer's first draw in the layer (0: not drawn yet); farther pixels are older.
struct BandRow{uint16_t row;int16_t left,right;template<class A> friend void Fields(A& a,BandRow& b){a(b.row,b.left,b.right);}};
// A band line's art at one of its ends, as the layer held it when the band's last draw was in and nothing nearer was
// (Layer::Ends): the column of its outermost pixel there (edge) and, from it inwards, kEndPens pens (pen[k]: k columns
// in), each with a bit in own when it is a pixel of the band's art. dither: its last columns repeat every two. measured:
// the line is one of the band's rows (else it only ends on the others' straight edge). What goes on from that end into
// the margin (Layer::MaskBands) is taken from here: whatever stands over the end afterwards (a pillar passing by, a
// fighter, a flying object) is no part of the band's pattern and must not be drawn out across the margin.
constexpr int kEndPens=24;
struct BandEnd{uint16_t row=0;int16_t edge=0;uint8_t right=0,dither=0,measured=0;uint32_t own=0;std::array<uint16_t,kEndPens> pen{};
    template<class A> friend void Fields(A& a,BandEnd& e){a(e.row,e.edge,e.right,e.dither,e.measured,e.own,e.pen);}};
struct Band{uint16_t layer=0;uint32_t first=0;std::vector<BandRow> rows;uint16_t full=0;   // full: its lines that cover the arcade's columns whole
    std::vector<BandEnd> ends;std::array<uint8_t,2> gradient{};uint8_t ended=0;   // Layer::Ends (saved by Layer::EvenFields)
    template<class A> friend void Fields(A& a,Band& b){a(b.layer,b.first,b.rows,b.full);}};
inline bool ArcadeColumn(int L){return L>=kWideMargin&&L<kWideMargin+kVisible;}
// A screen without an arena whose margins the game does not fill (a menu, a story picture: Layer::page_soft) shows soft
// sides instead of pieces of art and black: each margin line takes the colour of the arcade line's own end (its outermost
// kSoftColumns columns, past at most kSoftSkip black ones: a picture a few columns short of the screen's edge), blurred
// over kSoftRows lines up and down and dimmed towards the screen's edge. Nothing of the picture is copied: no object can
// appear twice. join: the soft side starts where the line's picture ends: the black columns skipped before it take the
// side's first colour (no black line between the picture and its side; MK1, whose select screen ends short on the right).
// short_most: the black columns a line's picture may stop short of the arcade's edge (MK3: a picture 16 columns narrower
// than the screen, the power-on advisory's bars).
// frame: kLayerWidth columns by height lines, 0xAARRGGBB; touched(index) for each pixel written.
constexpr int kSoftColumns=4,kSoftRows=10,kSoftSkip=12;
template<class Touched> void SoftMargins(uint32_t* frame,unsigned height,bool join,Touched&& touched,int short_most=kSoftSkip){
    for(int side=0;side<2;++side){
        std::vector<std::array<unsigned,3>> edge(height);std::vector<int> gap(height);   // each line's end colour; its picture's columns short of the edge
        for(unsigned y=0;y<height;++y){
            std::array<unsigned,3> sum{};
            const auto at=[&](int k){return frame[size_t(y)*kLayerWidth+size_t(side?kWideMargin+kVisible-1-k:kWideMargin+k)];};
            int skip=0;while(skip<short_most&&!(at(skip)&0xffffffu))++skip;
            gap[y]=join&&(at(skip)&0xffffffu)?skip:0;   // a black line is not a short picture
            for(int k=skip;k<skip+kSoftColumns;++k){
                const uint32_t p=at(k);
                sum[0]+=p>>16&255;sum[1]+=p>>8&255;sum[2]+=p&255;
            }
            edge[y]={sum[0]/unsigned(kSoftColumns),sum[1]/unsigned(kSoftColumns),sum[2]/unsigned(kSoftColumns)};
        }
        for(unsigned y=0;y<height;++y){
            std::array<unsigned,3> sum{};unsigned n=0;
            for(int dy=-kSoftRows;dy<=kSoftRows;++dy)if(const int at=int(y)+dy;at>=0&&at<int(height)){for(int c=0;c<3;++c)sum[size_t(c)]+=edge[size_t(at)][size_t(c)];++n;}
            for(int k=-gap[y];k<kWideMargin;++k){   // k: columns from the arcade's edge (below 0: inside it, the picture's black end)
                const unsigned light=150-90u*unsigned(std::max(k,0))/unsigned(kWideMargin);   // of 256: 59% beside the picture, 23% at the screen's edge
                const uint32_t rgb=(sum[0]/n*light>>8)<<16|(sum[1]/n*light>>8)<<8|(sum[2]/n*light>>8);
                const size_t to=size_t(y)*kLayerWidth+size_t(side?kWideMargin+kVisible+k:kWideMargin-1-k);
                frame[to]=0xff000000u|rgb;touched(to);
            }
        }
    }
}
// One scenery layer's draws measured (layer registers, x in layer columns; meant: each draw's columns as the game meant
// them): render(regs, put) calls put(layer index) for each pixel the draw writes. Its lines whose arcade columns it covers
// whole, each with its art's ends in the margins (its outermost pixels on the line; a side where a draw reaches past the
// layer keeps all of its columns).
// The draws may be measured over a wider canvas (width columns, the layer's column 0 at its column origin; draws, meant and
// render in canvas columns): the ends are then known that far past the layer (BandRow in layer columns all the same).
// Sets the bits of the arcade columns [a,b) in a line's words (the kVisible columns: bit 0 is the first).
inline void SetColumns(uint64_t* words,int a,int b){
    for(int c=std::max(a,0),end=std::min(b,int(kVisible));c<end;){
        const int stop=std::min(end,(c|63)+1);
        words[c>>6]|=(stop-c==64?~uint64_t(0):(uint64_t(1)<<(stop-c))-1)<<(c&63);c=stop;
    }
}
// A draw's own pixels as runs of columns per line ([a,b), counted from its x), kept by its registers but its x: a
// scenery draw comes again every frame, a few columns on, and measuring it is then these runs instead of a render.
// Only a draw whole inside the canvas is kept (its columns do not depend on its place) and only one the board drew
// whole (render returned true). Not state.
struct MeasureCache{
    struct Run{uint16_t row;int16_t a,b;bool operator==(const Run& o)const{return row==o.row&&a==o.a&&b==o.b;}};
    struct Hash{size_t operator()(const std::array<uint16_t,18>& r)const{uint64_t h=0xcbf29ce484222325ull;for(const uint16_t v:r)h=(h^v)*0x100000001b3ull;return size_t(h);}};
    std::unordered_map<std::array<uint16_t,18>,std::vector<Run>,Hash> shapes;size_t runs=0;
    static constexpr size_t kRuns=size_t(1)<<21;   // kept runs: past them the cache starts over
    static constexpr int kInside=8;                // columns a kept draw stays clear of the canvas's ends
};
// render(draw, put) gives the draw's pixels (put(index), index = row * width + column) and returns whether the board
// drew it whole; seen(row, a, b) takes every run of a line's own columns [a,b), rendered or kept. verify (the
// acceptance): a kept draw is rendered too and must give the runs kept.
template<class Render,class Seen> Band MeasureBand(uint16_t layer,const std::vector<std::array<uint16_t,18>>& draws,
                                                   const std::vector<std::pair<int,int>>& meant,Render&& render,Seen&& seen,
                                                   int width=kLayerWidth,int origin=0,MeasureCache* cache=nullptr,bool verify=false){
    constexpr size_t kWords=(kVisible+63)/64;
    std::vector<uint64_t> bits(512*kWords,0);
    std::vector<int16_t> lo(512,int16_t(width)),hi(512,0);std::vector<uint8_t> reach(512,0);
    const size_t stride=size_t(width);
    std::vector<MeasureCache::Run> made;
    for(size_t k=0;k<draws.size();++k){
        const auto [first,last]=Rows(draws[k]);
        const uint8_t past=uint8_t((meant[k].first<0?1:0)|(meant[k].second>width?2:0));
        if(past)for(int y=std::max(first,0);y<std::min(last,512);++y)reach[size_t(y)]|=past;
        int x=draws[k][4]&0x3ff;if(x>=768)x-=1024;
        const bool keep=cache&&meant[k].first>=MeasureCache::kInside&&meant[k].second<=width-MeasureCache::kInside;
        auto key=draws[k];key[4]&=uint16_t(~0x3ffu);
        const std::vector<MeasureCache::Run>* runs=nullptr;
        if(keep)if(const auto it=cache->shapes.find(key);it!=cache->shapes.end())runs=&it->second;
        if(!runs||verify){
            // The blitter gives a line's pixels one after another: they are gathered into runs (no division and no
            // store per pixel).
            made.clear();size_t row=512,base=0;int a=0,b=0,at=0;
            const auto flush=[&]{if(row<512)made.push_back({uint16_t(row),int16_t(a-x),int16_t(b+1-x)});};
            const bool drawn=render(draws[k],[&](size_t index){
                const size_t column=index-base;
                if(row<512&&column<stride&&(int(column)==at+1||int(column)==at-1)){at=int(column);a=std::min(a,at);b=std::max(b,at);return;}
                flush();row=index/stride;base=row*stride;a=b=at=int(index-base);
            });
            flush();
            if(runs&&made!=*runs)throw std::logic_error("mk5wide: a kept draw's runs differ from its render");
            if(!runs&&keep&&drawn){
                if(cache->runs>MeasureCache::kRuns){cache->shapes.clear();cache->runs=0;}
                cache->runs+=made.size();runs=&(cache->shapes[key]=made);
            }
            if(!runs)runs=&made;
        }
        for(const auto& r:*runs){
            const int a=r.a+x,b=r.b+x;const size_t row=r.row;
            lo[row]=std::min(lo[row],int16_t(a));hi[row]=std::max(hi[row],int16_t(b));
            SetColumns(&bits[row*kWords],a-origin-kWideMargin,b-origin-kWideMargin);
            seen(row,a,b);
        }
    }
    Band band;band.layer=layer;
    for(uint16_t y=0;y<512;++y){
        int covered=0;for(size_t w=0;w<kWords;++w)covered+=__builtin_popcountll(bits[size_t(y)*kWords+w]);
        if(covered!=kVisible)continue;
        ++band.full;
        const int16_t left=int16_t(((reach[y]&1)?0:lo[y])-origin),right=int16_t(((reach[y]&2)?width:hi[y])-origin);
        if(left>-origin||right<width-origin)band.rows.push_back({y,left,right});
    }
    return band;
}

// The wide frame over the two 256-line VRAM pages: pens (the VRAM's format), the floor stains over it (Blood: the game's
// blood layer element, 0 = none) and per pixel its marks (fixed: kFixed a screen-fixed (HUD) draw's, kActor a floor
// shadow's, kFighter a fighter's, kMask a margin pixel without art) and its writer's serial.
template<class Blood>
struct Layer {
    std::vector<uint16_t> pens;std::vector<Blood> blood;std::vector<uint8_t> fixed;
    // kClear: the page's background, what its clear left there with no draw since.
    static constexpr uint8_t kFixed=1,kActor=2,kFighter=4,kMask=8,kClear=16;   // fixed: bits
    // A constant-colour draw (DMA control's low nibble 8: a fighter's floor shadow) is an actor's, not the scene's.
    static bool ActorDraw(uint16_t control){return (control&15)==8;}
    // Each draw that writes the layer takes the next serial (Draw); every pixel keeps its last writer's serial (written; 0:
    // none since the page was taken plain from VRAM: never shown in a margin). With debug on (TRIUNIT_WIDE_DEBUG set when
    // the layer is enabled) each pixel also keeps that draw's Origin, the scanouts show kSentinel for a margin pixel no draw
    // wrote and each build's scenery draws are noted (notes, the parallax check).
    std::vector<uint32_t> written;std::vector<Origin> origin;uint32_t serial=0;Origin drawing;bool debug=false;
    std::array<uint32_t,2> page_start{};   // the serial when each page's frame started (a later serial: written by the frame)
    std::array<bool,2> cleared{{true,true}};   // the page clear ran on the page since its last build
    unsigned page=0;
    // The window offset (WindowShift) of the frame being built and of each page's last build. held: the camera and the
    // offset of the fight's last frame: after the fight's modes (the finisher, the round's end, the arena fading out) the
    // offset and the held planes stay while the camera does, build after build. limits:
    // the arena's last usable camera limits (min, max, valid). ratio: per scroll slot, for this arena.
    int shift=0;std::array<int,2> page_shift{},held{};std::array<int,3> limits{};
    std::array<Ratio,10> ratio{};std::array<int,2> ratio_limits{};
    // whole: this build's world comes from the shadow's draws alone (an offset window, or a far plane held at its art's
    // end: PlaneScroll); per page too. bound: per scroll slot, for this arena, the plane cameras between which its art
    // covers the wide screen whole (Learn, from the plane's measured bands).
    bool whole=false,fight=false;std::array<bool,2> page_whole{};   // fight: the build is a fight's (Offset): far planes are held
    // scene: this build drew a far plane that covers kPicture lines or more of the arcade screen (an arena, in a fight or
    // not). page_soft: the page's last build was a screen without an arena whose margins the game left unfilled
    // (EndFrame): its sides are shown soft (SoftMargins).
    // wider: in this build a scenery layer's art went on past the arcade screen (kSliver columns or more of a margin, on
    // kPicture lines or more that cover the arcade's columns whole): an arena, not a picture the size of the arcade
    // screen. Those lines are counted over all the build's layers (wide_lines): an arena can be many layers none of which
    // covers kPicture whole lines by itself (a bridge over a sky of clouds). arena: the build before had one. It tells
    // an attract demo's fight from the attract's other screens (the game's mode word is the same).
    bool scene=false,wider=false,arena=false;std::array<bool,2> page_soft{};int wide_lines=0;   // wide_lines: this build's, not state
    int arena_lines=kSliver;   // the lines that make an arena (the game's configuration; MK3 counts kPicture by itself)
    // A scenery layer's band, measured from its staged draws; slot: its far plane's scroll slot (-1: none).
    void Measured(int slot,int camera,Band&& band){
        int narrow=0;
        for(const auto& r:band.rows)narrow+=r.left>kWideMargin-kSliver&&r.right<kWideMargin+kVisible+kSliver;
        wide_lines+=std::max(int(band.full)-narrow,0);wider|=wide_lines>=arena_lines;
        if(slot>=0){scene|=band.full>=kPicture;Learn(unsigned(slot),camera,band);}
        if(!band.rows.empty())bands.push_back(std::move(band));
    }
    std::array<uint32_t,2> page_cleared_at{};   // the serial of each page's last clear (Mirror): a later pixel is art
    static constexpr int kNoLow=-(1<<30),kNoHigh=1<<30;
    static std::array<std::array<int,2>,10> Unbound(){std::array<std::array<int,2>,10> b;b.fill({kNoLow,kNoHigh});return b;}
    // want: the bounds as measured so far (Learn). A jump scrolls the screen up and shows lines of a plane whose art ends
    // sooner: a bound taken while the plane stands within its reach would move the plane at once, with the camera still.
    // It waits in want until taking it moves nothing (PlaneScroll: the camera away from that end).
    std::array<std::array<int,2>,10> bound=Unbound(),want=Unbound();
    void Whole(){page_whole[page]=whole=true;}
    // The arcade's camera stands exactly on one of the wide camera's limits in a fight (ScrollOffset).
    bool AtLimit(int camera)const{
        return fight&&limits[2]&&limits[0]+kWideMargin<=limits[1]-kWideMargin&&(camera==limits[0]+kWideMargin||camera==limits[1]-kWideMargin);
    }
    // span: per scroll slot, the plane cameras of the arcade at the arena's camera limits (PlaneScroll; first > second:
    // not known yet).
    std::array<std::array<int,2>,10> span=NoSpan();
    static std::array<std::array<int,2>,10> NoSpan(){std::array<std::array<int,2>,10> s;s.fill({1,0});return s;}
    // A far plane's measured band, drawn with its list's camera: where its art ends inside the shadow's clip. Only an end
    // the arcade never shows counts (the line's art reaches the arcade screen's edge at that camera limit): a narrower
    // line (an object on the plane) shows its ends on the arcade screen too, and keeps them.
    // seen: the columns of such an end that the arcade may show at its camera limit and still have it count (MK3: a
    // plane whose art stops short of the arcade's last screen, which shows the page's background there). The plane is
    // then held no sooner than where the wide screen's edge meets the arcade's own last column of it (its span moved in
    // by the margin): the wide picture shows of that end what the arcade shows at its limit, no more. With seen 0 an
    // end that counts lies past that column anyway. The game's configuration, not state.
    int seen=0;
    void Learn(unsigned slot,int camera,const Band& b){
        slot=std::min(slot,unsigned(want.size()-1));auto& at=want[slot];const auto [first,last]=span[slot];
        if(first>last||!fight||even)return;
        for(const auto& r:b.rows){
            if(r.left>-kReach&&camera+r.left-kWideMargin<=first+seen)at[0]=std::max(at[0],std::min(camera+r.left,first+kWideMargin));
            if(r.right<kLayerWidth+kReach&&camera+r.right-kWideMargin-kVisible>=last-seen)at[1]=std::min(at[1],std::max(camera+r.right-kLayerWidth,last-kWideMargin));
        }
    }
    // A far plane's scroll (16.16) in the shadow pass at the world camera x: the arcade's moved by its ratio times the
    // window offset (offset: ScrollOffset), then kept between its bounds, so its art's end stops at the wide screen's edge. The
    // plane slows down into the bound instead of stopping dead: over the last 2g columns before it, g being how far past
    // the bound the plane would stand at the wide camera's limit, it moves along a parabola (slope 1 to 0). Everything
    // depends on the camera alone: with the camera still, every plane stands still.
    // even (MK1, MK2; the game's configuration, not state): holding each plane at its own art's end stops some planes
    // of an arena well before the others (rows of one crowd then slide against each other while the screen scrolls).
    // Instead every far plane moves at an even pace between the arcade's own views of it at the arena's two camera
    // limits: at a wall the wide picture is the arcade's picture there, plane by plane (no plane shows art the arcade
    // never shows, so none needs holding), and in between each plane moves steadily, a little slower than in the
    // arcade, through the place it had before the world first moved (anchor: the wide camera and the plane's camera
    // then, where a fight starts), so nothing moves when the law starts. A plane whose travel between the limits is shorter than the two
    // margins keeps the arcade's place. A plane that moves by itself (alone: seen moving kAlone passes in a row with
    // the world still; clouds drifting) keeps the arcade's scroll: its ratio cannot be measured.
    bool even=true;static constexpr uint8_t kAlone=8;
    std::array<std::array<int,2>,10> anchor=NoAnchor();std::array<uint8_t,10> alone{};
    static std::array<std::array<int,2>,10> NoAnchor(){std::array<std::array<int,2>,10> a;a.fill({kNoLow,0});return a;}
    template<class A> void EvenFields(A& a){a(anchor,alone,ending);for(auto& b:bands)a(b.ends,b.gradient,b.ended);}   // save-state fields of the games with `even` and `edges`
    // A scroll slot's velocity and the world's in this pass: the plane's ratio while the world moves.
    void Velocity(unsigned slot,int32_t velocity,int32_t world_velocity){
        NewArena();slot=std::min(slot,unsigned(ratio.size()-1));
        if(world_velocity){ratio[slot]=MeasuredRatio(velocity,world_velocity);if(alone[slot]<kAlone)alone[slot]=0;}
        else if(!velocity){if(alone[slot]<kAlone)alone[slot]=0;}
        else if(alone[slot]<kAlone)++alone[slot];
    }
    int32_t PlaneScroll(unsigned slot,int32_t scroll,int camera,int64_t offset){
        slot=std::min(slot,unsigned(ratio.size()-1));const Ratio& r=ratio[slot];
        const auto part=[&](int64_t by){return r.world?by*r.plane/r.world:by;};   // an unknown ratio counts as the world's
        const auto move=[&](int64_t columns){return part(columns*65536);};
        if(even&&alone[slot]>=kAlone)return scroll;
        if(even&&limits[2]&&r.world&&fight){
            auto& s=span[slot];auto& at=anchor[slot];
            const int low=limits[0]+WindowShift(limits[0],limits[0],limits[1]),high=limits[1]+WindowShift(limits[1],limits[0],limits[1]);
            const int wide=std::clamp(camera+shift,low,high);
            if(s[0]>s[1]){   // the plane's arcade cameras at the limits; no place taken before the world moved: this one
                s={int((scroll-move(camera-limits[0])+65535)>>16),int((scroll+move(limits[1]-camera))>>16)};
                if(at[0]==kNoLow)at={wide,int((int64_t(scroll)+part(offset))>>16)};
            }
            const int64_t lo=int64_t(s[0]+kWideMargin)<<16,hi=int64_t(s[1]-kWideMargin)<<16;
            if(lo<=hi&&low<high){
                const int64_t pa=std::clamp(int64_t(at[1])<<16,lo,hi);const int wa=std::clamp(at[0],low,high);
                return int32_t(wide<wa?lo+(pa-lo)*(wide-low)/(wa-low):wide>wa?hi-(hi-pa)*(high-wide)/(high-wa):pa);
            }
        }
        // A sixteenth of a column: the sum's last bits depend on the arcade's fraction and must not tip a whole column.
        const auto settle=[&](int64_t v){return v==scroll?scroll:int32_t((v+2048)&~int64_t(4095));};
        int64_t q=int64_t(scroll)+part(offset);
        // While the ratio is not known the plane stands where the fight starts: the place its even pace passes through.
        if(even&&limits[2]&&!r.world&&fight)anchor[slot]={camera+shift,int(q>>16)};
        if(!limits[2]||!r.world||!fight)return settle(q);   // no arena, no ratio yet or no fight: as the arcade has it
        const int wide=camera+shift;   // the wide camera
        span[slot]={int((scroll-move(camera-limits[0])+65535)>>16),int((scroll+move(limits[1]-camera))>>16)};
        const int wide_low=limits[0]+WindowShift(limits[0],limits[0],limits[1]),wide_high=limits[1]+WindowShift(limits[1],limits[0],limits[1]);
        const auto ease=[](int64_t u,int64_t g){return u>=g?u:u<=-g?int64_t(0):(u+g)*(u+g)/(4*g);};
        const auto place=[&](const std::array<int,2>& b){   // the scroll kept between the bounds b
            int64_t p=q;if(b[0]>b[1])return p;   // art narrower than the wide screen: as the arcade has it
            if(b[0]!=kNoLow){const int64_t edge=int64_t(b[0])*65536,g=edge-(p-move(wide-wide_low));if(g>0)p=edge+ease(p-edge,g);}
            if(b[1]!=kNoHigh){const int64_t edge=int64_t(b[1])*65536,g=(p+move(wide_high-wide))-edge;if(g>0)p=edge-ease(edge-p,g);}
            return p;
        };
        auto& now=bound[slot];
        for(size_t e=0;e<2;++e)if(want[slot][e]!=now[e]){auto next=now;next[e]=want[slot][e];if(settle(place(next))==settle(place(now)))now=next;}
        return settle(place(now));
    }
    // keep: after the fight's modes the offset stays while the camera does (an attract demo's does not: Layer::arena).
    int Offset(bool shifting,int camera,int low,int high,bool keep=true){
        if(whole)return shift;   // the page kept its picture and its window (StartFrame)
        // Limits wider than kWorldMax: a finisher freed the camera (the fight goes on: the arena's last limits stay), or the
        // arena has a free camera from its start (no limits: the window stays centred).
        if(low<=high&&high-low<=kWorldMax)limits={low,high,1};
        else if(!fight)limits[2]=0;   // fight: still the last build's
        NewArena();
        const bool hold=keep&&!shifting&&fight&&camera==held[0];
        const int d=shifting?(limits[2]?WindowShift(camera,limits[0],limits[1]):0):hold?held[1]:0;
        if(shifting)held={camera,d};
        fight=shifting||hold;
        if(d)Whole();
        return page_shift[page]=shift=d;
    }
    void NewArena(){   // a new arena forgets the old one's ratios and bounds
        if(ratio_limits[0]!=limits[0]||ratio_limits[1]!=limits[1]){ratio={};bound=want=Unbound();span=NoSpan();anchor=NoAnchor();alone={};ratio_limits={limits[0],limits[1]};}
    }
    Ratio& PlaneRatio(unsigned slot){NewArena();return ratio[std::min(slot,unsigned(ratio.size()-1))];}
    // ending: the scenery layer whose draws are reaching the layer. The first draw of another one (or a screen-fixed
    // staged draw: the HUD) finds that layer's art whole and nothing over it yet: its ends are taken then (Ends). A
    // mirrored draw of no layer (a real DMA the shadow did not stage) may come among a layer's own and ends nothing.
    int ending=-1;
    void Draw(Kind kind,int lo,int hi,int layer=-1){
        if(layer!=ending&&(layer>=0||kind==Kind::shadow)){Ends(ending);ending=layer;}
        if(!++serial)serial=1;
        drawing={int16_t(std::clamp(lo,-32768,32767)),int16_t(std::clamp(hi,-32768,32767)),kind,uint8_t(page)};
        if(layer>=0)for(auto& b:bands)if(b.layer==layer&&!b.first)b.first=serial;
    }
    void Stamp(size_t index){written[index]=serial;if(debug)origin[index]=drawing;}
    // Scenery layers measured this build (MeasureBand); applied when the frame ends (MaskBands).
    std::vector<Band> bands;
    std::vector<Note> notes,notes_done;   // debug: this build's scenery draws, and the last finished build's
    std::vector<Placed> placed,placed_done;   // debug: this build's staged draws as placed, and the last finished build's
    MeasureCache shapes;   // the scenery draws MeasureBand keeps; not state
    std::vector<Band> bands_done;unsigned builds=0,dropped=0;   // debug: the last frame's measured layers; builds started; builds whose margins went with them (StartFrame)
    int shift_done=0;   // debug: the last finished build's window offset
    // A margin pixel of a band's line without the band's art, older than the band's first draw (the page clear's pen or a
    // farther layer: what the band would cover; a nearer one, a fighter or a projectile, stays), continues the line's art
    // where that is its own pattern going on:
    //  - a layer most of whose short lines on a side (3 in 5) end flat or in a two-column dither (their last kPattern
    //    columns repeat every two: a sky's gradient), or whose short lines there end on one straight edge (9 in 10 at
    //    the same column, kPicture lines or more: a backdrop picture narrower than the wide screen), keeps each line's
    //    last columns to the screen's edge, all its lines alike (no line left out among the others: a line of the page
    //    that was not measured, its art having a hole inside the arcade's columns, goes on from the same edge);
    //  - any other gap past the art (something nearer may cover the rest of the way to the screen's edge) takes the
    //    kSliver columns next to it, over again as far as it goes (a floor's or a sky's texture).
    // Any other shows black. Only an arena's lines are continued (a fight's build, or one with a far plane's picture): a
    // menu's textured background would come out as streaks, and its sides are shown soft instead (page_soft).
    // The page's background is not masked in a picture that shows it (open).
    static constexpr int kPattern=8,kSliver=kEndPens,kPicture=32;
    // open: this build's picture shows the page's background (kClear): the arcade's own columns have it on kPicture lines
    // or more, where no draw covers it (a sky, the windows of a hall). It is then the picture's own wherever it shows, in
    // the margins too: never masked, whatever covers a line in the arcade's columns (the HUD, a fighter in a jump, a roof).
    bool open=false;   // not state: found when the frame ends
    void Open(){
        int lines=0;
        for(size_t row=size_t(page)*256;row<size_t(page)*256+256;++row){
            const uint8_t* marks=fixed.data()+row*kLayerWidth+kWideMargin;
            lines+=std::any_of(marks,marks+kVisible,[](uint8_t m){return (m&kClear)!=0;});
        }
        open=lines>=kPicture;
    }
    // edges: the policy above applies (MK1, MK2). Without it (MK3: every plane is held at the arcade's own last view of
    // it, so what shows past a layer's end is what the arcade shows there, a farther plane or the page's background) the
    // bands are only measured. The game's configuration, not state.
    bool edges=true;
    // The ends of a scenery layer's lines, taken from the layer while it holds the band's art and nothing nearer.
    void Ends(int layer){
        if(layer<0||!edges)return;
        for(auto& b:bands)if(b.layer==layer&&b.first&&!b.ended&&!b.rows.empty()){
            b.ended=1;
            const auto own=[&](size_t row,int L){return L>=0&&L<kLayerWidth&&written[row+size_t(L)]>=b.first;};   // the band's art
            // The line's art from `edge` inwards (side -1: the left gap's, +1: the right gap's) repeats every two columns.
            const auto dithered=[&](size_t row,int edge,int side){
                for(int k=0;k<kPattern;++k){const int L=edge-side*k;
                    if(!own(row,L)||(k>=2&&pens[row+size_t(L)]!=pens[row+size_t(L+2*side)]))return false;}
                return true;
            };
            std::array<int,2> lines{},patterned{};std::array<std::vector<int>,2> ends{std::vector<int>(kLayerWidth+1,0),std::vector<int>(kLayerWidth+1,0)};
            for(const auto& r:b.rows){
                const size_t row=size_t(r.row)*kLayerWidth;
                if(r.left>0){++lines[0];patterned[0]+=dithered(row,r.left,-1);++ends[0][size_t(std::min<int>(r.left,kLayerWidth))];}
                if(r.right<kLayerWidth){++lines[1];patterned[1]+=dithered(row,r.right-1,+1);++ends[1][size_t(std::max<int>(r.right,0))];}
            }
            const auto straight=[&](int side){   // the column the short lines end at, give or take one (0: they do not)
                const auto& e=ends[size_t(side)];int best=0,at=0;
                for(int c=1;c<kLayerWidth;++c)if(const int n=e[size_t(c-1)]+e[size_t(c)]+e[size_t(c+1)];n>best){best=n;at=c;}
                if(lines[size_t(side)]<kPicture||best*10<lines[size_t(side)]*9)return 0;
                return e[size_t(at+1)]>e[size_t(at)]?at+1:e[size_t(at-1)]>e[size_t(at)]?at-1:at;
            };
            const std::array<int,2> end{straight(0),straight(1)};
            b.gradient={uint8_t(patterned[0]*5>=lines[0]*3||end[0]),uint8_t(patterned[1]*5>=lines[1]*3||end[1])};
            // measured: 1 a measured line, 0 one on the others' straight edge, 2 one beside measured lines (it is filled
            // where it can be and never masked: its margin stays as it is otherwise)
            const auto take=[&](size_t row,int edge,int side,int measured){   // the art's first column is `edge`
                BandEnd e;e.row=uint16_t(row/kLayerWidth);e.edge=int16_t(edge);e.right=side>0;e.dither=dithered(row,edge,side);e.measured=uint8_t(measured);
                for(int k=0;k<kEndPens;++k)if(const int L=edge-side*k;own(row,L)){e.own|=uint32_t(1)<<k;e.pen[size_t(k)]=pens[row+size_t(L)];}
                b.ends.push_back(e);
            };
            // A line that could not be measured (its art has a hole inside the arcade's columns: a vortex's dark centre)
            // lies among measured ones: beside[side][line] is the end column of the measured line nearest to it,
            // kPicture lines away at most (-1: none). Where its own art ends there too, give or take a column, it is a
            // line of the same picture and goes on like them.
            std::array<std::array<int16_t,256>,2> beside,ended;beside[0].fill(-1);beside[1].fill(-1);ended=beside;
            for(const auto& r:b.rows){
                if(r.left>0&&r.left<kLayerWidth)ended[0][r.row&255]=r.left;
                if(r.right<kLayerWidth&&r.right>0)ended[1][r.row&255]=int16_t(r.right-1);
            }
            for(size_t side=0;side<2;++side){
                std::array<int,256> apart;apart.fill(kPicture+1);
                for(int pass=0;pass<2;++pass){   // down the lines, then up
                    int last=-1,lines=kPicture+1;
                    for(int k=0;k<256;++k){
                        const size_t y=size_t(pass?255-k:k);
                        if(ended[side][y]>=0){last=ended[side][y];lines=0;}
                        else if(last>=0&&++lines<=kPicture&&lines<apart[y]){apart[y]=lines;beside[side][y]=int16_t(last);}
                    }
                }
            }
            size_t next=0;
            for(int y=b.rows.front().row&~255;y<=(b.rows.front().row|255);++y){
                const size_t row=size_t(y)*kLayerWidth;
                if(next<b.rows.size()&&b.rows[next].row==y){const auto& r=b.rows[next++];
                    if(r.left>0&&r.left<kLayerWidth)take(row,r.left,-1,true);
                    if(r.right<kLayerWidth&&r.right>0)take(row,r.right-1,+1,true);}
                else{   // not measured: where its art ends on the others' edge, or on its measured neighbours'
                    std::array<bool,2> taken{};
                    if(end[0]>0&&own(row,end[0])&&!own(row,end[0]-1)){take(row,end[0],-1,false);taken[0]=true;}
                    if(end[1]>0&&end[1]<kLayerWidth&&own(row,end[1]-1)&&!own(row,end[1])){take(row,end[1]-1,+1,false);taken[1]=true;}
                    for(int side=0;side<2;++side)if(const int e=beside[size_t(side)][size_t(y&255)];e>=0&&!taken[size_t(side)])
                        for(const int c:{e,e-1,e+1})if(own(row,c)&&!own(row,side?c+1:c-1)&&(side?c<kLayerWidth-1:c>0)){take(row,c,side?+1:-1,2);break;}
                }
            }
        }
    }
    void MaskBands(){
        Ends(ending);ending=-1;
        if(!edges){if(debug)bands_done=bands;bands.clear();return;}
        const bool art=fight||scene;
        for(const auto& b:bands)if(b.first&&!b.rows.empty())for(const auto& e:b.ends){
            if(e.measured!=1&&!art)continue;
            const size_t row=size_t(e.row)*kLayerWidth;const int side=e.right?1:-1,edge=e.edge;
            const int from=e.right?edge+1:0,to=e.right?kLayerWidth:edge;
            const auto bare=[&](int L){return !Arcade(L)&&written[row+size_t(L)]<b.first;};   // without the band's art
            // The gap as far as it shows: something nearer may cover the rest of the way to the screen's edge.
            int width=0;for(int L=from;L<to;++L)if(bare(L))width=std::max(width,side<0?edge-L:L-edge);
            const bool whole=art&&b.gradient[e.right]&&(e.own&1),dither=whole&&e.dither;
            // The columns next to the gap, as many as are kept (kSliver), over again as far as the gap goes.
            bool sliver=art&&!whole;
            for(int k=0;k<std::min(width,kSliver)&&sliver;++k)sliver=(e.own>>k&1)!=0;
            for(int L=from;L<to;++L){
                const size_t at=row+size_t(L);
                if(!bare(L))continue;
                if(!whole&&!sliver){if(e.measured!=2&&!(open&&(fixed[at]&kClear)))fixed[at]|=kMask;continue;}
                const int away=side<0?edge-L:L-edge;   // columns from the art's first column, outwards (>= 1)
                pens[at]=e.pen[size_t(sliver?(width-away)%kSliver:dither?(away&1):0)];fixed[at]=0;if(!blood.empty())blood[at]=Blood{};
                written[at]=b.first;if(debug)origin[at]={int16_t(L),int16_t(L+1),Kind::continued,uint8_t(page)};
            }
        }
        if(debug)bands_done=bands;
        bands.clear();
    }
    // Whether a layer pixel is shown: always in the arcade's columns; in a margin only when a draw wrote it and it has art.
    bool Shown(size_t index)const{return Arcade(int(index%kLayerWidth))||(written[index]&&!(fixed[index]&kMask));}
    // One shown line's margins (debug): the layer line of VRAM row vram_row.
    void AuditRow(Audit& a,unsigned vram_row)const{
        if(!debug)return;
        const size_t row=size_t(vram_row)*kLayerWidth;const uint32_t start=page_start[(vram_row>>8)&1];
        bool full=true;for(int L=kWideMargin;L<kWideMargin+kVisible&&full;++L)full=written[row+size_t(L)]>start;
        for(int L=0;L<kLayerWidth;++L){
            if(Arcade(L))continue;
            const size_t at=row+size_t(L);const Origin& o=origin[at];++a.shown;
            if(!written[at]){++a.sentinel;continue;}
            if(fixed[at]&kMask){++a.masked;continue;}
            a.generated+=o.kind==Kind::generated;
            a.outside+=(o.kind==Kind::real||o.kind==Kind::shadow)&&!o.Covers(L);a.outside_real+=o.kind==Kind::real&&!o.Covers(L);
            a.stale+=full&&written[at]<=start&&!(fixed[at]&kClear);   // the background is no draw
        }
    }
    bool Enabled()const{return !pens.empty();}
    static bool Arcade(int L){return ArcadeColumn(L);}
    void Enable(const std::vector<uint16_t>& vram,bool with_blood){
        pens.assign(kLayerSize,0);fixed.assign(kLayerSize,0);blood.assign(with_blood?kLayerSize:0,Blood{});
        written.assign(kLayerSize,0);debug=std::getenv("TRIUNIT_WIDE_DEBUG")!=nullptr;origin.assign(debug?kLayerSize:0,Origin{});
        for(unsigned p=0;p<2;++p)PlainPage(p,vram,{},true);
    }
    // The layer index of a VRAM index's screen position (-1: outside the layer).
    static int64_t At(size_t vram_index){
        const int c=int(vram_index&511)+kExtra;
        return c>=0&&c<kLayerWidth?int64_t(vram_index>>9)*kLayerWidth+c:-1;
    }
    void Write(size_t index,uint16_t pen,bool screen_fixed=false){
        pens[index]=booked[(pen>>8)&0x7f]?uint16_t(pen|kBooked):pen;fixed[index]=screen_fixed;if(!blood.empty())blood[index]=Blood{};
        Stamp(index);
    }
    // A palette slot the shadow pass booked this build (the game has not built the object, so its colours are not loaded):
    // a draw in it writes its pen | kBooked, whose colour is the palette's own in the ROM (the slot's pointer: a count word,
    // then the colours, as the game loads them), kept per page (the page it was drawn on). read: a ROM word at a bit address.
    static constexpr uint16_t kBooked=0x8000;
    std::array<bool,128> booked{};std::array<std::vector<uint16_t>,2> rom_palette;
    template<class Read> void Book(unsigned slot,uint32_t pointer,Read read){
        auto& to=rom_palette[page];if(to.empty())to.assign(0x8000,0);
        const unsigned count=std::min(unsigned(read(pointer)),256u);
        for(unsigned k=0;k<count;++k)to[slot*256+k]=uint16_t(read(pointer+16+16*k));
        booked[slot&0x7f]=true;
    }
    // dim (of 256): the game's own fade right now. A booked colour takes it, as the palette RAM would hold the colour had
    // the game loaded it (the scene dimmed behind the continue box).
    uint16_t Colour(size_t index,uint16_t pen,const uint16_t* palette,unsigned dim=256)const{
        const auto& rom=rom_palette[(index/kLayerWidth>>8)&1];
        if(!(pen&kBooked)||rom.empty())return palette[pen&0x7fff];
        const unsigned c=rom[pen&0x7fff];
        return dim>=256?uint16_t(c):uint16_t(((c>>10&31)*dim>>8)<<10|((c>>5&31)*dim>>8)<<5|((c&31)*dim>>8));
    }
    // CPU writes are screen-fixed at their VRAM column; a line copy (the page clear) writes the whole layer line, the
    // columns past the VRAM line included, in the line's pen. A cleared page holds no build any more: its window is the
    // arcade's and its sides are soft until a build ends on it (a picture the CPU writes on it, with no walk, is shown so).
    void Mirror(const std::vector<uint16_t>& vram,size_t start,size_t count){
        if(!Enabled())return;
        if(!(start&511)&&count&&!(count&511)){
            Draw(Kind::clear,0,kLayerWidth);
            for(size_t row=start>>9;row<(start+count)>>9;++row){
                const size_t p=(row>>8)&1;page_cleared_at[p]=serial;page_shift[p]=0;page_whole[p]=false;page_soft[p]=true;
                clear_pen[row]=vram[row*512];PlainRow(row,vram);
                for(size_t L=row*kLayerWidth;L<(row+1)*kLayerWidth;++L)Stamp(L);
            }
            return;
        }
        const auto put=[&](size_t to,uint16_t pen){Stamp(to);pens[to]=pen;fixed[to]=0;if(!blood.empty())blood[to]=Blood{};};
        for(size_t i=start;i<start+count;++i)if(const auto at=At(i);at>=0){
            Draw(Kind::cpu,int(at%kLayerWidth),int(at%kLayerWidth)+1);put(size_t(at),vram[i]);
            // The VRAM line's end column (far off the arcade screen: a line written whole, a wipe or a picture) gives its
            // pen to the layer's columns past it, as a line copy does.
            if(const size_t c=i&511;c==0||c==511)
                for(int L=c?kWidth+kExtra:0;L<(c?kLayerWidth:kExtra);++L)if(PastVram(L))put(size_t(at)/kLayerWidth*kLayerWidth+size_t(L),vram[i]);
        }
    }
    std::vector<uint16_t> clear_pen=std::vector<uint16_t>(512,0);   // each line's pen at its last page clear
    // The layer's columns past the VRAM line on each side (screen x -67..-57 and 456..466) have no VRAM column of their own.
    static bool PastVram(int L){return L<kExtra||L>=kWidth+kExtra;}
    void PlainRow(size_t row,const std::vector<uint16_t>& vram){   // a line as the page clear leaves it
        auto* p=pens.data()+row*kLayerWidth;const auto* v=vram.data()+row*512;
        for(int L=0;L<kLayerWidth;++L)p[L]=PastVram(L)?clear_pen[row]:v[L-kExtra];
        std::fill_n(fixed.begin()+row*kLayerWidth,kLayerWidth,kClear);
        if(!blood.empty())std::fill_n(blood.begin()+row*kLayerWidth,kLayerWidth,Blood{});
        std::fill_n(written.begin()+row*kLayerWidth,kLayerWidth,0u);if(debug)std::fill_n(origin.begin()+row*kLayerWidth,kLayerWidth,Origin{});
    }
    void MarkCleared(size_t index){if(Enabled())cleared[(index>>17)&1]=true;}
    // A page's arcade columns taken from VRAM (all: the margins too, the layer's start: they hold no draw). After the page
    // clear every pixel keeps the clear's serial and mark (it wrote them all); else the arcade's pixels have no writer, and
    // one is still the page's background where the layer had it so and VRAM holds the same pen.
    void PlainPage(unsigned p,const std::vector<uint16_t>& vram,const std::vector<Blood>& arcade_blood,bool all=false){
        const int first=all?0:kWideMargin,last=all?kLayerWidth:kWideMargin+kVisible;
        const bool stains=!blood.empty(),arcade=arcade_blood.size()==vram.size(),fresh=cleared[p];
        for(size_t row=size_t(p)*256;row<size_t(p)*256+256;++row){   // plain pointers: the stores cannot move the layers
            uint16_t* to=pens.data()+row*kLayerWidth;uint8_t* marks=fixed.data()+row*kLayerWidth;
            Blood* stain=stains?blood.data()+row*kLayerWidth:nullptr;
            const uint16_t* from=vram.data()+row*512;const Blood* arcade_stain=arcade?arcade_blood.data()+row*512:nullptr;
            for(int L=first;L<last;++L){const bool past=PastVram(L);const size_t v=past?0:size_t(L-kExtra);
                const uint16_t pen=past?clear_pen[row]:from[v];
                marks[L]=fresh||pen==to[L]?uint8_t(marks[L]&kClear):0;to[L]=pen;if(stain)stain[L]=arcade_stain&&!past?arcade_stain[v]:Blood{};
                if(!fresh){written[row*kLayerWidth+size_t(L)]=0;if(debug)origin[row*kLayerWidth+size_t(L)]={};}}
        }
    }
    // The frame's build starts on its page: its arcade columns from VRAM. The margins stay as the layer has them: what the
    // page clear and the draws since left there, or, on a page not cleared since its last build, that build's margins (the
    // game draws over what the page holds). VRAM's own columns past the arcade's are not the margins' picture: a write
    // that is not a draw (mk5widescreen::kNullWrite) lands there.
    // still: the camera stands where the fight's last frame left it. A page not cleared whose last build came from the
    // shadow's draws alone (an offset window: the continue screen over a fight that ended at an arena's end) then keeps
    // its picture and its window: VRAM has the arcade's window there, the layer the wide one, and the game draws over what
    // the page holds.
    // A build's margins stand beside its own picture. The game can drop a build: it resets its queue before the board has
    // drawn it all (a scene's last frame: the far layers and nothing else reach VRAM), and the page is shown so. On a
    // page not cleared whose arcade columns are not what the layer has there, the margins are masked with that build; a
    // build of the shadow's draws alone (its columns are not VRAM's) is taken as dropped when no display interrupt ended it.
    void StartFrame(unsigned page_row,const std::vector<uint16_t>& vram,const std::vector<Blood>& arcade_blood,bool still=false){
        page=(page_row>>8)&1;shift_done=shift;
        const bool keep=still&&!cleared[page]&&page_whole[page];
        if(keep){shift=page_shift[page];whole=true;}
        else{
            bool lost=!cleared[page]&&page_whole[page]&&builds!=ended;
            for(size_t row=size_t(page)*256;row<size_t(page)*256+254&&!cleared[page]&&!page_whole[page]&&!lost;++row)
                lost=!std::equal(pens.begin()+row*kLayerWidth+kWideMargin,pens.begin()+row*kLayerWidth+kWideMargin+kVisible,
                                 vram.begin()+row*512+size_t(kWideMargin-kExtra));
            for(size_t at=size_t(page)*256*kLayerWidth;lost&&at<(size_t(page)+1)*256*kLayerWidth;++at)
                if(!Arcade(int(at%kLayerWidth)))fixed[at]|=kMask;
            dropped+=lost;
            PlainPage(page,vram,arcade_blood);page_shift[page]=shift=0;page_whole[page]=whole=false;
        }
        cleared[page]=false;booked.fill(false);page_start[page]=serial;bands.clear();ending=-1;scene=false;arena=wider;wider=false;wide_lines=0;
        ++builds;if(debug){notes_done.swap(notes);notes.clear();placed_done.swap(placed);placed.clear();}
    }
    // The frame ends (the next display interrupt): the margins without a scenery layer's art are masked, and so is a margin
    // pixel the frame did not write on a line it rewrote across the arcade's 400 columns (the game redraws that line whole:
    // what the margin kept from an earlier frame, a fighter's old pose there, is no longer in the scene). The page's
    // background on such a line is masked too, unless the picture shows it (open): a sky is no place without art.
    // After kIdle display interrupts in a row with no build, and something written since the first of them (a slow build's
    // last draws come before it), the game is showing a picture drawn without its display walk (straight to the page,
    // over what a build left there): the pages hold no build's window any more, and their sides are shown soft.
    static constexpr unsigned kIdle=3;
    unsigned ended=0,idle=0;uint32_t idle_serial=0;bool released=false;   // builds at the last interrupt; interrupts without one; the serial at the first
    // The game stopped building for kIdle display interrupts or more: the build that follows is another screen's, whatever
    // its camera (MK2's continue screen has the camera at 0, where a fight at an arena's left end can leave it).
    bool Paused()const{return idle>=kIdle;}
    void EndFrame(){
        if(!Enabled())return;
        if(builds!=ended){ended=builds;idle=0;released=false;}
        else if(!idle++)idle_serial=serial;
        else if(idle>=kIdle&&serial!=idle_serial&&!released){
            released=true;page_shift={};page_whole={};page_soft={true,true};shift=0;whole=fight=arena=wider=false;
        }
        if(released)return;
        Open();MaskBands();
        const uint32_t start=page_start[page];
        for(size_t row=size_t(page)*256;row<size_t(page)*256+256;++row){
            const size_t base=row*kLayerWidth;bool full=true;
            for(int L=kWideMargin;L<kWideMargin+kVisible&&full;++L)full=written[base+size_t(L)]>start;
            if(full)for(int L=0;L<kLayerWidth;++L)if(const size_t at=base+size_t(L);!Arcade(L)&&
                (fixed[at]&kClear?!open:written[at]<=start))fixed[at]|=kMask;
        }
        // Filled: every margin pixel of the page's visible lines is shown and is art (drawn since the page's last clear) or
        // the colour its arcade line ends in (black beside black, a sky beside its sky).
        bool filled=true;
        for(size_t row=size_t(page)*256;row<size_t(page)*256+254&&filled;++row){
            const size_t base=row*kLayerWidth;
            for(int L=0;L<kLayerWidth&&filled;++L){
                if(Arcade(L))continue;
                const size_t at=base+size_t(L),edge=base+size_t(L<kWideMargin?kWideMargin:kWideMargin+kVisible-1);
                filled=Shown(at)&&(written[at]>page_cleared_at[page]||pens[at]==pens[edge]);
            }
        }
        page_soft[page]=!fight&&!scene&&!filled;
    }
    // Save-state fields (W5): the layer and the pages' facts.
    template<class A> void LayerFields(A& a){a(pens,blood,fixed,written,serial,page_start,cleared,page,bands,clear_pen,booked,rom_palette,shift,page_shift,held,limits,ratio,ratio_limits,whole,page_whole,bound,want,span,fight,scene,page_soft,page_cleared_at,arena,wider,released);}
    void Reset(){*this=Layer{};}
};
}
