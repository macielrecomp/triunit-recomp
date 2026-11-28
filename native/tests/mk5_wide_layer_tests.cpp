// The wide layer's line continuation (mk5wide::Layer::Ends, MaskBands), on a made-up layer (no ROM): a scenery layer
// narrower than the wide screen goes on into the margins from its own art, whatever stands over its ends afterwards. A
// nearer layer over an end (a pillar passing by, a fighter, a flying object) is drawn over the continued lines and is
// never drawn out across the margin.
// A textured layer that stops more than its kept columns short of the screen's edge has them over again as far as the
// gap goes, and a line with a hole inside the arcade's columns (it cannot be measured) goes on like the measured lines
// next to it when it ends where they do.
// The window's offset after a fight (mk5wide::Layer::Offset, Paused): it stays while the camera does, build after
// build, and not across display interrupts without a build (another screen, whatever its camera).
// A build the game drops (mk5wide::Layer::StartFrame): its margins stay only beside the picture it drew.
#include "../include/mk5_wide.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
using namespace mk5wide;
constexpr int W=kLayerWidth,kFirst=100,kLines=40,kLeft=40,kRight=500;   // the far layer: lines 100..139, columns 40..499
constexpr uint16_t kNear=0x7777,kFighter=0x5555;
void need(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
uint16_t art(int y,int L,bool dither){return uint16_t(0x100+y*2+(dither?(L&1):0));}

// One build: the far layer, then (covered) a nearer layer over each of its ends, then the frame's end.
Layer<uint16_t> build(bool dither,bool covered){
    std::vector<uint16_t> vram(512*512,0);
    Layer<uint16_t> l;l.Enable(vram,false);l.StartFrame(0,vram,{});l.fight=true;
    Band band;band.layer=0;band.full=kLines;
    for(int y=kFirst;y<kFirst+kLines;++y)band.rows.push_back({uint16_t(y),int16_t(kLeft),int16_t(kRight)});
    l.Measured(-1,0,std::move(band));
    const auto fill=[&](int layer,int lo,int hi,auto pen){
        l.Draw(Kind::shadow,lo,hi,layer);
        for(int y=kFirst;y<kFirst+kLines;++y)for(int L=lo;L<hi;++L)l.Write(size_t(y)*W+size_t(L),pen(y,L));
    };
    fill(0,kLeft,kRight,[&](int y,int L){return art(y,L,dither);});
    if(covered){
        fill(1,kRight-10,kRight+10,[](int,int){return kNear;});      // over the right end, reaching into the margin
        fill(2,kLeft-10,kLeft+10,[](int,int){return kFighter;});     // over the left end
    }
    l.EndFrame();
    return l;
}
void check(bool dither){
    const auto plain=build(dither,false),covered=build(dither,true);
    for(int y=kFirst;y<kFirst+kLines;++y)for(int L=0;L<W;++L){
        const size_t at=size_t(y)*W+size_t(L);
        need(plain.Shown(at)&&covered.Shown(at),"every pixel of the layer's lines is shown");
        need(plain.pens[at]==art(y,L,dither),"the line goes on into both margins in its own pattern");
        const bool near=L>=kRight-10&&L<kRight+10,fighter=L>=kLeft-10&&L<kLeft+10;
        need(covered.pens[at]==(near?kNear:fighter?kFighter:art(y,L,dither)),
             "a nearer layer over the line's end stays where it was drawn and is not drawn out across the margin");
    }
}
// A textured picture (no flat line, no two-column dither) on columns kShort..W-kShort-1: every line's margins take the
// kSliver columns next to them, over again; lines 110..119 have a hole in the arcade's columns and are not measured.
void texture(){
    constexpr int kShort=29;
    const auto pen=[](int y,int L){return uint16_t(0x400+(L*7+y*3)%23);};
    std::vector<uint16_t> vram(512*512,0);
    Layer<uint16_t> l;l.Enable(vram,false);l.StartFrame(0,vram,{});l.fight=true;
    Band band;band.layer=0;
    for(int y=kFirst;y<kFirst+kLines;++y)if(y<110||y>=120){band.rows.push_back({uint16_t(y),int16_t(kShort),int16_t(W-kShort)});++band.full;}
    l.Measured(-1,0,std::move(band));
    l.Draw(Kind::shadow,kShort,W-kShort,0);
    for(int y=kFirst;y<kFirst+kLines;++y)for(int L=kShort;L<W-kShort;++L)
        if(y<110||y>=120||L<250||L>=280)l.Write(size_t(y)*W+size_t(L),pen(y,L));
    l.EndFrame();
    for(int y=kFirst;y<kFirst+kLines;++y)for(int L=0;L<kShort;++L){
        const size_t left=size_t(y)*W+size_t(L),right=size_t(y)*W+size_t(W-1-L);
        need(l.Shown(left)&&l.Shown(right),"a textured line's margins are shown, measured or with a hole, 29 columns from the edge");
        need(l.pens[left]==pen(y,kShort+L%Layer<uint16_t>::kSliver),"the left margin has the columns next to it, over again");
        need(l.pens[right]==pen(y,W-kShort-1-L%Layer<uint16_t>::kSliver),"the right margin has the columns next to it, over again");
    }
}
void offset(){
    std::vector<uint16_t> vram(512*512,0);
    const auto fight_at_the_left_end=[&](Layer<uint16_t>& l){
        l.Enable(vram,false);l.StartFrame(0,vram,{});
        need(l.Offset(true,0,0,883)!=0,"a fight at the arena's left end has its window offset");
        l.EndFrame();
    };
    Layer<uint16_t> going;fight_at_the_left_end(going);
    going.StartFrame(0,vram,{});
    need(going.Offset(false,0,-80,4095,!going.Paused())!=0,"the build after the fight keeps the offset while the camera stands");
    Layer<uint16_t> paused;fight_at_the_left_end(paused);
    for(unsigned k=0;k<Layer<uint16_t>::kIdle;++k)paused.EndFrame();
    paused.StartFrame(0,vram,{});
    need(paused.Offset(false,0,-80,4095,!paused.Paused())==0,"a build after display interrupts without one is centred, with the same camera");
}
// The next build starts on a page not cleared. VRAM's arcade columns hold what the last build drew there (the board drew
// its whole queue): its margins stay; they hold less (the game dropped the queue first): the margins go with the build.
void dropped(){
    for(const bool drawn:{true,false}){
        std::vector<uint16_t> vram(512*512,0);
        Layer<uint16_t> l;l.Enable(vram,false);l.StartFrame(0,vram,{});l.fight=true;
        l.Draw(Kind::shadow,0,W,0);
        for(int y=kFirst;y<kFirst+kLines;++y)for(int L=0;L<W;++L){
            l.Write(size_t(y)*W+size_t(L),kNear);
            if(drawn&&L>=kExtra&&L<kExtra+512)vram[size_t(y)*512+size_t(L-kExtra)]=kNear;
        }
        l.StartFrame(0,vram,{});
        for(int y=kFirst;y<kFirst+kLines;++y)for(int L=0;L<kWideMargin;++L)
            need(l.Shown(size_t(y)*W+size_t(L))==drawn&&l.Shown(size_t(y)*W+size_t(W-1-L))==drawn,
                 drawn?"a build's margins stay beside the picture it drew":"a dropped build's margins go with it");
    }
}
}

int main(){
    try{check(false);check(true);texture();offset();dropped();}
    catch(const std::exception& error){std::printf("FAIL: %s\n",error.what());return 1;}
    std::printf("wide layer: a line goes on from its own art, flat and dithered, with and without a nearer layer over its ends; "
                "the window's offset does not outlive a pause in the builds; a dropped build's margins go with it\n");
    return 0;
}
