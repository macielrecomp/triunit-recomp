#include "mk5_live_video.hpp"
#include "mk5_widescreen.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
constexpr const char* graphics_hash="f897bdabf9d58d5c52d84eb6c50b8ce9ebcf228b152f731987412c1dbacb3641";
constexpr const char* bank_hash="bba1f99f49c7545bc0c70c7b13185dff06ea6cf7c6668b7f04da568a5a82a20f";

uint32_t dma_source(uint32_t source){
    if(source>=0x02000000)source-=0x02000000;
    if(source>=0xf8000000)source-=0xf8000000;
    return source;
}
unsigned graphics_pixel(const std::vector<uint8_t>& gfx,uint32_t bit,unsigned bpp){
    const size_t byte=bit/8;
    return ((gfx.at(byte)|(unsigned(gfx.at(byte+1))<<8))>>(bit&7))&((1u<<bpp)-1);
}
bool actor_slot(uint32_t bit){
    return bit>=0x010080d0 && bit<0x01041630 && (bit-0x010080d0)%0x290==0;
}
uint32_t ram_pointer(const Mk5MemoryBuffers& memory,uint32_t bit){
    const auto offset=(bit-0x01000000)/16;
    return memory.ram[offset]|(uint32_t(memory.ram[offset+1])<<16);
}
// A non-stream DMA command over registers r (r[1] the command): its size, scale and source; false outside the supported
// modes (skip compression, more than 1048576 output samples, a source beyond the graphics ROMs).
struct DmaShape {unsigned width,height,bpp,xstep,ystep;uint32_t source;bool fill;};
bool Shape(const std::array<uint16_t,18>& r,const std::vector<uint8_t>& gfx,DmaShape& d){
    const uint16_t value=r[1];
    d.width=r[6]&1023;d.height=r[7]&1023;d.bpp=(value>>12)&7;if(!d.bpp)d.bpp=8;
    d.fill=(value&15)==12;d.xstep=r[10]?r[10]:256;d.ystep=r[11]?r[11]:256;
    const uint64_t output_pixels=uint64_t((d.width*256+d.xstep-1)/d.xstep)*((d.height*256+d.ystep-1)/d.ystep);
    d.source=d.fill?0:dma_source(r[2]|(uint32_t(r[3])<<16));
    return !(value&0x80) && output_pixels<=1048576 && uint64_t(d.source)+uint64_t(d.width)*d.height*d.bpp+8<=uint64_t(gfx.size())*8;
}
// The pinned blitter: fixed-point source coordinates follow the scaled blitter; skip consumes source samples; the
// destination starts at XSTART in either mode. put(dest, pen) for every pixel written inside the destination clip.
template<class Put> void Blit(const std::array<uint16_t,18>& r,const DmaShape& d,const std::vector<uint8_t>& gfx,Put&& put,size_t stride=512){
    const uint16_t value=r[1];
    const unsigned start=(value&64)?r[0]&255:0,end=(value&64)?r[0]>>8:r[0];
    const uint16_t pal=r[8]&0x7f00,color=pal|(r[9]&255);unsigned sy=r[5]&511;
    const unsigned limit=(d.width>end?d.width-end:0)*256,first=((start*256)/d.xstep)*d.xstep;
    for(unsigned iy=0;iy<d.height*256;iy+=d.ystep){unsigned sx=r[4]&1023;
        for(unsigned ix=first;ix<limit;ix+=d.xstep){
            if(sy>=(r[12]&511) && sy<=(r[13]&511) && sx>=(r[16]&1023) && sx<=(r[17]&1023)){
                const uint32_t bit=d.source+((iy>>8)*d.width+(ix>>8))*d.bpp;
                const unsigned pixel=d.fill?0:graphics_pixel(gfx,bit,d.bpp);
                const unsigned mode=pixel?((value&8)?2:((value&2)?1:0)):((value&4)?2:((value&1)?1:0));
                if(mode)put(size_t(sy)*stride+sx,mode==2?color:uint16_t(pal|pixel));
            }sx=(sx+((value&16)?1023:1))&1023;
        }sy=(sy+((value&32)?511:1))&511;
    }
}
}
VerifiedMk5GlyphBank LoadMk5GlyphBank(const std::filesystem::path& root){
    constexpr const char* hashes[]={"b40748f4f29cb02d6689bb6ffc5e249fd25b0c124a4d78c0ca03c9cdfb80b001","ad972f4e0c2aa6153f7240a9a5aff990e42e43ff2d2f89e7e401d4ce84f65069","ff321475e3fc267b875f14d113e8f899c6a8d1c8fca09dae6fc52ba2d9a6e99f","2d08a075f2926f03d7be8b2a1636705ac54b21c4991224dd5324928169ff7c19","be6cad0830282cf85ec05a659ca363d5ed3c6769059395ba760f4e23cef206c6","0c8c2c91dbf481fbe17c5b75990ea99b54657abaf80f105a2b1eec70df99a825","3d3e6a7082a72b5b23767b203abc5265e4813dd91fbe6e558860919a7209296c","2d7387f70cf80fe6d1aaa0fdd37ee38a50dfa05c20fde4e7f7de9a884e34eb25","0023e22491559310a88542e11e6e1af6b95feeacb50944f94255ba1b9e431636","7f2694d09da188f7f715d46d11feba12adf4fa01c08fc7d1b40d5f73bbaea594","6b31c354ec0a26e844dcf891355ced6ee4b09ac3044d38ab710f4698792ad121","de8d5e4eecf5348bdc618172e16868c3a32e292579ddab21c581b0930af2a8ec"};
    std::array<std::vector<uint8_t>,12> lanes;
    for(const auto& file:std::filesystem::recursive_directory_iterator(root))if(file.is_regular_file() && file.file_size()==524288){
        std::ifstream in(file.path(),std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
        if(!in.bad() && bytes.size()==524288){auto hash=Sha256(bytes);for(unsigned i=0;i<12;++i)if(hash==hashes[i])lanes[i]=bytes;}
    }
    VerifiedMk5GlyphBank out;out.graphics.resize(0xc00000);
    for(unsigned lane=0;lane<12;++lane){if(lanes[lane].empty())throw std::runtime_error("MK5 missing authenticated graphics lane");for(size_t i=0;i<524288;++i)out.graphics[(lane/4)*0x200000+i*4+(lane%4)]=lanes[lane][i];}
    out.bytes.assign(out.graphics.begin()+0x400000,out.graphics.begin()+0x600000);
    if(Sha256(out.bytes)!=bank_hash || Sha256(out.graphics)!=graphics_hash)throw std::runtime_error("MK5 graphics bank mismatch");
    return out;
}
Mk5LiveVideo::Mk5LiveVideo(const VerifiedMk5GlyphBank& bank,ResetTrace& s,Mk5MemoryBuffers& memory,Mk5IrqController& controller,BootDiState& state,const VerifiedMk5Program* blood_program):m(memory),irq(controller),di(state),cpu(s),gfx(bank.graphics){
    if(gfx.size()!=0xc00000 || Sha256(gfx)!=graphics_hash || bank.bytes.size()!=0x200000 || Sha256(bank.bytes)!=bank_hash || !irq.IsBoundTo(s,m))throw std::runtime_error("MK5 live video binding/authentication");
    // The completed prefix has only non-triggering commands. Adopt actual IO
    // requests once; CPU/RAM/IRQ are never recreated or replayed.
    for(const auto& w:s.writes){if(w.address_bit==0x01f00000)Control(w.value);if(w.address_bit>=0x01a80000 && w.address_bit<0x01a80100 && !(w.address_bit&15)){
        unsigned offset=(w.address_bit-0x01a80000)/16,index=offset;if(offset==12 || offset==13)index=(regs[15]&32)?offset:offset+4;
        if(index==1 && (w.value&0x8000))throw std::runtime_error("MK5 video attach after historical active DMA");
        regs[index]=w.value;
    }}
    if(!blood_program)return;
    const auto word=[&](uint32_t bit){
        const size_t offset=(bit-0xff800000u)/8;
        return blood_program->bytes.at(offset)|(unsigned(blood_program->bytes.at(offset+1))<<8);
    };
    const auto dword=[&](uint32_t bit){return word(bit)|(uint32_t(word(bit+16))<<16);};
    // MK1 L5.0: third and last image of FFDE7C80, with the palette from FFDE7B40.
    blood_sprite.frame=dword(0xffde7cc0);
    blood_sprite.width=word(blood_sprite.frame);
    blood_sprite.height=word(blood_sprite.frame+16);
    const auto source=dma_source(dword(blood_sprite.frame+64));
    const auto palette=dword(0xffde7b60);
    const auto count=word(palette);
    constexpr unsigned bpp=6;
    const size_t area=size_t(blood_sprite.width)*blood_sprite.height;
    if(!area || count>=blood_sprite.colors.size() || uint64_t(source)+area*bpp+8>uint64_t(gfx.size())*8)
        throw std::runtime_error("MK5 invalid original blood frame");
    auto expand=[](unsigned v){return (v<<3)|(v>>2);};
    for(unsigned i=1;i<=count;++i){
        const auto rgb=word(palette+16*(i+1));
        blood_sprite.colors[i]=(expand((rgb>>10)&31)<<16)|(expand((rgb>>5)&31)<<8)|expand(rgb&31);
    }
    blood_sprite.pixels.resize(area);
    for(size_t i=0;i<area;++i){
        const auto pixel=graphics_pixel(gfx,source+uint32_t(i)*bpp,bpp);
        if(pixel>count)throw std::runtime_error("MK5 invalid original blood palette index");
        blood_sprite.pixels[i]=uint8_t(pixel);
    }
    blood_layer.resize(m.vram.size());
    blood_pen.resize(m.vram.size());
    blood_floor.resize(m.vram.size());
    backdrop.resize(m.vram.size());
    fighters=mk5presentation::FighterSurface(m.vram.size());
    fighters.ClearWith(0,m.vram.size(),[this](size_t pixel){return uint16_t(m.vram[pixel]&0x7fff);});
    for(auto address:{0x01051230u,0x01051510u})
        FighterRamWrite(0,address,ram_pointer(m,address));
}
bool Mk5LiveVideo::IsBackdropMode()const{
    const auto mode=m.ram[(0x01051190-0x01000000)/16];
    return mode==1 || mode==2 || mode==3 || mode==4 || mode==5 || mode==6 ||
           mode==7 || mode==9 || mode==11 || mode==13;
}
uint32_t Mk5LiveVideo::ObjectDrawReturn(uint32_t pc)const{
    if(!BackdropEnabled() || pc!=0xffc79b40)return 0;
    const auto sp=cpu.a[15];
    if(sp<0x01000000 || sp>0x013fffe0 || (sp&15))return 0;
    const auto slot=(sp-0x01000000)/16;
    return m.ram[slot]|(uint32_t(m.ram[slot+1])<<16);
}
bool Mk5LiveVideo::IsBackdropDma(uint32_t caller)const{
    if(!caller || !IsBackdropMode())return false;
    // L5.0 draws the parallax lists before shadows and the front scenery list after fighters.
    if(caller==0xffc7a1a0 || caller==0xffc7a2a0 || caller==0xffc7a800)return true;
    const auto mode=m.ram[(0x01051190-0x01000000)/16];
    // Selection and Battle Plan use the world-object list for their portraits.
    return caller==0xffc7a720 && (mode==4 || mode==7 || mode==13);
}
uint64_t Mk5LiveVideo::FighterOwner(uint32_t caller)const{
    const auto object=cpu.a[0];
    if(caller!=0xffc7a720 || !actor_slot(object))return 0;
    if(object==held_prop){
        // This allocation starts as a blood streak before becoming the held prop.
        const auto frame=ram_pointer(m,object+0x180);
        return frame==0xfffa6280 || frame==0xfffa62e0 || frame==0xfffa6340?
            fighter_objects.Generation(object):0;
    }
    // FFC82D40 walks the same multipart chain when creating each component's shadow.
    for(const auto& [root,generation]:fighter_objects.Live()){
        if(root==held_prop)continue;
        auto component=root;
        for(unsigned n=0;n<0x166 && actor_slot(component);++n){
            if(component==object)return generation;
            component=ram_pointer(m,component+0x20);
        }
    }
    return 0;
}
void Mk5LiveVideo::FighterRamWrite(uint32_t pc,uint32_t address,uint32_t value){
    if(!fighters.enabled())return;
    if(address==0x01051230 || address==0x01051510){
        BindFighter(value);
    }else if(address==0x01008000){
        switch(pc){
        case 0xffc7f230:case 0xffc897a0:
            fighter_objects.Clear();held_prop=0;return;
        case 0xffc7f360:value=cpu.a[0];break;
        case 0xffc7feb0:case 0xffc8ae40:case 0xffc8b260:case 0xffce4730:break;
        default:return;
        }
        fighter_objects.Forget(value);
        if(value==held_prop)held_prop=0;
    }
}
void Mk5LiveVideo::BindFighter(uint32_t object){
    if(fighters.enabled() && actor_slot(object))fighter_objects.Bind(object);
}
void Mk5LiveVideo::BindHeldProp(uint32_t object){
    if(fighters.enabled() && actor_slot(object)){
        fighter_objects.Bind(object);
        held_prop=object;
    }
}
void Mk5LiveVideo::ObserveBlood(uint32_t pc,const ResetTrace& s){
    if(blood_layer.empty())return;
    const auto word=[this](uint32_t bit){return m.ram[(bit-0x01000000)/16];};
    // The round setup increments this counter. Only a new bout enters at zero.
    // The hidden fight enters round one directly in the same arena.
    if((pc==0xffc9f010 && word(0x010519d0)==0) || pc==0xffca3d00){
        blood_stains.clear();
        std::fill(blood_layer.begin(),blood_layer.end(),0);
        std::fill(blood_pen.begin(),blood_pen.end(),0);
        std::fill(blood_floor.begin(),blood_floor.end(),0);
        std::fill(wide.blood.begin(),wide.blood.end(),uint8_t(0));
        return;
    }
    const auto mode=word(0x01051190);
    if(pc==0xffc7a700){
        DrawBlood(s,mode==2 || mode==3 || mode==5 || mode==6);
        return;
    }
    if(pc!=0xffde7ad0)return;
    if((mode!=2 && mode!=3 && mode!=5) || word(0x010528a0))return;
    const auto object=s.a[8];
    if(object<0x01000000 || object>=0x013ffe70 || (object&15))return;
    // Keep the completed native splash when its object is about to be removed.
    const auto frame=word(object+0x180)|(uint32_t(word(object+0x190))<<16);
    if(frame!=blood_sprite.frame)return;
    const bool flip=(word(object+0xe0)&0x10)!=0;
    const auto area=blood_sprite.pixels.size();
    blood_stains.try_emplace({int16_t(word(object+0x90)),int16_t(word(object+0xb0)),flip},
                             BloodStain{std::vector<uint16_t>(area),std::vector<uint8_t>(area)});
}
// The program's palette fades dim the stains in the scanout (mk5_blood_shade.hpp): each splash pixel keeps the pen of
// the word it first covers here and that pen's brightness through the palette of this draw.
void Mk5LiveVideo::DrawBlood(const ResetTrace& s,bool arena_visible){
    if(blood_stains.empty())return;
    const auto page_x=uint16_t(s.a[5]),page_y=uint16_t(s.a[5]>>16);
    if(page_y!=0 && page_y!=256)return;
    std::fill_n(blood_layer.begin()+page_y*512,256*512,0);
    // Widescreen: the stains over the wide window too (screen x -67+d .. 467+d, layer column page_x+x+11-d), into the wide
    // blood layer; the stains' own state (the pen they first cover) stays the arcade's.
    const bool wide_blood=wide.Enabled() && !wide.blood.empty();
    if(wide_blood)std::fill_n(wide.blood.begin()+page_y*mk5wide::kLayerWidth,256*mk5wide::kLayerWidth,uint8_t(0));
    if(!arena_visible)return;
    const int camera_x=int16_t(s.a[4]),camera_y=int16_t(s.a[4]>>16);
    const int left=uint16_t(s.a[14]),top=uint16_t(s.a[14]>>16);
    const int right=uint16_t(s.a[13]),bottom=uint16_t(s.a[13]>>16);
    // Background DMA has finished issuing. Foreground writes mask the layer as they draw.
    for(auto& [pose,stain]:blood_stains){
        const auto [origin_x,origin_y,flip]=pose;
        for(int dy=0;dy<blood_sprite.height;++dy){
            const int y=origin_y-camera_y+dy;
            if(y<top || y>=bottom || y>=256)continue;
            for(int dx=0;dx<blood_sprite.width;++dx){
                const int x=origin_x-camera_x+dx;
                const int source_x=flip?blood_sprite.width-1-dx:dx;
                const size_t k=size_t(dy*blood_sprite.width+source_x);
                const auto shade=blood_sprite.pixels[k];
                if(!shade)continue;
                if(wide_blood){
                    const int column=int(page_x)+x+mk5wide::kExtra-wide.shift;
                    if(column>=0 && column<mk5wide::kLayerWidth)wide.blood[size_t(page_y+y)*mk5wide::kLayerWidth+size_t(column)]=shade;
                }
                if(x<left || x>=right || x+page_x>=512)continue;
                const size_t i=size_t(page_y+y)*512+size_t(page_x+x);
                if(!stain.pen[k]){
                    stain.pen[k]=uint16_t(m.vram[i]|mk5blood::kTaken);
                    stain.floor[k]=uint8_t(mk5blood::Sum(m.palette[m.vram[i]&0x7fff]));
                }
                blood_layer[i]=shade;
                blood_pen[i]=stain.pen[k];
                blood_floor[i]=stain.floor[k];
            }
        }
    }
}
void Mk5LiveVideo::Update(){if(pending && irq.elapsed_quanta()*10>=due){pending=false;regs[1]&=0x7fff;irq.SetExternalLine(0,true);completions.push_back(due);}}
uint16_t Mk5LiveVideo::Read(unsigned offset)const{return regs.at(offset==0?1:offset);}
void Mk5LiveVideo::Write(uint32_t pc,unsigned offset,uint16_t value){
    unsigned index=offset;if(offset==12 || offset==13)index=(regs[15]&32)?offset:offset+4;
    if(index!=1 || !(value&0x8000)){regs.at(index)=value;if(index==1)irq.SetExternalLine(0,false);return;}
    auto r=regs;r[1]=value;
    DmaShape d;
    if(!Shape(r,gfx,d) || pending)throw std::runtime_error("MK5 DMA unsupported mode/source/overlap frontier");
    regs=r;irq.SetExternalLine(0,false);uint64_t now=irq.elapsed_quanta()*10;due=now+41ULL*((d.width*256)/d.xstep)*((d.height*256)/d.ystep);pending=true;commands.push_back({pc,now,due,r});
    const auto caller=ObjectDrawReturn(pc);
    const auto owner=FighterOwner(caller);
    const bool scenery=!owner && IsBackdropDma(caller);
    auto draw=fighters.Draw(owner);
    // Widescreen: in the real walk, the staged draws up to this object come first and this draw is mirrored inside its
    // arcade columns only; after a failed shadow pass the walk is mirrored whole. Any other draw is mirrored where it lands.
    const WideEntry* staged=wide.Enabled() && wide_walk && wide_walk_drawn?WideMerge(cpu.a[0]):nullptr;
    const bool merge=wide.Enabled() && wide_walk && wide_walk_drawn,world=wide_walk && !(staged && staged->fixed);
    const bool mirror=wide.Enabled() && !(merge && world && wide.whole);   // offset window: the world is the staged draws
    const uint8_t actor=mk5wide::Layer<uint8_t>::ActorDraw(r[1])?mk5wide::Layer<uint8_t>::kActor:0;   // a floor shadow
    if(mirror){const auto [lo,hi]=mk5wide::Span(r[4],r[6],r[10],(r[1]&16)!=0);
        wide.Draw(mk5wide::Kind::real,lo+mk5wide::kExtra,hi+mk5wide::kExtra,staged?staged->layer:-1);}
    Blit(r,d,gfx,[&](size_t dest,uint16_t pen){
        m.vram.at(dest)=pen;
        fighters.Write(draw,dest,m.vram[dest]|(scenery?backdrop_owner:0),owner?BloodPixel(dest):0);
        if(!blood_layer.empty())blood_layer[dest]=0;
        if(!backdrop.empty()){
            if(scenery)backdrop[dest]=m.vram[dest]|backdrop_owner;
            else backdrop[dest]&=~backdrop_owner;
        }
        ++pixels_written;
        if(mirror){
            const int column=int(dest&511);const auto at=wide.At(dest);
            if(at>=0 && (!merge || (column>=mk5wide::kMargin && column<mk5wide::kMargin+mk5wide::kVisible))){
                const uint8_t under=owner?uint8_t(wide.fixed[size_t(at)]&mk5wide::Layer<uint8_t>::kActor):0;   // a shadow under a fighter
                wide.Write(size_t(at),pen,!world);wide.fixed[size_t(at)]|=uint8_t(actor|under|(owner?mk5wide::Layer<uint8_t>::kFighter:0));}
        }
    });
}
// The shadow pass's DMA registers: a command is staged for the wide layer, with no timing, no interrupt and no effect on
// VRAM or the Remastered layers. False when the board's DMA would refuse the command.
bool Mk5LiveVideo::ShadowWrite(unsigned offset,uint16_t value){
    unsigned index=offset;if(offset==12 || offset==13)index=(shadow_regs[15]&32)?offset:offset+4;
    if(index>=shadow_regs.size())return false;
    if(index!=1 || !(value&0x8000)){shadow_regs[index]=value;return true;}
    auto r=shadow_regs;r[1]=value;shadow_regs=r;
    DmaShape d;if(!Shape(r,gfx,d))return false;
    // A slot the pass booked for another palette: its colours come from the ROM (mk5wide::Layer::Book); left out when the
    // slot holds no ROM palette.
    if(const unsigned slot=(r[8]>>8)&0x7f;slot<mk5widescreen::kPaletteSlots && !wide.booked[slot]){
        const uint32_t pointer=ram_pointer(m,mk5widescreen::kPaletteTable+slot*mk5widescreen::kPaletteEntry);
        if(pointer!=wide_palettes[slot]){
            if(!wide_rom || pointer<0xff800000u || size_t(pointer-0xff800000u)/8+2*257>wide_rom->size())return true;
            wide.Book(slot,pointer,[&](uint32_t bit){const size_t at=(bit-0xff800000u)/8;return uint16_t((*wide_rom)[at]|(*wide_rom)[at+1]<<8);});
        }
    }
    const WideEntry e{r,cpu.a[0],wide_hud,wide_hud?-1:wide_layer};
    if(wide_hud){WideStage(e);WideRing(e,wide_staged);}else{wide_plane.push_back(e);WideRing(e,wide_plane);}
    return true;
}
// A part of a group the game wraps around a ring (WideWrap): its copies one ring length away on each side, where they
// reach the layer, so no part pops up or vanishes in the margins. A copy belongs to no object: the real walk never draws
// it. The groups keep their own screen place (the drawer takes them for screen-fixed objects, as it does the HUD), so
// their copies are staged like them: in the margins only.
void Mk5LiveVideo::WideRing(const WideEntry& e,std::vector<WideEntry>& to){
    const auto part=std::find_if(wide_wrap.begin(),wide_wrap.end(),[&](const WideWrapPart& w){return w.object==e.object;});
    if(part==wide_wrap.end())return;
    const auto [lo,hi]=mk5wide::Span(e.regs[4],e.regs[6],e.regs[10],(e.regs[1]&16)!=0);
    int x=e.regs[4]&0x3ff;if(x>=768)x-=1024;
    for(const int move:{-part->length,part->length}){
        const int at=x+move;
        if(at<-256 || at>=768 || hi+move+mk5wide::kExtra<=0 || lo+move+mk5wide::kExtra>=mk5wide::kLayerWidth)continue;
        auto copy=e;copy.object=0;copy.regs[4]=uint16_t((e.regs[4]&~0x3ffu)|(unsigned(at)&0x3ffu));to.push_back(copy);++wide_ring;
    }
}
void Mk5LiveVideo::WideStage(const WideEntry& e){wide_staged.push_back(e);}
void Mk5LiveVideo::WidePalettes(std::array<uint32_t,64>& to)const{
    for(unsigned slot=0;slot<mk5widescreen::kPaletteSlots;++slot)to[slot]=ram_pointer(m,mk5widescreen::kPaletteTable+slot*mk5widescreen::kPaletteEntry);
}
// The game's fade right now, of 256: per palette slot, its colours' light in the palette RAM against the ROM palette the
// slot holds; the median slot (the HUD's palettes do not fade with the scene). 256 when nothing fades.
unsigned Mk5LiveVideo::WideDim()const{
    if(!wide_rom)return 256;
    const auto rom=[&](uint32_t bit){const size_t at=(bit-0xff800000u)/8;return unsigned((*wide_rom)[at]|(*wide_rom)[at+1]<<8);};
    const auto light=[](unsigned c){return (c>>10&31)+(c>>5&31)+(c&31);};
    std::vector<unsigned> slots;
    for(unsigned slot=0;slot<mk5widescreen::kPaletteSlots;++slot){
        const uint32_t pointer=ram_pointer(m,mk5widescreen::kPaletteTable+slot*mk5widescreen::kPaletteEntry);
        if(pointer<0xff800000u || size_t(pointer-0xff800000u)/8+2*257>wide_rom->size())continue;
        unsigned was=0,now=0;
        for(unsigned k=0,count=std::min(rom(pointer),256u);k<count;++k){was+=light(rom(pointer+16+16*k));now+=light(m.palette[slot*256+k]);}
        if(was)slots.push_back(std::min(256u,now*256/was));
    }
    if(slots.empty())return 256;
    std::nth_element(slots.begin(),slots.begin()+slots.size()/2,slots.end());
    return slots[slots.size()/2];
}
// The real walk draws `object`: the staged draws before its own (and its own) reach the layer; returns its own, if staged.
const Mk5LiveVideo::WideEntry* Mk5LiveVideo::WideMerge(uint32_t object){
    for(size_t i=wide_cursor;i<wide_staged.size();++i)if(wide_staged[i].object==object){
        WideFlush(i);WideOutside(wide_staged[i]);wide_cursor=i+1;return &wide_staged[i];}
    return nullptr;
}
// Staged draws the real walk did not make: the arcade's clip dropped them, or the arcade has not built the object yet (its
// scenery streamer fills the 4:3 window every few frames: a block entering the screen while it scrolls shows late, and
// the arcade's edge columns hold the page's background meanwhile). Drawn whole: the wide picture already has the object
// beside those columns.
void Mk5LiveVideo::WideFlush(size_t end){
    for(;wide_cursor<end && wide_cursor<wide_staged.size();++wide_cursor)WideOutside(wide_staged[wide_cursor],true);
}
// A staged draw on the layer, rendered in layer columns at the x the game gave it, outside the arcade's columns (the real
// draws own those). A screen-fixed draw (the HUD) still marks its arcade pixels fixed. With a window offset a world draw
// is rendered whole (the real draws stand elsewhere), and so is one the real walk did not make (unmade).
void Mk5LiveVideo::WideOutside(const WideEntry& e,bool unmade){
    auto r=e.regs;
    {const auto [a,b]=mk5wide::Span(r[4],r[6],r[10],(r[1]&16)!=0);   // the draw as the game meant it
        wide.Draw(mk5wide::Kind::shadow,a+mk5wide::kExtra,b+mk5wide::kExtra,e.layer);}
    const auto [lo,hi]=mk5wide::Span(r[4],r[6],r[10],(r[1]&16)!=0);
    r[4]=mk5wide::LayerX(r[4]);
    DmaShape d;if(!Shape(r,gfx,d))return;
    constexpr int first=mk5wide::kWideMargin,last=first+mk5wide::kVisible;
    const bool actor=mk5wide::Layer<uint8_t>::ActorDraw(r[1]);
    const auto put=[&](size_t dest,uint16_t pen){
        if(dest>=wide.pens.size())return;
        wide.Write(dest,pen,e.fixed);if(actor)wide.fixed[dest]|=mk5wide::Layer<uint8_t>::kActor;};
    if((wide.whole || unmade) && !e.fixed){r[16]=0;r[17]=mk5wide::kLayerWidth-1;Blit(r,d,gfx,put,mk5wide::kLayerWidth);return;}
    if(lo+mk5wide::kExtra<first){r[16]=0;r[17]=first-1;Blit(r,d,gfx,put,mk5wide::kLayerWidth);}
    if(hi+mk5wide::kExtra>last){r[16]=last;r[17]=mk5wide::kLayerWidth-1;Blit(r,d,gfx,put,mk5wide::kLayerWidth);}
    if(e.fixed && hi+mk5wide::kExtra>first && lo+mk5wide::kExtra<last){   // its arcade pixels are the HUD's
        r[16]=first;r[17]=last-1;Blit(r,d,gfx,[&](size_t dest,uint16_t){if(dest<wide.fixed.size())wide.fixed[dest]|=mk5wide::Layer<uint8_t>::kFixed;},mk5wide::kLayerWidth);
    }
}
// The frame ends at the display interrupt: the margins without a scenery layer's art are masked.
void Mk5LiveVideo::WideEndUnbuilt(){wide.EndFrame();}
// The waiting list (a scenery layer): its art measured over the layer's columns (mk5wide::MeasureBand), its draws staged.
bool Mk5LiveVideo::WidePlane(){
    if(wide_plane.empty())return true;
    // Measured over the shadow's whole clip (the layer and kReach columns on each side): a far plane's art ends are known
    // before they reach the wide screen (mk5wide::Layer::Learn).
    constexpr int kOrigin=mk5wide::kReach,kCanvas=mk5wide::kLayerWidth+2*mk5wide::kReach;
    std::vector<std::array<uint16_t,18>> draws;std::vector<std::pair<int,int>> meant;
    for(const auto& e:wide_plane){
        auto r=e.regs;const auto [a,b]=mk5wide::Span(r[4],r[6],r[10],(r[1]&16)!=0);meant.push_back({a+kOrigin+mk5wide::kExtra,b+kOrigin+mk5wide::kExtra});
        r[4]=uint16_t((r[4]+kOrigin+mk5wide::kExtra)&0x3ff);r[16]=0;r[17]=kCanvas-1;
        DmaShape d;if(!Shape(r,gfx,d)){wide_plane.clear();return false;}
        draws.push_back(r);
        if(wide.debug)wide.notes.push_back({uint16_t(e.layer),uint32_t(r[2]|uint32_t(r[3])<<16),uint16_t(r[5]&0x1ff),int16_t(a+mk5wide::kExtra),int16_t(b+mk5wide::kExtra),wide_camera});
    }
    auto band=mk5wide::MeasureBand(uint16_t(wide_plane.front().layer),draws,meant,[&](const std::array<uint16_t,18>& r,auto&& put){
        DmaShape d;if(!Shape(r,gfx,d))return false;
        Blit(r,d,gfx,[&](size_t dest,uint16_t){put(dest);},kCanvas);return true;},[](size_t,int,int){},kCanvas,kOrigin,&wide.shapes,wide.debug);
    wide.Measured(wide_slot,wide_camera,std::move(band));
    for(const auto& e:wide_plane)WideStage(e);
    wide_plane.clear();return true;
}
uint32_t Mk5LiveVideo::CpuRead(uint32_t bit,unsigned width)const{
    if(width && width<=32 && bit>=0x02000000 && uint64_t(bit)+width<=0x08000000ULL){
        uint32_t value=0;
        for(unsigned b=0;b<width;++b){const uint32_t relative=bit+b-0x02000000u;const size_t byte=(((relative>>25)&1u)?0x400000u:0u)+((relative&0x01ffffffu)>>3);value|=uint32_t((gfx.at(byte)>>(relative&7u))&1u)<<b;}
        return value;
    }
    if(!width || width>32 || uint64_t(bit)+width>0x400000)throw std::runtime_error("MK5 CPU VRAM read field frontier");
    uint32_t value=0;for(unsigned b=0;b<width;++b){auto pixel=m.vram.at((bit+b)/8);unsigned byte=data_bank?pixel&255:pixel>>8;value|=((byte>>((bit+b)&7))&1)<<b;}return value;
}
void Mk5LiveVideo::CpuWrite(uint32_t bit,unsigned width,uint32_t value,bool draw){
    if((bit&7) || (width!=8 && width!=16 && width!=32) || uint64_t(bit)+width>0x400000)throw std::runtime_error("MK5 CPU VRAM field frontier");
    for(unsigned b=0;b<width;b+=8){
        unsigned shift=((bit+b)&8)?8:0;
        const auto index=(bit+b)/8;
        auto& dest=m.vram.at(index);dest=data_bank?uint16_t(((value>>b)&255)|(((regs[8]>>shift)&255)<<8)):uint16_t((dest&255)|(((value>>b)&255)<<8));
        if(!blood_layer.empty())blood_layer[index]=0;
        if(!backdrop.empty())backdrop[index]&=~backdrop_owner;
        if(draw)wide.Mirror(m.vram,index,1);
    }
    const auto first=bit/8;
    fighters.ClearWith(first,width/8,[this,first](size_t x){return uint16_t(m.vram[first+x]&0x7fff);});
}
uint32_t Mk5LiveVideo::ShiftLoad(uint32_t bit){if(shadow_active)throw std::runtime_error("MK5 shift register load in the widescreen shadow pass");size_t index=bit>>3;if(index+1024>m.vram.size())throw std::runtime_error("MK5 shiftreg range");std::copy_n(m.vram.begin()+index,1024,di.shiftreg.begin());++shift_loads;return di.shiftreg[0];}
uint32_t Mk5LiveVideo::FillXY(ResetTrace& s){
    if(shadow_active)throw std::runtime_error("MK5 FILL XY in the widescreen shadow pass");
    const auto& io=irq.io();
    uint32_t base=s.b[4]&~7u; // Pinned FILL aligns daddr to PSIZE8; B4 stays live.
    // Whole-instruction budget, DINT path. No host CPU-cycle slice or scanout.
    if(!(io[8]&0x800) || (io[11]!=0 && io[11]!=0x2c) || io[20]!=0x12 || io[21]!=8 || s.b[2]!=0 || s.b[3]!=0x2000 || s.b[7]!=0x007f0001 || (base!=0 && base!=0x100000) || (s.status&0x02200000)!=0)
        throw std::runtime_error("MK5 FILL XY unsupported live graphics state");
    const bool scenery=!backdrop.empty() && IsBackdropMode();
    s.status|=0x02000000;for(unsigned y=0;y<127;++y){
        size_t index=(base+y*s.b[3])>>3;
        std::copy(di.shiftreg.begin(),di.shiftreg.end(),m.vram.begin()+index);
        wide.Mirror(m.vram,index,di.shiftreg.size());wide.MarkCleared(index);
        if(!blood_layer.empty())std::fill_n(blood_layer.begin()+index,di.shiftreg.size(),0);
        if(!backdrop.empty()){
            std::transform(di.shiftreg.begin(),di.shiftreg.end(),backdrop.begin()+index,
                           [scenery](uint16_t pixel){
                               // The original page clear uses the live backdrop pen, 0x003f.
                               const auto pen=uint16_t(pixel&~backdrop_owner);
                               return uint16_t(pen|(scenery && pen==0x003f?backdrop_owner:0));
                           });
            fighters.ClearWith(index,di.shiftreg.size(),[this,index](size_t x){return backdrop[index+x];});
        }
        ++shift_stores;
    }
    s.status&=~uint32_t(0x02000000);s.b[2]+=0x007f0000;return (io[11]&0x20)?516:262;
}
