"""MK2 part of the widescreen acceptance (tools/widescreen/acceptance.py): the replay harness of tools/mk2/replay.py with
the wide layer and its diagnostics (TRIUNIT_WIDE_DEBUG) when TRIUNIT_WIDE is set. Per shown frame it logs the VRAM and
palette hash, the shown frame's hash, the centre against the arcade frame, the margin audit, the parallax comparison and
the measured scenery layers and the floor's lines between builds whose arcade camera moved under a wide camera standing
on its limit; per live chunk (about 17 frames) the RAM, CPU registers and cycles, and the arena's camera
limits. The same replay without and with the layer must log the same game; the criteria are checked here."""
import json,os,subprocess,sys
from collections import defaultdict
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.quality import run as q

REPLAYS=os.environ.get('ACCEPT_REPLAYS','finisher_replays/case00,finisher_replays/case05,finisher_replays/natural-f01-stage-a,walk-red-sky,lost-left').split(',')
ALL=['flow_replays/campaign-f10-very-easy-ending-return']
# The mode word's fight values: 2 the fight, 6 the round's start, 7 (shifts with the fight), 3 the finisher, 5 the round's end.
FIGHT_MODES={2,3,5,6,7}

HEADER=r'''#include <algorithm>
#include <map>
'''
FRAME=r'''
            if(scanout.frames()!=before){
                const auto fnv=[](uint64_t h,const auto& v){for(auto x:v)h=(h^uint64_t(x))*0x100000001b3ull;return h;};
                const uint64_t basis=14695981039346656037ull;
                const unsigned W=mk2::Scanout::kWideWidth,X=scanout.width(),H=scanout.height(),margin=(W-X)/2;
                uint64_t centre=0;
                if(accept_wide&&!scanout.wide_whole()){const auto& w=scanout.wide_pixels();for(unsigned y=0;y<H;++y)for(unsigned x=0;x<X;++x)centre+=w[y*W+margin+x]!=scanout.pixels()[y*X+x];}
                if(accept_wide&&std::getenv("ACCEPT_TRACE")){static int flags=-1;const auto& l=video.wide;const int now=int(l.fight)|int(l.scene)<<1|int(l.arena)<<2|int(l.whole)<<3;
                    if(now!=flags){flags=now;std::cerr<<"OUT state frame "<<scanout.frames()<<" fight "<<l.fight<<" scene "<<l.scene<<" arena "<<l.arena<<" whole "<<l.whole<<" limits "<<l.limits[0]<<".."<<l.limits[1]<<"/"<<l.limits[2]<<" held "<<l.held[0]<<","<<l.held[1]<<char(10);}}
                if(accept_wide&&std::getenv("ACCEPT_TRACE")){static unsigned seen=0,quiet=0;const auto& l=video.wide;   // display frames without a build (Layer::Paused)
                    if(l.builds==seen)++quiet;else{if(quiet>=3)std::cerr<<"OUT gap frame "<<scanout.frames()<<" frames without a build "<<quiet<<" shift "<<l.shift<<char(10);quiet=0;seen=l.builds;}}
                if(centre&&std::getenv("ACCEPT_TRACE")){static int traced=0;if(traced++<60)std::cerr<<"OUT centre frame "<<scanout.frames()<<" differ "<<centre<<" shift "<<scanout.wide_shift()<<char(10);}
                if(accept_wide&&std::getenv("ACCEPT_DUMP_FRAMES")&&(","+std::string(std::getenv("ACCEPT_DUMP_FRAMES"))+",").find(","+std::to_string(scanout.frames())+",")!=std::string::npos){   // diagnostics
                    std::ofstream dump(std::filesystem::path(std::getenv("ACCEPT_DUMP_DIR"))/("mk2-"+std::to_string(scanout.frames())+".ppm"),std::ios::binary);
                    dump<<"P6"<<char(10)<<W<<" "<<H<<char(10)<<255<<char(10);for(auto rgb:scanout.wide_pixels()){dump.put(char(rgb>>16));dump.put(char(rgb>>8));dump.put(char(rgb));}
                    std::ofstream narrow(std::filesystem::path(std::getenv("ACCEPT_DUMP_DIR"))/("arcade-"+std::to_string(scanout.frames())+".ppm"),std::ios::binary);
                    narrow<<"P6"<<char(10)<<X<<" "<<H<<char(10)<<255<<char(10);for(auto rgb:scanout.pixels()){narrow.put(char(rgb>>16));narrow.put(char(rgb>>8));narrow.put(char(rgb));}
                    std::ofstream raw(std::filesystem::path(std::getenv("ACCEPT_DUMP_DIR"))/("vram-"+std::to_string(scanout.frames())+".bin"),std::ios::binary);   // both pages' pens, then the palette
                    raw.write(reinterpret_cast<const char*>(video.vram.data()),std::streamsize(512*512*2));raw.write(reinterpret_cast<const char*>(video.palette_ram.data()),std::streamsize(video.palette_ram.size()*2));
                }
                if(accept_wide&&std::getenv("ACCEPT_DRAWS_FRAMES")&&(","+std::string(std::getenv("ACCEPT_DRAWS_FRAMES"))+",").find(","+std::to_string(scanout.frames())+",")!=std::string::npos){   // diagnostics
                    std::ofstream draws(std::getenv("ACCEPT_DRAWS_FILE")?std::getenv("ACCEPT_DRAWS_FILE"):"NUL",std::ios::app);
                    for(const auto& d:video.wide.placed_done)draws<<"draw "<<scanout.frames()<<" L"<<d.layer<<(d.fixed?" F":" W")<<" src "<<std::hex<<d.source<<" ctl "<<d.control<<std::dec<<" x "<<d.lo<<".."<<d.hi<<" y "<<d.row<<"+"<<d.rows<<char(10);}
                if(accept_wide&&std::getenv("ACCEPT_PAGE_FRAMES")&&(","+std::string(std::getenv("ACCEPT_PAGE_FRAMES"))+",").find(","+std::to_string(scanout.frames())+",")!=std::string::npos){   // diagnostics: each page's build and who wrote its pixels (centre/margins by kind)
                    const auto& l=video.wide;
                    for(unsigned p=0;p<2;++p){unsigned kinds[2][7]={};
                        for(size_t row=size_t(p)*256;row<size_t(p)*256+254;++row)for(int L=0;L<mk5wide::kLayerWidth;++L){const size_t at=row*mk5wide::kLayerWidth+size_t(L);
                            ++kinds[l.Arcade(L)?0:1][l.written[at]>l.page_start[p]?size_t(l.origin[at].kind):0];}
                        std::cerr<<"OUT page frame "<<scanout.frames()<<" page "<<p<<(p==l.page?"*":"")<<" builds "<<l.builds<<" dropped "<<l.dropped<<" whole "<<l.page_whole[p]<<" start "<<l.page_start[p]<<" cleared_at "<<l.page_cleared_at[p]<<" serial "<<l.serial<<" cleared "<<l.cleared[p]<<" soft "<<l.page_soft[p]<<" idle "<<l.idle<<" centre";
                        for(unsigned k=0;k<14;++k)std::cerr<<(k==7?" margins ":" ")<<kinds[k/7][k%7];
                        std::cerr<<char(10);}}
                if(accept_wide&&std::getenv("ACCEPT_ROW_FRAMES")&&(","+std::string(std::getenv("ACCEPT_ROW_FRAMES"))+",").find(","+std::to_string(scanout.frames())+",")!=std::string::npos){   // diagnostics: who wrote a line's pixels
                    const auto& l=video.wide;std::string rows=std::getenv("ACCEPT_ROWS")?std::getenv("ACCEPT_ROWS"):"100";
                    for(size_t at=0;at<rows.size();at=rows.find(',',at)==std::string::npos?rows.size():rows.find(',',at)+1)for(unsigned p=0;p<2;++p){
                        const size_t row=(size_t(p)*256+size_t(std::atoi(rows.c_str()+at)))*mk5wide::kLayerWidth;
                        std::cerr<<"OUT row frame "<<scanout.frames()<<" page "<<p<<(p==l.page?"*":"")<<" y "<<std::atoi(rows.c_str()+at)<<" start "<<l.page_start[p]<<":";
                        for(int L=0,from=0;L<=mk5wide::kLayerWidth;++L){const auto& o=l.origin[row+size_t(from)];
                            if(L==mk5wide::kLayerWidth||l.origin[row+size_t(L)].kind!=o.kind||l.origin[row+size_t(L)].lo!=o.lo||(l.written[row+size_t(L)]>l.page_start[p])!=(l.written[row+size_t(from)]>l.page_start[p])||(l.fixed[row+size_t(L)]&l.kMask)!=(l.fixed[row+size_t(from)]&l.kMask)){
                                std::cerr<<" ["<<from<<".."<<L<<" k"<<int(o.kind)<<" "<<o.lo<<".."<<o.hi<<(l.written[row+size_t(from)]>l.page_start[p]?" new":" old")<<(l.fixed[row+size_t(from)]&l.kMask?" mask":"")<<"]";from=L;}}
                        std::cerr<<char(10);}}
                if(accept_wide&&std::getenv("ACCEPT_TRACE")){static int last=1<<20;if(scanout.wide_shift()!=last){last=scanout.wide_shift();std::cerr<<"OUT shift frame "<<scanout.frames()<<" d "<<last<<char(10);}}
                if(accept_wide&&std::getenv("ACCEPT_TRACE")){static uint64_t flown=0;if(video.wide_flown!=flown){flown=video.wide_flown;std::cerr<<"OUT flown frame "<<scanout.frames()<<" draws "<<flown<<char(10);}}   // a parked far object's flight continued (BootVideo::WideMove)
                if(accept_wide&&std::getenv("ACCEPT_TRACE")){static int streamer=-1;if(int(video.margin_streamer)!=streamer){streamer=video.margin_streamer;   // 0: the object pool was reset (the planes are gone)
                    std::cerr<<"OUT pool frame "<<scanout.frames()<<" streamer "<<streamer<<" fight "<<video.wide.fight<<" shift "<<video.wide.shift<<char(10);}}
                const auto& a=scanout.wide_audit();const auto& layer=video.wide;
                uint64_t checked=0,differ=0,floor_pairs=0,floor=0,flight_cut=0,flights=0;
                if(accept_wide&&layer.builds!=accept_builds){accept_builds=layer.builds;
                    // 15: a far object's flight (BootVideo::WideMove) starts and ends off the wide picture: none shows up
                    // with more than kEdge columns inside it unless the game started it in view (id bit 0), and none is
                    // gone so unless the game ended it in view (its object on the arcade's own screen: s[2]).
                    {static std::map<uint32_t,std::array<int,3>> was;static bool was_fight=false;constexpr int kEdge=12;
                     std::map<uint32_t,std::array<int,3>> now;for(const auto& s:video.wide_seen_done)now[s.id]={s.lo,s.hi,int(s.on_arcade)};
                     const auto inside=[&](const std::array<int,3>& s){return s[1]>kEdge&&s[0]<mk5wide::kLayerWidth-kEdge;};
                     const auto cut=[&](const char* what,uint32_t id,const std::array<int,3>& s){++flight_cut;
                         if(std::getenv("ACCEPT_TRACE"))std::cerr<<"OUT flight frame "<<scanout.frames()<<" "<<what<<" inside the picture: id "<<id<<" columns "<<s[0]<<".."<<s[1]<<char(10);};
                     if(layer.fight&&was_fight){
                         for(const auto& [id,s]:now)if(!was.count(id)&&!(id&1)&&inside(s))cut("starts",id,s);
                         for(const auto& [id,s]:was)if(!now.count(id)&&!s[2]&&inside(s))cut("ends",id,s);
                     }
                     flights=now.size();was=std::move(now);was_fight=layer.fight;}
                    // 13: the floor (its producer's lines, each as wide as the layer: line, image source, column) is the same in
                    // two builds between which the arcade's camera moved under a wide camera standing on its limit (the
                    // offset changed, same side, no further than the margin), when both drew the same lines.
                    {static int floor_d=0;static std::vector<std::array<int,3>> floor_was;std::vector<std::array<int,3>> floor_now;
                     for(const auto& k:layer.notes_done)if(k.hi-k.lo==mk5wide::kLayerWidth)floor_now.push_back({int(k.row),int(k.source),int(k.lo)});
                     const int d=layer.shift_done;
                     if(d&&floor_d&&d!=floor_d&&(d<0)==(floor_d<0)&&std::abs(d)<=mk5wide::kWideMargin&&std::abs(floor_d)<=mk5wide::kWideMargin&&
                        !floor_now.empty()&&floor_now.size()==floor_was.size()&&
                        std::equal(floor_now.begin(),floor_now.end(),floor_was.begin(),[](const auto& p,const auto& r){return p[0]==r[0];})){
                         ++floor_pairs;floor+=floor_now!=floor_was;
                         if(floor_now!=floor_was&&std::getenv("ACCEPT_TRACE")){static int traced=0;if(traced++<20){size_t i=0;while(floor_now[i]==floor_was[i])++i;
                             std::cerr<<"OUT floor frame "<<scanout.frames()<<" d "<<floor_d<<" > "<<d<<" lines "<<floor_now.size()<<" first "<<i<<" line "<<floor_now[i][0]<<std::hex<<" source "<<floor_was[i][1]<<" > "<<floor_now[i][1]<<std::dec<<" column "<<floor_was[i][2]<<" > "<<floor_now[i][2]
                                      <<" last source "<<std::hex<<floor_was.back()[1]<<" > "<<floor_now.back()[1]<<std::dec<<char(10);}}}
                     floor_was=std::move(floor_now);floor_d=d;}
                    std::map<std::tuple<uint16_t,uint32_t,uint16_t>,std::array<std::vector<const mk5wide::Note*>,2>> runs;
                    for(const auto& k:accept_previous)runs[{k.layer,k.source,k.row}][0].push_back(&k);
                    for(const auto& k:layer.notes_done)runs[{k.layer,k.source,k.row}][1].push_back(&k);
                    std::map<uint16_t,std::array<std::map<int,int>,2>> moves;
                    for(auto& [key,two]:runs){
                        if(two[0].size()!=two[1].size())continue;
                        for(auto& v:two)std::sort(v.begin(),v.end(),[](const mk5wide::Note* p,const mk5wide::Note* r){return p->lo<r->lo;});
                        for(size_t i=0;i<two[1].size();++i){const auto& k=*two[1][i];
                            const bool in_margin=(k.hi<=mk5wide::kWideMargin||k.lo>=mk5wide::kWideMargin+mk5wide::kVisible)&&k.hi>0&&k.lo<mk5wide::kLayerWidth;   // shown in a margin
                            const bool in_centre=k.lo>=mk5wide::kWideMargin&&k.hi<=mk5wide::kWideMargin+mk5wide::kVisible;
                            if(in_margin||in_centre)++moves[k.layer][in_margin?1:0][k.lo-two[0][i]->lo];}
                    }
                    const auto mode_of=[](const std::map<int,int>& c){int best=0,most=0;for(const auto& [m,n]:c)if(n>most){most=n;best=m;}return std::make_pair(best,most);};
                    std::map<uint16_t,int> camera_move;{std::map<uint16_t,int> was;for(const auto& k:accept_previous)was[k.layer]=k.camera;
                        for(const auto& k:layer.notes_done)if(was.count(k.layer))camera_move[k.layer]=was[k.layer]-k.camera;}
                    for(const auto& [l,both]:moves)if(!both[0].empty()&&!both[1].empty()&&camera_move.count(l)&&camera_move[l]&&mode_of(both[0]).first==camera_move[l]){const auto c=mode_of(both[0]),g=mode_of(both[1]);
                        if(c.second>=3&&g.second>=3){++checked;differ+=c.first!=g.first;
                            if(c.first!=g.first&&std::getenv("ACCEPT_TRACE"))std::cerr<<"OUT parallax frame "<<scanout.frames()<<" layer "<<l<<" centre "<<c.first<<"x"<<c.second<<" margin "<<g.first<<"x"<<g.second<<"\n";}}
                    accept_previous=layer.notes_done;
                }
                accept_log<<"F\t"<<scanout.frames()<<"\t"<<fnv(fnv(basis,video.vram),video.palette_ram)<<"\t"<<fnv(basis,scanout.pixels())<<"\t"<<centre
                          <<"\t"<<a.shown<<"\t"<<a.sentinel<<"\t"<<a.masked<<"\t"<<a.outside<<"/"<<a.outside_real<<"\t"<<a.generated<<"\t"<<a.stale
                          <<"\t"<<checked<<"\t"<<differ<<"\t"<<video.margin_faults<<"\t"<<floor<<"\t"<<scanout.wide_shift()<<"\t"<<floor_pairs<<"\t"<<video.wide_flown<<"\t"<<flight_cut<<"\t"<<flights<<"\t"<<(accept_wide?layer.bands_done.size():0);
                if(accept_wide)for(const auto& b:layer.bands_done){int left=mk5wide::kWideMargin,right=mk5wide::kWideMargin;
                    for(const auto& r:b.rows){left=std::min(left,std::max(0,mk5wide::kWideMargin-std::max<int>(r.left,0)));
                        right=std::min(right,std::max(0,std::min<int>(r.right,mk5wide::kLayerWidth)-(mk5wide::kWideMargin+mk5wide::kVisible)));}
                    accept_log<<"\t"<<b.layer<<":"<<left<<":"<<right;}
                accept_log<<"\n";
                if(accept_wide&&a.outside&&std::getenv("ACCEPT_TRACE")&&accept_traced<40){
                    for(size_t i=0;i<mk5wide::kLayerSize&&accept_traced<40;++i){const int L=int(i%mk5wide::kLayerWidth);const auto& o=layer.origin[i];
                        if(!layer.Arcade(L)&&layer.Shown(i)&&(o.kind==mk5wide::Kind::real||o.kind==mk5wide::Kind::shadow)&&!o.Covers(L)){
                            ++accept_traced;std::cerr<<"OUT frame "<<scanout.frames()<<" row "<<i/mk5wide::kLayerWidth<<" col "<<L<<" lo "<<o.lo<<" hi "<<o.hi<<" kind "<<int(o.kind)<<"\n";i+=mk5wide::kLayerWidth-L;}}}
            }'''
CHUNK=r'''
            {const auto& st=current.State();const auto& rs=st.ready;const auto fnv=[](uint64_t h,const auto& v){for(auto x:v)h=(h^uint64_t(x))*0x100000001b3ull;return h;};
             const auto word=[&](uint32_t bit){return int16_t(st.main_ram[(bit-0x01000000)/16]);};
             if(std::getenv("ACCEPT_TRACE")){static uint64_t was=0;const auto ram_long=[&](uint32_t bit){return uint32_t(uint16_t(word(bit)))|uint32_t(uint16_t(word(bit+16)))<<16;};   // the floor producer's velocity pointer and routine
                 if(const uint64_t now=uint64_t(ram_long(mk2::wide::kFloorVelocity))<<32|ram_long(mk2::wide::kFloorRoutine);now!=was){was=now;
                     std::cerr<<"OUT floorptr frame "<<scanout.frames()<<std::hex<<" velocity "<<(now>>32)<<" routine "<<uint32_t(now)<<std::dec<<" limits "<<word(mk2::wide::kCameraMin)<<".."<<word(mk2::wide::kCameraMax)<<" mode "<<word(mk2::wide::kMode)<<char(10);}}
             accept_log<<"C\t"<<scanout.frames()<<"\t"<<fnv(fnv(fnv(14695981039346656037ull,st.main_ram),rs.a),rs.b)<<"\t"<<rs.cycles<<"\t"<<rs.status
                       <<"\t"<<word(mk2::wide::kCameraX)<<"\t"<<word(mk2::wide::kCameraMin)<<"\t"<<word(mk2::wide::kCameraMax)<<"\t"<<word(mk2::wide::kMode)<<"\n";}'''


def source():
    from tools.mk2 import replay
    s=replay.recovery_source()
    def sub(a,b):
        nonlocal s
        assert s.count(a)==1,a
        s=s.replace(a,b)
    s=HEADER+s
    sub('mk2::Scanout scanout;','const bool accept_wide=std::getenv("TRIUNIT_WIDE")!=nullptr;mk2::Scanout scanout;if(accept_wide)scanout.EnableWide();'
        'std::ofstream accept_log(output/"accept.tsv");unsigned accept_builds=0;std::vector<mk5wide::Note> accept_previous;unsigned accept_traced=0;')
    sub('mk2::GeneratedBoot generated(program,*machine,&image,0xfc7d);',
        'mk2::GeneratedBoot generated(program,*machine,&image,0xfc7d);if(accept_wide)generated.OnVideoReady([](mk2::BootVideo& v){v.EnableWide();});')
    sub('const auto before=scanout.frames();scanout.Line(edge,video);','const auto before=scanout.frames();scanout.Line(edge,video);'+FRAME)
    marker='            current=mk2::ResumeValidLiveHost('
    assert s.count(marker)==1
    i=s.index(marker);j=s.index('\n',i)+1
    s=s[:j]+CHUNK+'\n'+s[j:]
    return s


def play(exe,tape,out,wide):
    from tools.mk2 import replay
    cap=replay.read_capture(tape.read_bytes().decode('utf8'))
    out.mkdir(parents=True,exist_ok=True)
    (out/'inputs.txt').write_text(''.join(f'{t} {v}\n' for t,v in cap['inputs']));(out/'cmos.bin').write_bytes(cap['cmos'])
    env={**os.environ,'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ['PATH']}
    if wide:env.update(TRIUNIT_WIDE='1',TRIUNIT_WIDE_DEBUG='1')
    r=subprocess.run([str(exe),str(ROOT/'roms/mk2'),'--smoke',str(cap.get('end_cycles')),'replay',str(out),'2000000',str(out/'inputs.txt'),
                      str(out/'cmos.bin'),cap['cmos_sha256']],cwd=str(ROOT),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=14400)
    rows=[l.split('\t') for l in (out/'accept.tsv').read_text().splitlines()]
    return r,rows


def run(all_replays,table):
    from tools.mk2 import package_native as package
    from tools.setup import mk2
    from tools.widescreen import mk2_tapes
    mk2.inc_dir()
    failures=[];flown=0;measured=0   # fight frames with a scenery layer's edge in a margin (the mask at work), all replays
    with q.scratch_tree() as (scratch,cleanup):
        (scratch/'accept.cpp').write_text(source())
        exe=scratch/'accept.exe';env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ['PATH']}
        b=q.command([q.DEFAULT_CC,*package.FLAGS,scratch/'accept.cpp',*[ROOT/p for p in package.SOURCES if p!='native/mk2/mk2_native_check.cpp'],
                     *package.LIBRARIES,'-o',exe],scratch,cwd=ROOT,env=env,timeout=3600)
        if not b['passed']:
            return ['MK2 build: '+' '.join(u.get('output_tail','')[-1500:] for u in b.get('build_units',[]) if not u['passed'])]
        for name in REPLAYS+(ALL if all_replays else []):
            tape=ROOT/'tools/mk2'/f'{name}.json'
            if name in mk2_tapes.MADE:   # a generated replay (tools/widescreen/mk2_tapes.py)
                tape=scratch/f'{name}.json';tape.write_text(json.dumps(mk2_tapes.MADE[name]()))
            (r0,plain),(r1,wide)=play(exe,tape,scratch/'plain',False),play(exe,tape,scratch/'wide',True)
            bad=[]
            if os.environ.get('ACCEPT_TRACE'):print(r1.stderr[-int(os.environ.get('ACCEPT_TRACE_TAIL','4000')):])
            if r0.returncode or r1.returncode:bad.append(f'exit {r0.returncode}/{r1.returncode}: {(r1.stderr or r0.stderr)[-600:]}')
            fp=[x for x in plain if x[0]=='F'];fw=[x for x in wide if x[0]=='F'];cp=[x for x in plain if x[0]=='C'];cw=[x for x in wide if x[0]=='C']
            if len(fp)!=len(fw) or any(a[1:4]!=b[1:4] for a,b in zip(fp,fw)):bad.append('1: widescreen changed VRAM, palette or a shown frame')
            if len(cp)!=len(cw) or any(a[1:6]!=b[1:6] for a,b in zip(cp,cw)):bad.append('1: widescreen changed RAM, CPU or cycles')
            if r0.stdout.splitlines()[-1:]!=r1.stdout.splitlines()[-1:]:bad.append('1: the replay ends differently')
            # Each frame takes its live chunk's camera, limits and mode; a fight frame (FIGHT_MODES) is an arena's.
            limits=[(int(c[1]),int(c[6]),int(c[7]),int(c[5]),int(c[8])) for c in cw]
            if os.environ.get('ACCEPT_TRACE'):   # diagnostics: where the mode or the camera limits change
                for (f,low,high,camera,mode),before in zip(limits,[None]+limits):
                    if before is None or (mode,low,high)!=(before[4],before[1],before[2]):print(f'  chunk frame {f}: mode {mode} limits {low}..{high} camera {camera}')
            def chunk(frames):
                k=0
                for x in frames:
                    while limits and k<len(limits)-1 and limits[k][0]<int(x[1]):k+=1
                    yield x,(limits[k] if limits else None)
            total=defaultdict(int);arena=defaultdict(int)
            for x,c in chunk(fw):
                outside,outside_real=x[8].split('/');x=x[:8]+[outside]+x[9:];total['outside_real']+=int(outside_real)
                fight=c is not None and c[4] in FIGHT_MODES
                arena['frames']+=fight;arena['measured']+=fight and int(x[20])>0;arena['flight_cut']+=int(x[18]) if c is not None and c[4]==2 else 0;arena['flights']+=int(x[19])>0;total['floor']+=int(x[14]);total['shifted']+=int(x[15])!=0;total['floor_pairs']+=int(x[16])
                for k,v in zip(('centre','shown','sentinel','masked','outside','generated','stale','checked','differ'),x[4:13]):
                    total[k]+=int(v)
                    if fight:arena[k]+=int(v)
            faults=int(fw[-1][13]) if fw else 0
            if total['centre']:bad.append(f"2: the centre differs from the arcade frame ({total['centre']} pixels)")
            if arena['sentinel']:bad.append(f"3: a fight frame shows {arena['sentinel']} margin pixels no draw wrote")
            if total['outside'] or total['generated'] or arena['stale']:bad.append('4: a margin pixel does not belong to a draw meant to cover its column')
            if total['differ'] or not total['checked']:bad.append(f"5: parallax checked {total['checked']}, differ {total['differ']}")
            if faults:bad.append(f'the shadow pass faulted {faults} times')
            if arena['flight_cut']:bad.append(f"15: a far object's flight started or ended inside the wide picture ({arena['flight_cut']} times)")
            if total['floor']:bad.append(f"13: the floor moved under a wide camera standing on its limit ({total['floor']} of {total['floor_pairs']} builds)")
            # The edge coverage table: each frame takes the arena (camera limits) of its live chunk.
            coverage={}
            for x,c in chunk(fw):
                if c is None:break
                _,low,high,camera,mode=c
                for band in x[21:]:
                    layer,left,right=map(int,band.split(':'))
                    c=coverage.setdefault((low,high,layer),[low,high,layer,0,camera,camera,67,67])
                    c[3]+=1;c[4]=min(c[4],camera);c[5]=max(c[5],camera);c[6]=min(c[6],left);c[7]=min(c[7],right)
            if bad:   # where: the first frames with a margin fault, with their live chunk's camera, limits and mode
                shown=0
                for x,c in chunk(fw):
                    if c is not None and c[4] in FIGHT_MODES and (int(x[6]) or int(x[10])) and shown<12:
                        shown+=1;print(f'  frame {x[1]}: sentinel {x[6]} stale {x[10]} layers {x[21:]} chunk {c[1:]}')
            print(f'MK2 {name}:','FAIL' if bad else 'PASS',{'frames':len(fw),'faults':faults,**total,'fight_frames':arena['frames'],'fight_measured':arena['measured'],
                   'fight_sentinel':arena['sentinel'],'fight_stale':arena['stale'],'flown':int(fw[-1][17]) if fw else 0,'flight_frames':arena['flights'],'flight_cut':arena['flight_cut']})
            if coverage:table(f'MK2 {name}: edge coverage (camera from the live chunk)',list(coverage.values()))
            failures+=[f'MK2 {name}: {m}' for m in bad];measured+=arena['measured'];flown+=int(fw[-1][17]) if fw else 0
    assert not cleanup['exists_after']
    if not measured:failures.append('MK2: 3: no fight frame met the edge of a scenery layer (the mask was never exercised)')
    if not flown:failures.append("MK2: 14: no far object's flight went on in a margin after the game parked it (the red-sky arena's dragons were never reached)")
    return failures
