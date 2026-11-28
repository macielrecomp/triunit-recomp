// MK1 widescreen acceptance (tools/widescreen/acceptance.py): a legal tape played twice in the Remastered edition,
// without and with the widescreen shadow pass (debug diagnostics on, mk5wide::Layer::debug).
//  1. The game is the same in every shown frame: a hash of CPU registers, cycles, RAM, VRAM and palette per frame, the
//     shown frame and its Remastered layers, and at the end the PCM and CMOS.
//  2. The wide frame's 400 centre columns are the arcade frame bit for bit in every frame (raw and presented), but for the
//     pixels of a scenery block the arcade has not drawn yet (filled) and a soft side's start (joined), counted apart.
//  3. No margin pixel is left unwritten (sentinel) in a fight frame; masked pixels are counted (the edge policy).
//  4. No margin pixel comes from a draw that never meant to cover its column, or from no draw of the game.
//  2b. A frame whose wide camera is not the arcade's (mk5wide::WindowShift at an arena's end, a far plane held at its
//     art's end) is counted apart: its world is the shadow's draws, its HUD the real ones.
//  6. With the camera still, every scenery list's camera is still.
//  7. No fight frame with a lit picture masks more than a speck (kSpeck pixels) of its margins.
// 12. Far planes move at an even pace (mk5wide::Layer::even): per arena and scenery list, the list's camera stays within
//     kPace columns of the straight line between its places at the lowest and highest world camera seen in fight builds
//     (when those are kPaceRange columns or more apart). A plane held early at its own art's end, or eased, fails.
//  5. Each scenery layer's draws in the margins move by the same columns per frame as the layer's draws in the centre: per
//     layer and pair of builds, the most frequent move of its draws (one image on one row, paired in column order) wholly
//     in the margins equals that of its draws wholly in the centre, when at least 3 draws share it on each side, the
//     layer's list camera moved and the centre's draws moved by minus that move (scenery, not objects of their own motion).
// Prints one JSON line with the counts and, per arena (its camera limits) and scenery layer, the margin columns its art
// covers at least (the edge coverage table). Inputs: the tape (tools/mk1/tapes, rendered by the caller), the setup's seed.
#include "mk5_task_body.hpp"
#include "mk5_scanout.hpp"
#include "mk5_live_video.hpp"
#include "mk5_audio.hpp"
#include "mk5_audio_link.hpp"
#include "mk5_widescreen.hpp"
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>

namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> bytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"cannot open chip ROM");return {std::istreambuf_iterator<char>(f),{}};}
template<class V> uint64_t Fnv(uint64_t h,const V& v){for(auto x:v)h=(h^uint64_t(x))*0x100000001b3ull;return h;}
constexpr uint64_t kBasis=14695981039346656037ull;
constexpr int kPace=3,kPaceRange=100;
constexpr uint64_t kSpeck=16;   // masked margin pixels a fight frame may show (a farther layer's pixel at the screen's last column)
struct Frame{uint64_t state,shown;};
// The edge coverage table: per arena (camera limits) and scenery layer, the fewest margin columns its art covered.
struct Coverage{int left=mk5wide::kWideMargin,right=mk5wide::kWideMargin;unsigned frames=0;int camera_low=1<<20,camera_high=-(1<<20);};
struct Run{
    std::vector<Frame> frames;std::vector<int16_t> pcm;std::vector<uint16_t> cmos;uint64_t faults=0,streamer_skips=0;   // streamer_skips: builds whose shadow ran without its scenery streamer
    uint64_t centre=0,presented=0,fight_frames=0;mk5wide::Audit fight,other;
    uint64_t filled=0,joined=0;   // centre pixels the shadow filled (frames with the arcade's window); soft sides started inside the arcade's columns
    uint64_t masked_frames=0;     // lit fight frames with more than kSpeck masked margin pixels
    // Frames with a window offset (mk5wide::WindowShift): how many, their arcade columns that differ from the arcade frame
    // at the offset (far planes stand elsewhere there), and their masked margin pixels.
    uint64_t shifted_frames=0,shifted_differ=0,shifted_masked=0;
    uint64_t still_checked=0,still_moved=0;   // builds whose last scenery list (the world's) kept its camera; of them, with a list that moved
    uint64_t parallax_checked=0,parallax_differ=0;std::map<std::tuple<int,int,int>,Coverage> coverage;
    std::map<std::tuple<int,int,int>,std::map<int,std::array<int,2>>> pace;   // arena limits, scenery list -> world camera -> the list's camera, the frame
    uint64_t pace_checked=0,pace_off=0,ring=0;   // ring: copies of a wrapping far-plane part staged (Mk5LiveVideo::WideRing)
};
Run Play(const std::filesystem::path& root,const std::vector<uint32_t>& tape,const std::vector<uint16_t>& cmos,bool widescreen){
    auto program=LoadMk5Program(root/"roms");auto graphics=LoadMk5GlyphBank(root/"roms");
    const auto rom=root/"roms/mk1";
    mk5audio::WilliamsAudio audio(bytes(rom/"mks-u3.rom"),bytes(rom/"mks-u12.rom"),bytes(rom/"mks-u13.rom"));
    mk5audio::GameAudioScope scope(audio);
    Run run;Mk5MemoryBuffers m;Mk5BoardState f;f.cmos=cmos;auto& s=f.check.cpu;BootDiState di;
    Mk5IrqController irq(program,s,m);Mk5TaskBodyRunner runner(program,f,m,irq,di);
    const BootDiInputs released{0xffffffff,0xfeffffff};
    Mk5BootInputs power_on;power_on.ports=released;power_on.pending=0x400;power_on.next_line=1;power_on.quanta_to_next=6325;
    check(runner.Boot(power_on).stop==TaskBodyStop::TaskEntry,"power-on did not reach the first task");
    TaskBodyInputs in;in.dsw=0xfeff;in.main_dsw=0xfeff;in.sound_status=0x80;in.quiet_di=released;
    check(runner.Run(in).stop==TaskBodyStop::MainLoop,"runner did not enter the main loop");
    runner.AttachVideo(graphics,true);
    Mk5Scanout screen(irq,m,&runner.Video());
    if(widescreen){runner.EnableWide();screen.EnableWidePresentation(runner.Video());}
    in.active_di=released;in.stop_after_decompression=true;
    TaskBodyResult result;
    for(unsigned i=0;i<100;++i){result=runner.ContinueMain(in,64);if(result.stop!=TaskBodyStop::Budget && result.stop!=TaskBodyStop::DiLimit)break;}
    check(result.stop==TaskBodyStop::DecompressorComplete,"original decompression did not complete");
    in.stop_after_decompression=false;in.instruction_budget=10000;
    const unsigned W=Mk5Scanout::kWideWidth;const auto& layer=runner.Video().WideLayer();
    std::vector<mk5wide::Note> previous;unsigned seen_builds=0;
    const auto word=[&](uint32_t bit){return m.ram[(bit-0x01000000)/16];};
    for(unsigned n=0;screen.frames()<tape.size() && n<tape.size()*250;++n){
        const auto before=screen.frames();
        in.active_di=BootDiInputs{0xffffffffu&~tape[before],0xfeffffff};
        result=runner.ContinueMain(in,64);
        check(result.stop==TaskBodyStop::Budget || result.stop==TaskBodyStop::DiLimit,"MK1 replay left the native route");
        const auto& pcm=audio.Pcm();run.pcm.insert(run.pcm.end(),pcm.begin(),pcm.end());audio.DiscardDiagnostics();
        runner.CompactDiagnostics();
        if(screen.frames()==before)continue;
        check(screen.frames()==before+1,"observer missed a raster frame");
        const auto& fighter=screen.fighter_frame();
        std::vector<uint32_t> layers(fighter.underlay);for(auto o:fighter.opaque_order)layers.push_back(uint32_t(o));
        for(const auto& l:fighter.layers){layers.push_back(uint32_t(l.x));layers.push_back(l.y);layers.insert(layers.end(),l.pixels.begin(),l.pixels.end());}
        uint64_t state=Fnv(Fnv(Fnv(Fnv(kBasis,s.a),s.b),m.ram),m.vram);state=Fnv(Fnv(state,m.palette),std::array<uint64_t,2>{s.reference_cycles,s.status});
        run.frames.push_back({state,Fnv(Fnv(Fnv(kBasis,screen.pixels()),screen.backdrop_pixels()),layers)});
        if(!widescreen)continue;
        // 2: the centre, raw and presented (pixels and backdrop).
        const unsigned X=screen.width(),H=screen.height(),margin=(W-X)/2;
        const auto& wide=screen.wide_pixels();const auto& p=screen.wide_presentation();const auto& b=screen.wide_backdrop_pixels();
        const int shift=screen.wide_shift();const bool whole=screen.wide_whole();
        if(whole){++run.shifted_frames;run.shifted_masked+=screen.wide_audit().masked;}
        // Two differences belong to the picture: a pixel the shadow filled where the arcade has not drawn an object yet (the
        // frame's audit counts them, each at most once raw, once presented and once in the backdrop), and, on a screen with
        // soft sides, an arcade line's black end within kSoftSkip columns of its edge, where the soft side starts.
        uint64_t raw=0,presented=0;const uint64_t filled=screen.wide_audit().filled;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<X;++x){
            const uint32_t arcade=screen.pixels()[y*X+x];
            if(whole){if(const int at=int(margin+x)-shift;at>=0&&at<int(W))run.shifted_differ+=p[y*W+unsigned(at)]!=arcade;continue;}   // past a limit the arcade's last columns are off the wide picture (mk5wide::kOvershoot)
            if(screen.wide_soft()&&!(arcade&0xffffff)&&(x<unsigned(mk5wide::kSoftSkip)||x>=X-unsigned(mk5wide::kSoftSkip))){run.joined+=p[y*W+margin+x]!=arcade;continue;}
            raw+=wide[y*W+margin+x]!=arcade;presented+=p[y*W+margin+x]!=arcade;
            if(!screen.backdrop_pixels().empty())presented+=b[y*W+margin+x]!=screen.backdrop_pixels()[y*X+x];
        }
        if(!whole){run.filled+=filled;run.centre+=raw>filled?raw-filled:0;run.presented+=presented>2*filled?presented-2*filled:0;}
        if(std::getenv("WIDE_TRACE")&&!whole&&(filled||raw)){static uint64_t was=0;if(filled!=was){was=filled;std::cerr<<"filled frame "<<before<<" "<<filled<<" raw "<<raw<<" presented "<<presented<<char(10);}}
        // Diagnostics: WIDE_DUMP_FRAMES (frame numbers, commas) writes each one's presented wide frame as a PPM in WIDE_DUMP_DIR;
        // WIDE_DUMP_MODE (a mode word's value) writes every 64th frame of that mode (a screen whose frames are not known yet).
        const char* const dump_list=std::getenv("WIDE_DUMP_FRAMES");const char* const dump_mode=std::getenv("WIDE_DUMP_MODE");
        if((dump_list&&(","+std::string(dump_list)+",").find(","+std::to_string(before)+",")!=std::string::npos)||
           (dump_mode&&unsigned(word(mk5widescreen::kMode))==unsigned(std::atoi(dump_mode))&&before%64==0)){
            std::ofstream out(std::filesystem::path(std::getenv("WIDE_DUMP_DIR"))/("wide-"+std::to_string(before)+".ppm"),std::ios::binary);
            out<<"P6"<<char(10)<<W<<" "<<H<<char(10)<<255<<char(10);
            const bool mark=std::getenv("WIDE_DUMP_MASK");   // masked margin pixels in green (a fight frame: soft sides can be the same black)
            for(auto rgb:p){if(mark&&rgb==mk5wide::kMasked)rgb=0x00ff00;out.put(char(rgb>>16));out.put(char(rgb>>8));out.put(char(rgb));}
            // WIDE_DUMP_SHOWN: the frame as the window shows it (4x: fighter layers over the scaled scenery), wide and arcade.
            if(std::getenv("WIDE_DUMP_SHOWN")){
                const auto shown=[&](const char* name,const std::vector<uint32_t>& pixels,const std::vector<uint32_t>& backdrop,const mk5presentation::FighterFrame& fighters,unsigned w){
                    std::vector<uint32_t> image;
                    if(!fighters.layers.empty())mk5presentation::ComposeFighterFrame4x(fighters,w,H,image);
                    else if(!backdrop.empty())mk5stage::Bicubic4x(pixels,backdrop,w,H,image);
                    else for(unsigned y=0;y<H*4;++y)for(unsigned x=0;x<w*4;++x)image.push_back(pixels[(y/4)*w+x/4]);
                    std::ofstream file(std::filesystem::path(std::getenv("WIDE_DUMP_DIR"))/(name+std::to_string(before)+".ppm"),std::ios::binary);
                    file<<"P6"<<char(10)<<w*4<<" "<<H*4<<char(10)<<255<<char(10);for(auto rgb:image){file.put(char(rgb>>16));file.put(char(rgb>>8));file.put(char(rgb));}
                };
                shown("shown-",p,b,screen.wide_fighter_frame(),W);shown("arcade-",screen.pixels(),screen.backdrop_pixels(),screen.fighter_frame(),X);
            }
        }
        // Diagnostics: WIDE_DRAWS_FRAMES lists each one's staged draws per scenery layer and its measured layers.
        if(const char* list=std::getenv("WIDE_DRAWS_FRAMES");list&&(","+std::string(list)+",").find(","+std::to_string(before)+",")!=std::string::npos){
            std::map<int,std::array<int,6>> per;   // layer -> draws, fixed, lowest column, highest, first line, last
            for(const auto& d:layer.placed_done){auto [it,fresh]=per.try_emplace(d.layer,std::array<int,6>{0,0,d.lo,d.hi,d.row&255,(d.row&255)+d.rows});auto& v=it->second;++v[0];v[1]+=d.fixed;v[2]=std::min<int>(v[2],d.lo);v[3]=std::max<int>(v[3],d.hi);
                v[4]=std::min<int>(v[4],d.row&255);v[5]=std::max<int>(v[5],(d.row&255)+d.rows);}
            for(const auto& [l,v]:per)std::cerr<<"draws frame "<<before<<" layer "<<l<<" n "<<v[0]<<" fixed "<<v[1]<<" x "<<v[2]<<".."<<v[3]<<" y "<<v[4]<<".."<<v[5]<<char(10);
            for(const auto& m:layer.bands_done){int lo=1<<20,hi=-(1<<20);for(const auto& r:m.rows){lo=std::min<int>(lo,r.left);hi=std::max<int>(hi,r.right);}
                std::cerr<<"band frame "<<before<<" layer "<<m.layer<<" full "<<m.full<<" rows "<<m.rows.size()<<" left>="<<lo<<" right<="<<hi<<char(10);}
        }
        if(std::getenv("WIDE_TRACE")){static int was=-1;if(int(layer.arena)!=was){was=layer.arena;   // where the layer's arena flag changes
            std::cerr<<"arena frame "<<before<<" "<<was<<" camera "<<int16_t(word(mk5widescreen::kCameraX))<<" fight "<<layer.fight<<" scene "<<layer.scene<<" builds "<<layer.builds;
            for(const auto& m:layer.bands_done){int lo=1<<20,hi=-(1<<20),narrow=0;for(const auto& r:m.rows){lo=std::min<int>(lo,r.left);hi=std::max<int>(hi,r.right);narrow+=r.left>43&&r.right<491;}
                std::cerr<<" L"<<m.layer<<":full "<<m.full<<" rows "<<m.rows.size()<<" narrow "<<narrow<<" "<<lo<<".."<<hi;}
            std::cerr<<char(10);}}
        if(std::getenv("WIDE_TRACE")){static int was=-1;if(int(layer.open)!=was){was=layer.open;   // where the picture starts or stops showing the page's background
            std::cerr<<"open frame "<<before<<" "<<was<<" mode "<<word(mk5widescreen::kMode)<<" camera "<<int16_t(word(mk5widescreen::kCameraX))<<char(10);}}
        if(std::getenv("WIDE_TRACE")){static std::array<int,3> seen{-1,0,0};   // where the mode or the camera limits change
            if(const std::array<int,3> now{int(word(mk5widescreen::kMode)),int16_t(word(mk5widescreen::kCameraMin)),int16_t(word(mk5widescreen::kCameraMax))};now!=seen){seen=now;
                std::cerr<<"mode frame "<<before<<" mode "<<now[0]<<" limits "<<now[1]<<".."<<now[2]<<" camera "<<int16_t(word(mk5widescreen::kCameraX))<<char(10);}}
        if(std::getenv("WIDE_TRACE")){static int last=1<<20;
            static const auto range=[]{std::pair<long,long> r{1,0};if(const char* e=std::getenv("WIDE_TRACE_RANGE"))std::sscanf(e,"%ld-%ld",&r.first,&r.second);return r;}();   // every frame in it
            const bool every=long(before)>=range.first&&long(before)<=range.second;
            if(every){const auto wide_long=[&](uint32_t bit){return int32_t(uint32_t(uint16_t(word(bit)))|uint32_t(uint16_t(word(bit+16)))<<16);};
                for(unsigned k=5;k<8;++k)std::cerr<<"slot "<<k<<" scroll "<<wide_long(mk5widescreen::kScrollFirst+k*0x20)<<" velocity "<<wide_long(mk5widescreen::kScrollFirst+k*0x20-mk5widescreen::kVelocityGap)<<" ";}
            if(shift!=last||every){last=shift;
            std::cerr<<"shift frame "<<before<<" d "<<shift<<" camera "<<int16_t(word(mk5widescreen::kCameraX))<<" mode "<<word(mk5widescreen::kMode);std::cerr<<" dim "<<runner.Video().WideDim();
            for(const auto& r:layer.ratio)std::cerr<<" "<<r.plane<<"/"<<r.world;
            for(unsigned k=0;k<mk5widescreen::kScrollSlots;++k)std::cerr<<" S"<<k<<"="<<int16_t(word(mk5widescreen::kScrollFirst+k*0x20+16));
            for(const auto& lim:layer.bound)std::cerr<<" ["<<lim[0]<<","<<lim[1]<<"]";
            std::map<int,int> cameras;for(const auto& k:layer.notes_done)cameras[k.layer]=k.camera;
            for(const auto& [l,c]:cameras)std::cerr<<" L"<<l<<"@"<<c;
            std::cerr<<char(10);}}
        // 3, 4: the margins' audit; fight frames apart.
        const unsigned mode=word(mk5widescreen::kMode);const bool fight=mode==2||mode==3||mode==5||mode==6;
        (fight?run.fight:run.other).Add(screen.wide_audit());run.fight_frames+=fight;
        // 7: no fight frame with a lit picture masks more than a speck of its margins (a black bar or block over a sky); a
        // frame with soft sides shows none of its margins (a menu's page still up when the mode word changes).
        run.masked_frames+=fight&&!screen.wide_soft()&&screen.wide_audit().masked>kSpeck&&std::any_of(screen.pixels().begin(),screen.pixels().end(),[](uint32_t rgb){return (rgb&0xffffff)!=0;});
        if(std::getenv("WIDE_TRACE")&&fight){static uint64_t was=0;   // where a fight frame's masked margin pixels change
            if(const uint64_t now=screen.wide_audit().masked;now!=was){was=now;
                unsigned box[4]={W,0,H,0};for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(wide[y*W+x]==mk5wide::kMasked){box[0]=std::min(box[0],x);box[1]=std::max(box[1],x);box[2]=std::min(box[2],y);box[3]=std::max(box[3],y);}
                std::cerr<<"masked frame "<<before<<" "<<now<<" d "<<shift<<" camera "<<int16_t(word(mk5widescreen::kCameraX))<<" mode "<<mode<<" columns "<<box[0]<<".."<<box[1]<<" lines "<<box[2]<<".."<<box[3]<<char(10);}}
        // 5: per layer, the most frequent move of its draws (same image and row in the last build) in the margins and in
        // the centre.
        if(layer.builds!=seen_builds){seen_builds=layer.builds;
            // The draws of one image on one row pair up in column order when both builds have as many of them.
            std::map<std::tuple<uint16_t,uint32_t,uint16_t>,std::array<std::vector<const mk5wide::Note*>,2>> runs;
            for(const auto& k:previous)runs[{k.layer,k.source,k.row}][0].push_back(&k);
            for(const auto& k:layer.notes_done)runs[{k.layer,k.source,k.row}][1].push_back(&k);
            std::map<uint16_t,std::array<std::map<int,int>,2>> moves;   // layer -> [centre, margin] move -> count
            std::map<uint16_t,std::vector<std::string>> margin_pairs;   // diagnostics of a divergence
            for(auto& [key,two]:runs){
                if(two[0].size()!=two[1].size())continue;
                for(auto& v:two)std::sort(v.begin(),v.end(),[](const mk5wide::Note* a,const mk5wide::Note* b){return a->lo<b->lo;});
                for(size_t i=0;i<two[1].size();++i){
                    const auto& k=*two[1][i];
                    const bool in_margin=(k.hi<=mk5wide::kWideMargin||k.lo>=mk5wide::kWideMargin+mk5wide::kVisible)&&k.hi>0&&k.lo<mk5wide::kLayerWidth;   // shown in a margin
                    const bool in_centre=k.lo>=mk5wide::kWideMargin&&k.hi<=mk5wide::kWideMargin+mk5wide::kVisible;
                    if(in_margin||in_centre)++moves[k.layer][in_margin?1:0][k.lo-two[0][i]->lo];
                    if(in_margin)margin_pairs[k.layer].push_back(std::to_string(k.source)+"@"+std::to_string(k.row)+":"+std::to_string(two[0][i]->lo)+">"+std::to_string(k.lo));
                }
            }
            const auto mode_of=[](const std::map<int,int>& counts){int best=0,most=0;for(const auto& [move,count]:counts)if(count>most){most=count;best=move;}return std::make_pair(best,most);};
            std::map<uint16_t,int> camera_move;   // per layer: the scenery's move, minus its list camera's
            {std::map<uint16_t,int> was;for(const auto& k:previous)was[k.layer]=k.camera;
             for(const auto& k:layer.notes_done)if(was.count(k.layer))camera_move[k.layer]=was[k.layer]-k.camera;}
            // 6: with the camera still every plane stands still (a plane's place depends on the camera alone).
            if(!camera_move.empty()&&!camera_move.rbegin()->second){++run.still_checked;
                for(const auto& [l,move]:camera_move)if(move){++run.still_moved;
                    std::cerr<<"parallax still frame "<<before<<" layer "<<l<<" moved "<<move<<" mode "<<word(mk5widescreen::kMode)<<" camera "<<int16_t(word(mk5widescreen::kCameraX))
                             <<" limits "<<int16_t(word(mk5widescreen::kCameraMin))<<".."<<int16_t(word(mk5widescreen::kCameraMax))<<" d "<<shift<<char(10);break;}}
            for(const auto& [l,both]:moves)if(!both[0].empty()&&!both[1].empty()&&camera_move.count(l)&&camera_move[l]&&mode_of(both[0]).first==camera_move[l]){
                const auto c=mode_of(both[0]),g=mode_of(both[1]);   // the centre's scenery moved with the camera: the margins' must too
                if(c.second>=3&&g.second>=3){   // at least 3 draws moved alike on each side: scenery, not one moving object
                    ++run.parallax_checked;run.parallax_differ+=c.first!=g.first;
                    if(c.first!=g.first)std::cerr<<"parallax frame "<<before<<" layer "<<l<<" centre "<<c.first<<"x"<<c.second<<" margin "<<g.first<<"x"<<g.second<<" camera "<<int16_t(word(mk5widescreen::kCameraX));
                    if(c.first!=g.first)for(const auto& d:margin_pairs[l])std::cerr<<" "<<d;
                    if(c.first!=g.first)std::cerr<<char(10);}
            }
            // 12: the finished build's lists against the world's camera (the last list's), in a fight under the same limits.
            {static std::array<int,2> limits_were{1,0};const std::array<int,2> limits{int16_t(word(mk5widescreen::kCameraMin)),int16_t(word(mk5widescreen::kCameraMax))};
             std::map<uint16_t,int> cameras;for(const auto& k:layer.notes_done)cameras[k.layer]=k.camera;
             if(fight&&limits==limits_were&&!cameras.empty())for(const auto& [l,c]:cameras)run.pace[{limits[0],limits[1],l}][cameras.rbegin()->second]={c,int(before)};
             limits_were=limits;}
            previous=layer.notes_done;
        }
        // The edge coverage table (fight frames): per layer measured this build, the margin columns its art covers.
        if(fight)for(const auto& band:layer.bands_done){
            auto& c=run.coverage[{int16_t(word(mk5widescreen::kCameraMin)),int16_t(word(mk5widescreen::kCameraMax)),band.layer}];
            const int camera=int16_t(word(mk5widescreen::kCameraX));
            c.camera_low=std::min(c.camera_low,camera);c.camera_high=std::max(c.camera_high,camera);++c.frames;
            for(const auto& r:band.rows){
                c.left=std::min(c.left,std::max(0,mk5wide::kWideMargin-std::max<int>(r.left,0)));
                c.right=std::min(c.right,std::max(0,std::min<int>(r.right,mk5wide::kLayerWidth)-(mk5wide::kWideMargin+mk5wide::kVisible)));
            }
        }
    }
    check(screen.frames()==tape.size(),"legal tape endpoint not reached");
    for(const auto& [key,at]:run.pace){
        const int w0=at.begin()->first,p0=at.begin()->second[0],w1=at.rbegin()->first,p1=at.rbegin()->second[0];
        if(w1-w0<kPaceRange)continue;
        ++run.pace_checked;
        for(const auto& [w,place]:at)if(const int line=p0+int(int64_t(p1-p0)*(w-w0)/(w1-w0));std::abs(place[0]-line)>kPace){++run.pace_off;
            std::cerr<<"pace arena "<<std::get<0>(key)<<".."<<std::get<1>(key)<<" list "<<std::get<2>(key)<<" world camera "<<w<<" list camera "<<place[0]<<" even "<<line<<" frame "<<place[1]
                     <<" from "<<w0<<":"<<p0<<" to "<<w1<<":"<<p1<<char(10);break;}
    }
    run.cmos=f.cmos;run.ring=runner.Video().wide_ring;run.faults=runner.Video().wide_faults;run.streamer_skips=runner.Video().wide_streamer_skips;
    return run;
}
std::string Json(const mk5wide::Audit& a){
    return "{\"shown\":"+std::to_string(a.shown)+",\"sentinel\":"+std::to_string(a.sentinel)+",\"masked\":"+std::to_string(a.masked)+
           ",\"outside\":"+std::to_string(a.outside)+",\"generated\":"+std::to_string(a.generated)+",\"stale\":"+std::to_string(a.stale)+"}";
}
}
int wmain(int argc,wchar_t** argv){
    try{
        check(argc==4,"usage: root tape seed");
        _wputenv(L"TRIUNIT_WIDE_DEBUG=1");
        std::ifstream timeline(argv[2]);check(bool(timeline),"missing legal tape");std::string header;std::getline(timeline,header);
        std::vector<uint32_t> tape;unsigned frame,duration;uint32_t mask;
        while(timeline>>frame>>duration>>mask){if(tape.size()<frame+duration)tape.resize(frame+duration);for(unsigned k=0;k<duration;++k)tape[frame+k]=mask;}
        const auto cmos=LoadMk5CmosSnapshot(argv[3]);
        const bool wide_only=std::getenv("WIDE_ONLY");   // diagnostics: no 4:3 run, criterion 1 not checked, never a pass
        const auto wide=Play(argv[1],tape,cmos,true),plain=wide_only?wide:Play(argv[1],tape,cmos,false);
        check(plain.frames.size()==wide.frames.size(),"frame counts differ");
        for(size_t i=0;i<wide.frames.size();++i){
            check(plain.frames[i].state==wide.frames[i].state,"1: widescreen changed the game state at frame "+std::to_string(i));
            check(plain.frames[i].shown==wide.frames[i].shown,"1: widescreen changed the shown frame or its Remastered layers at frame "+std::to_string(i));
        }
        check(plain.pcm==wide.pcm && plain.cmos==wide.cmos,"1: widescreen changed the PCM or the CMOS");
        check(!wide.faults,"the shadow pass faulted");
        std::cout<<"{\"frames\":"<<wide.frames.size()<<",\"fight_frames\":"<<wide.fight_frames<<",\"centre_differ\":"<<wide.centre
                 <<",\"presented_differ\":"<<wide.presented<<",\"filled\":"<<wide.filled<<",\"joined\":"<<wide.joined<<",\"masked_frames\":"<<wide.masked_frames<<",\"shifted_frames\":"<<wide.shifted_frames<<",\"shifted_differ\":"<<wide.shifted_differ
                 <<",\"shifted_masked\":"<<wide.shifted_masked<<",\"still_checked\":"<<wide.still_checked<<",\"still_moved\":"<<wide.still_moved<<",\"fight\":"<<Json(wide.fight)<<",\"other\":"<<Json(wide.other)
                 <<",\"streamer_skips\":"<<wide.streamer_skips<<",\"ring\":"<<wide.ring<<",\"pace_checked\":"<<wide.pace_checked<<",\"pace_off\":"<<wide.pace_off<<",\"parallax_checked\":"<<wide.parallax_checked<<",\"parallax_differ\":"<<wide.parallax_differ<<",\"coverage\":[";
        bool first=true;
        for(const auto& [key,c]:wide.coverage){
            std::cout<<(first?"":",")<<"["<<std::get<0>(key)<<","<<std::get<1>(key)<<","<<std::get<2>(key)<<","<<c.frames<<","<<c.camera_low<<","<<c.camera_high<<","<<c.left<<","<<c.right<<"]";first=false;
        }
        std::cout<<"]}\n";
        // The raw layer has no floor blood (the presentation lays it over both): the shown frame is the one compared.
        check(!wide.presented,"2: the wide frame's centre differs from the arcade frame");
        check(!wide.fight.sentinel,"3: a fight frame shows a margin pixel no draw wrote");
        check(!wide.fight.outside && !wide.other.outside && !wide.fight.generated && !wide.other.generated && !wide.fight.stale,
              "4: a margin pixel does not belong to a draw meant to cover its column");
        check(!wide.parallax_differ && wide.parallax_checked,"5: a layer moves differently in the margins and in the centre");
        check(!wide.still_moved,"6: a scenery plane moved while the camera stood still");
        check(!wide.masked_frames,"7: a lit fight frame masks part of its margins");
        check(!wide.pace_off,"12: a far plane does not move at an even pace");
        check(!wide_only,"WIDE_ONLY: diagnostic run, criterion 1 not checked");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
