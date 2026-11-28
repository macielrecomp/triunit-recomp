#pragma once
#include "mk2_board.hpp"
#include "mk2_program.hpp"
#include "mk2_widescreen.hpp"
#include "../include/mk5_fighter_video.hpp"
#include "../include/mk5_blood_shade.hpp"
#include <algorithm>
#include <optional>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace mk2 {
[[gnu::always_inline]] inline uint32_t ExpandRgb555(uint16_t color) {   // each 5-bit channel v as (v<<3)|(v>>2); per pixel, so always inline
    const unsigned r=(color>>10)&31,g=(color>>5)&31,b=color&31;
    return 0xff000000|(((r<<3)|(r>>2))<<16)|(((g<<3)|(g>>2))<<8)|((b<<3)|(b>>2));
}
struct BootVideo {
    std::vector<uint16_t> palette_ram = std::vector<uint16_t>(32768);
    std::vector<uint16_t> vram = std::vector<uint16_t>(524288);
    std::array<uint16_t, 4096> shift_register{};
    uint16_t dma_palette = 0;
    std::array<uint16_t, 18> dma_registers{};
    static constexpr uint16_t backdrop_owner=0x8000;
    void EnableBackdrop(){backdrop.resize(vram.size());}
    void EnableFighters(){fighters_enabled=true;}
    bool FightersEnabled()const{return fighters_enabled;}
    const mk5presentation::FighterSurface& FighterVideo()const{return fighters;}
    uint64_t FighterGeneration(uint32_t object)const{return fighter_objects.Generation(object);}
    void BindFighter(uint32_t object){
        if(fighters_enabled && object>=0x0102d6f0 && object<0x0105b950 &&
           (object-0x0102d6f0)%0x210==0)fighter_objects.Bind(object);
    }
    void ForgetFighter(uint32_t object){fighter_objects.Forget(object);}
    void ResetFighters(){fighter_objects.Clear();}
    bool BackdropEnabled()const{return !backdrop.empty();}
    uint16_t BackdropPixel(size_t index)const{return backdrop[index];}
    bool backdrop_list=false,world_object_list=false;
    struct DrawOwner {bool scenery=false;uint64_t fighter=0;
        template<class A> friend void Fields(A& a,DrawOwner& o){a(o.scenery,o.fighter);}};
    void StageDraw(uint32_t cursor,DrawOwner owner){
        if(owner.scenery || owner.fighter)draw_queue[cursor]=owner;
        else draw_queue.erase(cursor);
    }
    DrawOwner TakeDraw(uint32_t cursor){
        auto node=draw_queue.extract(cursor);
        return node.empty()?DrawOwner{}:node.mapped();
    }
    void ResetDrawQueue(){draw_queue.clear();}
    void CopyBackdrop(size_t index,size_t count,bool scenery){
        fighters.ClearWith(index,count,[this,index,scenery](size_t x){
            return uint16_t((vram[index+x]&0x7fff)|(scenery?backdrop_owner:0));
        });
        if(!BackdropEnabled())return;
        std::transform(vram.begin()+index,vram.begin()+index+count,backdrop.begin()+index,
            [scenery](uint16_t pen){return uint16_t((pen&0x7fff)|(scenery?backdrop_owner:0));});
    }
    void EnableBlood(const Program& program) {
        const auto word=[&](uint32_t bit) {
            const size_t index=(bit-0xff800000u)/8;
            return uint16_t(program.bytes.at(index)|(unsigned(program.bytes.at(index+1))<<8));
        };
        constexpr uint32_t frame=0xffbecd10;
        const auto width=word(frame), height=word(frame+16);
        if(width!=35 || height!=11)throw std::runtime_error("MK2 invalid original floor splash");
        // Use the board's compressed-row DMA with the final frame's exact command and source.
        BootVideo decoded;
        BoardBootControls board;
        auto& r=decoded.dma_registers;
        r[2]=word(frame+64); r[3]=word(frame+80);
        r[6]=width; r[7]=height; r[13]=height-1; r[17]=width-1;
        decoded.StartDma(word(frame+96)|0x8002,board,program.graphics);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto pixel=decoded.vram[y*512+x];
            if(pixel>=BloodPalette{}.size())throw std::runtime_error("MK2 invalid floor splash palette index");
            blood_pixels[y*width+x]=uint8_t(pixel);
        }
        blood_layer.resize(vram.size());
        blood_pen.resize(vram.size());
        blood_floor.resize(vram.size());
    }
    bool BloodEnabled()const { return !blood_layer.empty(); }
    void ClearBlood() {
        blood_stains.clear();
        std::fill(blood_layer.begin(),blood_layer.end(),0);
        std::fill(blood_pen.begin(),blood_pen.end(),0);
        std::fill(blood_floor.begin(),blood_floor.end(),0);
        std::fill(wide.blood.begin(),wide.blood.end(),0);
        blood_draw.cursor=0;
    }
    void KeepBlood(int16_t x,int16_t y,bool flip,uint16_t palette) {
        if(!BloodEnabled())return;
        // The next object can reuse this slot as soon as the original removes the splash.
        auto& colors=blood_stains[{x,y,flip}].colors;
        std::copy_n(palette_ram.begin()+(palette&0x7f00),colors.size(),colors.begin());
    }
    // The palette fades of the program dim the stains in the scanout (mk5_blood_shade.hpp): each stain pixel keeps the
    // pen of the word it first covers here and that pen's brightness through the palette of this draw.
    void DrawBlood(uint32_t page,uint32_t camera,uint32_t clip_start,uint32_t clip_end,bool visible=true) {
        if(blood_stains.empty())return;
        const unsigned page_x=uint16_t(page), page_y=page>>16;
        if(page_y!=0 && page_y!=256)return;
        std::fill_n(blood_layer.begin()+page_y*512,256*512,0);
        // Widescreen: the stains over the wide window too (screen x -67+d .. 467+d, layer column page_x+x+11-d), into the
        // wide blood layer; the stains' own state (the pen they first cover) stays the arcade's.
        const bool wide_on=wide.Enabled();
        if(wide_on){wide.blood.resize(mk5wide::kLayerSize);std::fill_n(wide.blood.begin()+page_y*mk5wide::kLayerWidth,256*mk5wide::kLayerWidth,0);}
        if(!visible)return;
        const int camera_x=int16_t(camera),camera_y=int16_t(camera>>16);
        const int left=uint16_t(clip_start),top=clip_start>>16;
        const int right=uint16_t(clip_end),bottom=clip_end>>16;
        for(auto& [pose,stain]:blood_stains) {
            const auto [origin_x,origin_y,flip]=pose;
            for(int dy=0;dy<11;++dy) {
                const int y=origin_y-camera_y+dy;
                if(y<top || y>=bottom || y>=256)continue;
                for(int dx=0;dx<35;++dx) {
                    const int x=origin_x-camera_x+dx;
                    const size_t k=size_t(dy*35+(flip?34-dx:dx));
                    const auto shade=blood_pixels[k];
                    if(!shade)continue;
                    if(wide_on){
                        const int column=int(page_x)+x+mk5wide::kExtra-wide.page_shift[page_y?1:0];
                        if(column>=0 && column<mk5wide::kLayerWidth)
                            wide.blood[(page_y+y)*mk5wide::kLayerWidth+size_t(column)]=uint16_t(stain.colors[shade]|0x8000);
                    }
                    if(x<left || x>=right || x+int(page_x)>=512)continue;
                    const size_t i=(page_y+y)*512+page_x+x;
                    if(!stain.pen[k]){
                        stain.pen[k]=uint16_t(vram[i]|mk5blood::kTaken);
                        stain.floor[k]=uint8_t(mk5blood::Sum(palette_ram[vram[i]&0x7fff]));
                    }
                    blood_layer[i]=stain.colors[shade]|0x8000;
                    blood_pen[i]=stain.pen[k];
                    blood_floor[i]=stain.floor[k];
                }
            }
        }
    }
    uint32_t BloodPixel(size_t index)const {
        const auto pixel=BloodEnabled()?blood_layer[index]:0;
        return pixel?ExpandRgb555(uint16_t(pixel)):0;
    }
    // The floor of the stain word at `index` (meaningful where BloodPixel is not 0): pen | kTaken and its brightness then.
    uint16_t BloodPen(size_t index)const { return blood_pen.empty()?0:blood_pen[index]; }
    uint8_t BloodFloor(size_t index)const { return blood_floor.empty()?0:blood_floor[index]; }
    void MaskBlood(size_t index,size_t count=1) {
        if(BloodEnabled())std::fill_n(blood_layer.begin()+index,count,0);
    }
    void QueueBloodDraw(uint32_t cursor,uint32_t page,uint32_t camera,
                        uint32_t clip_start,uint32_t clip_end,bool visible) {
        if(!blood_stains.empty())blood_draw={cursor,page,camera,clip_start,clip_end,visible};
    }
    void DrawQueuedBlood(uint32_t cursor) {
        if(!blood_draw.cursor || blood_draw.cursor!=cursor)return;
        DrawBlood(blood_draw.page,blood_draw.camera,blood_draw.clip_start,blood_draw.clip_end,blood_draw.visible);
        blood_draw.cursor=0;
    }
    void WriteDma(unsigned offset, uint16_t data, BoardBootControls& board, uint16_t mask = 0xffff) {
        if (offset == 1) board.WriteDmaCommand(data, mask);
        else if (offset == 8)
            dma_palette = uint16_t((dma_palette & uint16_t(~mask)) | (data & mask));
        else if (offset == 12 || offset == 13 || offset == 15) {
            const unsigned physical = offset != 15 && !(dma_registers[15] & 0x20) ? offset + 4 : offset;
            auto& word = dma_registers[physical];
            word = uint16_t((word & uint16_t(~mask)) | (data & mask));
        }
        else if (offset <= 11 || offset == 14) {
            auto& word = dma_registers[offset];
            word = uint16_t((word & uint16_t(~mask)) | (data & mask));
        } else throw std::runtime_error("MK2 DMA register outside qualified video initialization");
    }
    uint16_t ReadDma(unsigned offset, const BoardBootControls& board) const {
        if (offset == 0 || offset == 1) return board.dma_command;
        if (offset == 8) return dma_palette;
        if (offset <= 15) return dma_registers[offset];
        throw std::runtime_error("MK2 DMA register outside qualified video initialization");
    }
    // Measured commands plus the pinned finite unscaled sets below, as a table built once.
    static bool KnownCommand(uint16_t command){
        static const std::vector<bool> table=[]{
            std::vector<bool> known(65536);
            known[0x8423]=true;   // the opaque copy
            for(auto value : {0x9008,0xe002,0xe482,0xef82,0xe082,0xe282,0xd782,0xd182,0xd382,
                              0xec82,0xe882,0xee92,0xe192,0xe182,0xeb92,0xeb82,0xe012,0xe292,0xee82,0xc382,0xe092,
                              0xb002,0xc002,0xc012,0xe582,0xf002,0x900c,0xe982,0xf00c,
                              0xa00c,0xb082,0xb182,0xb582,0xd002,0xd082,0xd192,0xd282,0xd582,0xd682,0xd882,0xe098,0xe198,0xe382,0xe488,0xe492,0xe498,0xe588,0xe592,0xe598,0xe682,0xe692,0xe698,0xe782,0xe892,0xea82,0xea92,0xea98,0xed92,0xed98,
                              0xe088,0xe188,0xe792,0xd692,0xe008,0xe898,0xe992,0xe998,0xd592,0xe5b2,0xe888,0xa002,0xc082,0xc092,0xc192,0xc482,0xc582,0xc592,0xc692,0xd092,0xed82,0xe018,0xe988,0xe688,0xea88,0xc182,0xc492,0xca92,0xd012,0xd492})
                known[value]=true;
            // Pinned unscaled 1..8bpp skip/copy: all16 pre/post pairs and both X directions tested.
            for(unsigned base : {0x8082u,0x9082u,0xa082u,0xb082u,0xc082u,0xd082u,0xe082u,0xf082u})
                for(unsigned pair=0;pair<16;++pair)
                    for(unsigned flip : {0u,0x10u})
                        known[base|(pair<<8)|flip]=true;
            // Pinned uncompressed copy: both X directions and start/end skip encodings.
            for(unsigned base : {0x8002u,0x9002u,0xa002u,0xb002u,0xc002u,0xd002u,0xe002u,0xf002u})
                for(unsigned flags : {0u,0x10u,0x40u,0x50u})
                    known[base|flags]=true;
            // Pinned 1..8bpp Y flip, all row-header pairs, both X directions and skip encodings.
            for(unsigned base : {0x80a2u,0x90a2u,0xa0a2u,0xb0a2u,0xc0a2u,0xd0a2u,0xe0a2u,0xf0a2u})
                for(unsigned pair=0;pair<16;++pair)
                    for(unsigned flip : {0u,0x10u})
                        known[base|(pair<<8)|flip]=true;
            for(unsigned base : {0x8002u,0x9002u,0xa002u,0xb002u,0xc002u,0xd002u,0xe002u,0xf002u})
                for(unsigned flags : {0x20u,0x30u,0x60u,0x70u})
                    known[base|flags]=true;
            // Pinned 6bpp skip/color: all pre/post pairs and independent X/Y flips.
            for(unsigned pair=0;pair<16;++pair)
                for(unsigned flips : {0u,0x10u,0x20u,0x30u})
                    known[0xe088u|(pair<<8)|flips]=true;
            // Pinned non-skipping unscaled fill: all BPP encodings and independent X/Y flips.
            for(unsigned base : {0x800cu,0x900cu,0xa00cu,0xb00cu,0xc00cu,0xd00cu,0xe00cu,0xf00cu})
                for(unsigned flips : {0u,0x10u,0x20u,0x30u})
                    known[base|flips]=true;
            // Pinned uncompressed nonzero-color: 1..8bpp, X/Y flips and both edge-skip encodings.
            for(unsigned base : {0x8008u,0x9008u,0xa008u,0xb008u,0xc008u,0xd008u,0xe008u,0xf008u})
                for(unsigned flags : {0u,0x10u,0x20u,0x30u,0x40u,0x50u,0x60u,0x70u})
                    known[base|flags]=true;
            return known;
        }();
        return table[command];
    }
    // Render's passes: both (the board's DMA: the whole draw validated, then written), only the validating one (put
    // always gets write false), or only the writing one when no read of the draw can fail (its source ends inside the
    // graphics ROM); else both, so a refused draw still changes nothing.
    enum class Passes{both,check,write};
    // The board's DMA over registers r (the destination clip in r12/r13 and r16/r17) and the palette register: every pixel
    // goes to put(index, pen, write), first with write false (the whole draw is validated before any change), then true;
    // between() runs between the passes. Returns the pixels the DMA timing charges.
    template<class Put,class Between>
    static unsigned Render(uint16_t command,const std::array<uint16_t,18>& r,uint16_t palette_register,
                           const std::vector<uint8_t>& graphics,Put&& put,Between&& between,size_t stride=512,
                           Passes passes=Passes::both) {
        // Validate the whole draw before framebuffer mutation.
        const bool fill=(command&15)==12;
        const bool opaque_copy=command==0x8423;
        const bool known=KnownCommand(command);
        const unsigned xstep=r[10]?r[10]:0x100,ystep=r[11]?r[11]:0x100;
        const bool scaled=xstep!=0x100 || ystep!=0x100;
        // The secret-fighter route's D002 zoom reaches step 0x40. E002 uses the qualified shrink path.
        const bool scaled_copy=(command==0xd002 && xstep>=0x40) || (command==0xe002 && xstep>=0x100);
        if(!known || (scaled && (!scaled_copy || xstep!=ystep)) ||
           (!fill && graphics.empty()))
            throw std::runtime_error("MK2 pair DMA outside measured unscaled commands; command " +
                std::to_string(command) + "; xstep " + std::to_string(xstep) +
                "; ystep " + std::to_string(ystep));
        const unsigned encoded_bpp=(command>>12)&7;
        const unsigned bpp=encoded_bpp?encoded_bpp:8, mask=(1u<<bpp)-1;
        const bool skip=command&0x80, flip=command&0x10;
        const unsigned width=r[6]&0x3ff,height=r[7]&0x3ff;
        const unsigned startskip=command&0x40?r[0]&0xff:0, endskip=command&0x40?r[0]>>8:r[0];
        const unsigned palette=palette_register&0x7f00;
        uint32_t source=fill?0:r[2]|(uint32_t(r[3])<<16);
        if(source>=0xf8000000) source-=0xf8000000;
        if(source>=0x10000000) throw std::runtime_error("MK2 pair DMA source outside graphics mapping");
        const uint8_t* const bytes=graphics.data();bool unchecked=false;
        auto extract=[&](uint32_t bit,unsigned bits) {
            if(unchecked)return ((unsigned(bytes[bit/8])|(unsigned(bytes[bit/8+1])<<8))>>(bit&7))&bits;
            return ((unsigned(graphics.at(bit/8))|(unsigned(graphics.at(bit/8+1))<<8))>>(bit&7))&bits;
        };
        // A single pass (check or write): every bit the draw reads lies before its source's end (the rows' headers and
        // pixels), so no read fails when that end's bytes are inside the ROM and the reads go unchecked; a write that does not
        // fit gets both passes. A row header read here fails as the validating pass would.
        if(passes!=Passes::both && (!fill || skip)){
            uint64_t end=source;
            if(scaled)end+=uint64_t(height)*width*bpp;
            else for(unsigned y=0;y<height;++y){
                if(skip){const unsigned packed=extract(uint32_t(end),0xff),pre=(packed&15)<<((command>>8)&3),post=(packed>>4)<<((command>>10)&3);
                    end+=8;if(width>pre+post)end+=uint64_t(width-pre-post)*bpp;}
                else end+=uint64_t(width)*bpp;
            }
            unchecked=end/8+1<graphics.size();
            if(!unchecked&&passes==Passes::write)passes=Passes::both;
        }
        const bool check_pass=passes!=Passes::write,write_pass=passes!=Passes::check;
        const unsigned clip_top=r[12]&0x1ff,clip_bottom=r[13]&0x1ff,clip_left=r[16]&0x3ff,clip_right=r[17]&0x3ff,color=r[9]&0xff;
        if(scaled) {
            // D002 and E002 share the uncompressed scaler for nonzero pixels.
            auto render_scaled=[&](bool write) {
                unsigned sy=r[5]&0x1ff;
                const int end=(int(width)-int(endskip))*256;
                for(unsigned iy=0;iy<(height<<8);iy+=ystep,sy=(sy+1)&0x1ff) {
                    if(sy<clip_top || sy>clip_bottom) continue;
                    const uint32_t row_bit=source+(iy>>8)*width*bpp;
                    unsigned sx=r[4]&0x3ff;
                    for(int ix=0;ix<end;ix+=int(xstep),sx=(sx+1)&0x3ff) {
                        if(sx<clip_left || sx>clip_right) continue;
                        const auto pixel=extract(row_bit+unsigned(ix>>8)*bpp,mask);
                        if(pixel)put(size_t(sy)*stride+sx,uint16_t(palette|pixel),write);
                    }
                }
            };
            if(check_pass)render_scaled(false);
            if(write_pass){if(check_pass)between();render_scaled(true);}
            return ((width<<8)/xstep)*((height<<8)/ystep);
        }
        auto render=[&](bool write) {
            uint32_t offset=source;
            unsigned sy=r[5]&0x1ff;
            for(unsigned y=0;y<height;++y,sy=(command&0x20?sy-1:sy+1)&0x1ff) {
                unsigned sx=r[4]&0x3ff, pre=0,post=0,ix=0;
                uint32_t bit=offset;
                int end=int(width);
                if(skip) {
                    const unsigned packed=extract(bit,0xff); bit+=8;
                    pre=(packed&15)<<((command>>8)&3); post=(packed>>4)<<((command>>10)&3);
                    sx=(flip?sx-pre:sx+pre)&0x3ff; ix=pre; end-=int(post);
                }
                if(sy>=clip_top && sy<=clip_bottom) {
                    if(ix<startskip) {bit+=(startskip-ix)*bpp; ix=startskip;}
                    if(end>int(width)-int(endskip)) end=int(width)-int(endskip);
                    for(;int(ix)<end && clip_left<=clip_right;++ix,sx=(flip?sx-1:sx+1)&0x3ff,bit+=bpp) {
                        if(sx<clip_left || sx>clip_right) {
                            // Every step up to the clip's next column draws nothing: taken at once.
                            const unsigned steps=(flip?sx-clip_right:clip_left-sx)&0x3ff;
                            if(int(ix)+int(steps)>=end) break;
                            ix+=steps-1; bit+=(steps-1)*bpp; sx=(flip?sx-(steps-1):sx+(steps-1))&0x3ff;
                            continue;
                        }
                        const auto pixel=fill?0u:extract(bit,mask);
                        if(fill || opaque_copy || pixel)
                            put(size_t(sy)*stride+sx,uint16_t(palette|(fill || (command&15)==8?color:pixel)),write);
                    }
                }
                if(skip) {
                    offset+=8;
                    if(width>pre+post) offset+=(width-pre-post)*bpp;
                } else offset+=width*bpp;
            }
        };
        if(check_pass)render(false);
        if(write_pass){if(check_pass)between();render(true);}
        return width*height;
    }

    unsigned StartDma(uint16_t command, BoardBootControls& board, const std::vector<uint8_t>& graphics,
                      bool scenery=false,uint64_t fighter=0) {
        auto draw=fighters.Draw(fighter);
        const auto prepare_fighter_surface=[&]{
            if(!fighter || !fighters_enabled || fighters.enabled())return;
            fighters=mk5presentation::FighterSurface(vram.size());
            fighters.ClearWith(0,vram.size(),[this](size_t pixel){
                const auto tag=BackdropEnabled()?backdrop[pixel]&backdrop_owner:0;
                return uint16_t((vram[pixel]&0x7fff)|tag);
            });
        };
        const auto present_pixel=[&](size_t index){
            fighters.Write(draw,index,uint16_t(vram[index]|(scenery?backdrop_owner:0)),
                           fighter?BloodPixel(index):0);
            MaskBlood(index);
            UpdateBackdrop(index,scenery);
        };
        // Widescreen: a mirrored DMA reaches the layer where it lands in VRAM; one that draws the world (a fighter's floor
        // shadow, by direct DMA) lands at the wide window's column, so it stays under its fighter near an arena's end.
        const bool actor=mk5wide::Layer<uint16_t>::ActorDraw(command);   // a floor shadow
        const int mirror_extra=mk5wide::kExtra-(wide_mirror_world?wide.shift:0);
        if(wide_mirror){const auto [lo,hi]=mk5wide::Span(dma_registers[4],dma_registers[6],dma_registers[10],(command&0x10)!=0);
            wide.Draw(mk5wide::Kind::real,lo+mirror_extra,hi+mirror_extra);}
        const auto pixels=Render(command,dma_registers,dma_palette,graphics,[&](size_t index,uint16_t pen,bool write){
            auto& dest=vram.at(index);
            if(write){dest=pen;present_pixel(index);
                if(wide_mirror){const int column=int(index&511)+mirror_extra;
                    if(column>=0&&column<mk5wide::kLayerWidth){const size_t at=(index>>9)*mk5wide::kLayerWidth+size_t(column);
                        wide.Write(at,pen,!wide_mirror_world);if(actor)wide.fixed[at]|=mk5wide::Layer<uint16_t>::kActor;}}}
        },[&]{prepare_fighter_surface();});
        board.dma_command=command; board.io[18]&=uint16_t(~2);
        return pixels;
    }
    // Widescreen (mk2_task_batch.cpp): the frame as the shadow pass composes it over all 512 VRAM columns. Each frame starts
    // from its page as VRAM holds it at the queue reset (after the page clear); the shadow walk draws its entries with the
    // wide clip; every other draw of the frame (DMAs not staged by the walk, the page clear, CPU writes) is mirrored as it
    // happens. Not in save states (shortcut, until W4).
    bool margin_streamer=false,margin_streaming=false;   // the scenery streamer has run since the pool reset / is running
    uint32_t margin_fault_pc=0;   // the last fault's PC (bit 0: in the walk, bit 1: an exception)
    uint64_t margin_streamer_skips=0;   // frames whose shadow walk ran without the streamer
    uint64_t margin_faults=0;   // shadow passes left incomplete (an outside PC, a refused access, a refused DMA)
    bool wide_walk_drawn=false;   // this frame's walk entries are in the layer: their real DMAs are not mirrored
    bool wide_mirror=false;       // set around a real DMA that the layer takes too
    // The wide layer (mk5_wide.hpp): pens, floor stains, marks and writers over both pages.
    mk5wide::Layer<uint16_t> wide;
    bool wide_hud=false,wide_mirror_world=false;   // a list with a screen-fixed camera (the HUD); a mirrored real DMA of scenery
    void MarkCleared(size_t index){wide.MarkCleared(index);}
    // The frame ends at the display interrupt: the margins without a scenery layer's art are masked.
    void EndWideFrame(){wide.EndFrame();}
    std::vector<uint32_t> wide_walk_entries;   // queue cursors of the walk's entries this frame (real staging)
    // The table's marker: the shadow's entries after it wait (wide_pending) until the drain reaches the real entries after
    // it (from wide_post_key on), so the floor shadows drawn at the marker stay under the world objects, as in VRAM.
    bool wide_shadow_post=false,wide_real_post=false;uint32_t wide_post_key=0;bool wide_post_keyed=false;
    // An entry for the layer: fixed, a screen-fixed (HUD) draw; outside, an object the arcade has not built yet (WideFresh):
    // its margin columns only; layer, its scenery layer (the walk's table entry, mk5wide::Band; -1: none).
    struct WideDraw{std::array<uint16_t,12> entry;bool fixed;bool outside=false;int layer=-1;
        template<class A> friend void Fields(A& a,WideDraw& d){a(d.entry,d.fixed,d.outside,d.layer);}};
    std::vector<WideDraw> wide_pending;
    // A scenery layer's entries (a list or a callback of the walk's table) wait in wide_plane until it ends (WidePlane), so
    // its art is measured whole (mk5wide::MeasureBand).
    struct WideStaged{std::array<uint16_t,12> entry;bool fresh;};
    int wide_layer=0;int16_t wide_camera=0;int wide_slot=-1;std::vector<WideStaged> wide_plane;   // the waiting list's camera x and far plane slot (-1: none)
    // The objects on the pool's free list when the shadow pass starts (sorted): one the shadow walk stages was built by the
    // shadow's own streamer run. The real streamer builds only every 8 frames, so such an object that reaches the arcade's
    // columns is not on the arcade screen yet: it draws only its margin columns (WideDraw::outside).
    std::vector<uint32_t> wide_free;
    // The palette slots' ROM pointers when the shadow pass starts (mk2::wide::kPaletteTable); not state (the pass never spans
    // a batch). A shadow draw in a slot the pass booked for another palette takes its colours from the ROM (Layer::Book).
    std::array<uint32_t,mk2::wide::kPaletteSlots> wide_palettes{};
    // Far-plane objects the shadow walk staged moving (x velocity long 16.16, not 0): the red-sky arena's dragons. The game
    // flies each across the arcade's screen: it puts the object just off one side of that screen, moves it by its
    // velocity, parks it once it is off the other side (velocity 0), waits and starts again. The wide picture shows more
    // of the plane than that screen (the margins, and the plane's own place under the wide camera: wide_delta), so the
    // game's flight would start out of nothing inside the picture, stand still at its end and then vanish. Here each
    // flight goes from one edge of the wide picture to the other:
    //  - a flight the game starts off the arcade's screen but inside the wide picture is shown further back on its way
    //    (lead, columns), where it starts just off the picture's edge, and keeps that distance all the way;
    //  - when the game parks the object off the arcade's screen, the flight goes on by itself (a WideGhost: its velocity
    //    and its recorded wing cycle) until it has left the picture, the next flight starting meanwhile.
    // An object the game stops on the arcade's own screen stays where it is shown, and one standing there with no flight
    // of its own is drawn as the arcade draws it. One not staged for kMoverKeep frames is forgotten (freed, reused), and
    // so are all of them when the game resets its object pool (a new scene). Nothing but the game's own velocity and poses is used; the acceptance counts a
    // ghost's draws (wide_flown) like any other of the shadow's.
    struct WideFlight{std::array<uint16_t,12> entry;int16_t hot;template<class A> friend void Fields(A& a,WideFlight& f){a(f.entry,f.hot);}};
    struct WideGhost{int32_t velocity=0,park_x=0;uint32_t parked=0;int16_t lead=0;uint32_t id=0;std::vector<WideFlight> flight;
        template<class A> friend void Fields(A& a,WideGhost& g){a(g.velocity,g.park_x,g.parked,g.lead,g.id,g.flight);}};
    struct WideMover{uint32_t object=0,seen=0;int32_t velocity=0;int16_t lead=0;uint8_t flying=0;uint32_t id=0;std::vector<WideFlight> flight;std::vector<WideGhost> ghosts;
        template<class A> friend void Fields(A& a,WideMover& m){a(m.object,m.seen,m.velocity,m.lead,m.flying,m.id,m.flight,m.ghosts);}};
    // Debug (wide.debug, the acceptance): each flight's draw in a build: its id (bit 0: the game started it on the
    // arcade's own screen, a scene that opens with the object in view), its layer columns and whether the game's object
    // is on the arcade's own screen now (a ghost's is not).
    struct WideSeen{uint32_t id;int16_t lo,hi;bool on_arcade;};
    std::vector<WideSeen> wide_seen,wide_seen_done;uint32_t wide_flights=0;   // wide_flights: flights started (their ids; state)
    static constexpr uint32_t kMoverKeep=8,kFlightOut=600;static constexpr size_t kFlight=48;static constexpr int kFlightEdge=8;   // kFlightOut: frames, a bound
    std::array<int16_t,10> wide_delta{};   // per scroll slot, this pass: the shadow's camera minus the arcade's (columns); not state
    std::vector<WideMover> wide_moving;uint32_t wide_frame=0;uint64_t wide_flown=0;   // wide_flown: ghost draws (diagnostics)
    static int EntryX(const std::array<uint16_t,12>& e){const int x=e[4]&0x3ff;return x>=768?x-1024:x;}
    static std::array<uint16_t,12> EntryAt(std::array<uint16_t,12> e,int x){e[4]=uint16_t((e[4]&~0x3ffu)|(unsigned(x)&0x3ffu));return e;}
    // The entries to draw for an object of a far list (x: its x position long, screen: its x on the plane's screen under
    // the shadow's camera, both from the game's RAM): its own, and its earlier flights still crossing the picture.
    std::vector<std::array<uint16_t,12>> WideMove(uint32_t object,int32_t velocity,int32_t x,int screen,const std::array<uint16_t,12>& entry){
        if(wide_slot<0)return {entry};
        auto it=std::find_if(wide_moving.begin(),wide_moving.end(),[&](const WideMover& m){return m.object==object;});
        if(!velocity&&it==wide_moving.end())return {entry};
        if(it==wide_moving.end()){wide_moving.push_back({});it=wide_moving.end()-1;it->object=object;}
        auto& m=*it;m.seen=wide_frame;
        const auto [lo,hi]=WideSpan(entry);   // VRAM columns under the shadow's camera
        const int delta=wide_delta[size_t(wide_slot)];
        // On the arcade's own screen: more than its edge columns (the game starts a flight with the object's first columns there).
        const bool on_arcade=std::min(hi+delta,mk2::wide::kMargin+400)-std::max(lo+delta,mk2::wide::kMargin)>kFlightEdge;
        std::vector<std::array<uint16_t,12>> shown;
        if(velocity){
            if(!m.flying){   // a flight starts: off the arcade's screen, it starts off the wide picture's edge too
                m.flying=1;m.flight.clear();m.lead=0;m.id=++wide_flights<<1|uint32_t(on_arcade);
                if(!on_arcade){
                    const int left=lo+mk5wide::kExtra,right=hi+mk5wide::kExtra;   // layer columns
                    if(velocity<0&&left<mk5wide::kLayerWidth)m.lead=int16_t(mk5wide::kLayerWidth-left);
                    else if(velocity>0&&right>0)m.lead=int16_t(-right);
                }
            }
            m.velocity=velocity;
            m.flight.push_back({entry,int16_t(EntryX(entry)-screen)});
            if(m.flight.size()>kFlight)m.flight.erase(m.flight.begin());
            shown.push_back(EntryAt(entry,EntryX(entry)+m.lead));
        }else if(m.flying&&!on_arcade&&!m.flight.empty()){   // parked off the arcade's screen: the flight goes on by itself
            m.ghosts.push_back({m.velocity,x,wide_frame,m.lead,m.id,std::move(m.flight)});m.flight.clear();m.flying=0;
        }else if(m.flying)shown.push_back(EntryAt(entry,EntryX(entry)+m.lead));   // stopped on the arcade's screen: as shown
        else if(on_arcade)shown.push_back(entry);   // no flight's object, standing on the arcade's screen: the arcade shows it
        const int camera=(x>>16)-screen;   // the list's camera under the shadow
        for(size_t g=0;g<m.ghosts.size();){
            auto& ghost=m.ghosts[g];const size_t n=ghost.flight.size();size_t period=n;
            // The wing cycle: the shortest period whose last frames repeat the ones before (image source), else the whole record.
            const auto source=[&](size_t i){return uint32_t(ghost.flight[i].entry[2])|uint32_t(ghost.flight[i].entry[3])<<16;};
            for(size_t p=4;p<=n/2&&period==n;++p){bool same=true;for(size_t k=0;k<p&&same;++k)same=source(n-1-k)==source(n-1-k-p);if(same)period=p;}
            const uint32_t t=wide_frame-ghost.parked+1;const auto& pose=ghost.flight[n-period+t%period];
            const int at=int((int64_t(ghost.park_x)+int64_t(ghost.velocity)*t)>>16)-camera+ghost.lead+pose.hot;
            auto e=EntryAt(pose.entry,at);
            e[5]=uint16_t((e[5]&~0x100u)|(entry[5]&0x100u));   // the y register's page bit: this frame's page
            const auto [a,b]=WideSpan(e);
            const bool gone=ghost.velocity<0?b+mk5wide::kExtra<=0:a+mk5wide::kExtra>=mk5wide::kLayerWidth;
            if(gone||t>kFlightOut||at<-256||at>=768){m.ghosts.erase(m.ghosts.begin()+std::ptrdiff_t(g));continue;}
            shown.push_back(e);++wide_flown;++g;
            if(wide.debug)wide_seen.push_back({ghost.id,int16_t(a+mk5wide::kExtra),int16_t(b+mk5wide::kExtra),false});
        }
        if(wide.debug&&m.flying){const auto [a,b]=WideSpan(shown.front());wide_seen.push_back({m.id,int16_t(a+mk5wide::kExtra),int16_t(b+mk5wide::kExtra),on_arcade});}
        // A flight shown away from the game's object is no part of the arcade's picture: the build's world is the shadow's.
        if((m.flying&&m.lead)||!m.ghosts.empty())wide.Whole();
        return shown;
    }
    bool WideFresh(uint32_t object)const{return std::binary_search(wide_free.begin(),wide_free.end(),object);}
    // An entry's columns in the layer [first, last) from its x (10-bit DMA x read as -256..767), width, X step and flip.
    static std::pair<int,int> WideSpan(const std::array<uint16_t,12>& e){return mk5wide::Span(e[4],e[6],e[10],(e[1]&0x10)!=0);}
    static bool WideOnArcade(const std::array<uint16_t,12>& e){
        const auto [lo,hi]=WideSpan(e);return hi>mk2::wide::kMargin&&lo<mk2::wide::kMargin+400;
    }
    void EnableWide(){wide.Enable(vram,false);}   // from what VRAM already holds; the stains' layer comes with the first stain
    bool WideEnabled()const{return wide.Enabled();}
    void StartWideFrame(unsigned page_row){
        wide.StartFrame(page_row,vram,blood_layer);
        wide_walk_entries.clear();wide_walk_drawn=false;wide_shadow_post=wide_real_post=wide_post_keyed=false;wide_pending.clear();
        wide_plane.clear();wide_layer=0;
        wide_seen_done.swap(wide_seen);wide_seen.clear();
        ++wide_frame;wide_moving.erase(std::remove_if(wide_moving.begin(),wide_moving.end(),[&](const WideMover& m){return wide_frame-m.seen>kMoverKeep;}),wide_moving.end());
    }
    void MirrorWide(size_t start,size_t count){wide.Mirror(vram,start,count);}
    uint32_t WideBloodPixel(size_t index)const{const auto pixel=wide.blood.empty()?0:wide.blood[index];return pixel?ExpandRgb555(pixel):0;}
    // One entry staged by the shadow walk (r0..r11 as the queue holds them) with the board's current clip; false when the
    // board's DMA would refuse it (it then draws nothing). A scenery entry waits for its layer's end.
    bool WideEntry(const std::array<uint16_t,12>& entry,const std::vector<uint8_t>& graphics,bool fresh=false){
        if(!wide_hud){wide_plane.push_back({entry,fresh});return true;}
        return WidePlace({entry,true,fresh&&WideOnArcade(entry),-1},graphics);
    }
    bool WidePlace(const WideDraw& draw,const std::vector<uint8_t>& graphics){
        if(wide_shadow_post){wide_pending.push_back(draw);return true;}
        return DrawWide(draw,graphics);
    }
    void FlushWide(const std::vector<uint8_t>& graphics){for(const auto& e:wide_pending)DrawWide(e,graphics);wide_pending.clear();}
    // A list or callback starts (or ends) in the shadow walk: the waiting layer is measured and placed.
    bool WideList(const std::vector<uint8_t>& graphics){const bool placed=WidePlane(graphics);++wide_layer;return placed;}
    bool WidePlane(const std::vector<uint8_t>& graphics){
        if(wide_plane.empty())return true;
        // An entry the board would refuse (its source outside the graphics: a stale object that only the shadow's wider
        // window stages, the game never draws it) is left out of the layer.
        // Measured over the shadow's whole clip (the layer and kReach columns on each side): a far plane's art ends are
        // known before they reach the wide screen (mk5wide::Layer::Learn).
        constexpr int kOrigin=mk5wide::kReach,kCanvas=mk5wide::kLayerWidth+2*mk5wide::kReach;
        std::vector<std::array<uint16_t,18>> draws;std::vector<std::pair<int,int>> meant;std::vector<uint8_t> refused;
        const auto measure=[&]{
            draws.clear();meant.clear();
            for(const auto& staged:wide_plane){
                const auto& e=staged.entry;const auto [a,b]=mk5wide::Span(e[4],e[6],e[10],(e[1]&0x10)!=0);
                auto canvas=LayerRegisters(e);canvas[4]=uint16_t((e[4]+kOrigin+mk5wide::kExtra)&0x3ff);canvas[17]=kCanvas-1;
                draws.push_back(canvas);meant.push_back({a+kOrigin+mk5wide::kExtra,b+kOrigin+mk5wide::kExtra});
            }
            refused.assign(draws.size(),0);
            return mk5wide::MeasureBand(uint16_t(wide_layer),draws,meant,[&](const std::array<uint16_t,18>& r,auto&& put){
                try{Render(r[1],r,r[8],graphics,[&](size_t index,uint16_t,bool){put(index);},[]{},kCanvas,Passes::check);return true;}   // the check pass (write false) gives every pixel
                catch(const std::runtime_error&){refused[size_t(&r-draws.data())]=1;return false;}},[](size_t,int,int){},kCanvas,kOrigin,&wide.shapes,wide.debug);
        };
        auto band=measure();
        if(std::find(refused.begin(),refused.end(),1)!=refused.end()){   // measured again without the refused entries
            for(size_t n=wide_plane.size();n--;)if(refused[n])wide_plane.erase(wide_plane.begin()+std::ptrdiff_t(n));
            band=measure();
        }
        if(wide.debug)for(const auto& staged:wide_plane){const auto& e=staged.entry;const auto [a,b]=mk5wide::Span(e[4],e[6],e[10],(e[1]&0x10)!=0);
            wide.notes.push_back({uint16_t(wide_layer),uint32_t(e[2]|uint32_t(e[3])<<16),uint16_t(e[5]&0xff),   // the row within its page
                int16_t(a+mk5wide::kExtra),int16_t(b+mk5wide::kExtra),wide_camera});}
        wide.Measured(wide_slot,wide_camera,std::move(band));
        bool placed=true;
        // A build whose world is the shadow's draws alone (an offset window, far planes at their even pace) has no arcade
        // columns to keep to: its planes stand elsewhere, and an object the arcade has not built yet is drawn whole.
        for(const auto& [e,fresh]:wide_plane)placed&=WidePlace({e,false,fresh&&!wide.whole&&WideOnArcade(e),wide_layer},graphics);
        wide_plane.clear();return placed;
    }
    // An entry's registers for the layer: x in layer columns, the layer's own clip.
    std::array<uint16_t,18> LayerRegisters(const std::array<uint16_t,12>& entry)const{
        auto r=dma_registers;std::copy(entry.begin(),entry.end(),r.begin());
        r[4]=mk5wide::LayerX(r[4]);r[16]=0;r[17]=mk5wide::kLayerWidth-1;return r;
    }
    bool DrawWide(const WideDraw& draw,const std::vector<uint8_t>& graphics){
        auto r=LayerRegisters(draw.entry);
        {const auto [a,b]=mk5wide::Span(draw.entry[4],draw.entry[6],draw.entry[10],(draw.entry[1]&0x10)!=0);
            wide.Draw(mk5wide::Kind::shadow,a+mk5wide::kExtra,b+mk5wide::kExtra,draw.layer);
            if(wide.debug){const auto [first,last]=mk5wide::Rows(r);
                wide.placed.push_back({int16_t(a+mk5wide::kExtra),int16_t(b+mk5wide::kExtra),uint16_t(first),uint16_t(std::max(last-first,0)),
                                       uint32_t(draw.entry[2]|uint32_t(draw.entry[3])<<16),draw.entry[1],draw.fixed,int16_t(draw.layer)});}}
        if(draw.outside){   // the margin columns only (an object the arcade has not built yet)
            const int first=mk5wide::kWideMargin-wide.shift,last=first+mk5wide::kVisible;   // the arcade's columns in the wide window
            auto left=r,right=r;left[17]=uint16_t(std::max(first,1)-1);right[16]=uint16_t(std::min(last,mk5wide::kLayerWidth-1));
            return (first<=0||RenderLayer(left,graphics))&&(last>=mk5wide::kLayerWidth||RenderLayer(right,graphics));
        }
        return RenderLayer(r,graphics,draw.fixed);
    }
    // Layer registers drawn into the layer; fixed: screen-fixed pixels.
    bool RenderLayer(const std::array<uint16_t,18>& r,const std::vector<uint8_t>& graphics,bool fixed=false){
        // mk5wide::Layer::Write through plain pointers (the render's stores cannot move the layer).
        uint16_t* const pens=wide.pens.data();uint8_t* const marks=wide.fixed.data();uint16_t* const stains=wide.blood.empty()?nullptr:wide.blood.data();
        const size_t size=wide.pens.size();const uint16_t booked=wide.booked[(r[8]>>8)&0x7f]?wide.kBooked:0;   // Layer::Book
        const uint8_t actor=mk5wide::Layer<uint16_t>::ActorDraw(r[1])?mk5wide::Layer<uint16_t>::kActor:0;   // a floor shadow
        try{
            Render(r[1],r,r[8],graphics,[&](size_t i,uint16_t pen,bool write){
                if(!write||i>=size)return;
                pens[i]=uint16_t(pen|booked);marks[i]=uint8_t(fixed|actor);if(stains)stains[i]=0;wide.Stamp(i);
            },[]{},mk5wide::kLayerWidth,Passes::write);
        }catch(const std::runtime_error&){return false;}
        return true;
    }
    std::array<uint16_t, 18> DmaRegisters(const BoardBootControls& board) const {
        auto result = dma_registers;
        result[1] = board.dma_command;
        result[8] = dma_palette;
        return result;
    }
    void WritePalette(uint32_t address, uint16_t data, uint16_t mask = 0xffff) {
        auto& word = palette_ram.at(PaletteOffset(address));
        word = uint16_t((word & uint16_t(~mask)) | (data & mask));
    }
    uint16_t ReadPalette(uint32_t address) const { return palette_ram.at(PaletteOffset(address)); }
    void WriteVram(uint32_t address, uint16_t data, const BoardBootControls& board,
                   uint16_t mask = 0xffff) {
        const unsigned offset = VramOffset(address);
        if (board.VideoBank()) {
            if (mask & 0x00ff) vram[offset] = uint16_t((data & 0xff) | ((dma_palette & 0xff) << 8));
            if (mask & 0xff00) vram[offset + 1] = uint16_t((data >> 8) | (dma_palette & 0xff00));
        } else {
            if (mask & 0x00ff) vram[offset] = uint16_t((vram[offset] & 0xff) | ((data & 0xff) << 8));
            if (mask & 0xff00) vram[offset + 1] = uint16_t((vram[offset + 1] & 0xff) | (data & 0xff00));
        }
        if (mask & 0x00ff) { fighters.Clear(offset,1,vram[offset]&0x7fff);MaskBlood(offset);UpdateBackdrop(offset,false); }
        if (mask & 0xff00) { fighters.Clear(offset+1,1,vram[offset+1]&0x7fff);MaskBlood(offset+1);UpdateBackdrop(offset+1,false); }
        if (mask & 0x00ff) MirrorWide(offset,1);
        if (mask & 0xff00) MirrorWide(offset+1,1);
    }
    uint16_t ReadVram(uint32_t address, const BoardBootControls& board) const {
        const unsigned offset = VramOffset(address);
        return board.VideoBank()
            ? uint16_t((vram[offset] & 0xff) | (vram[offset + 1] << 8))
            : uint16_t((vram[offset] >> 8) | (vram[offset + 1] & 0xff00));
    }
private:
    std::vector<uint16_t> backdrop;
    bool fighters_enabled=false;
    mk5presentation::FighterSurface fighters;
    mk5presentation::FighterSlots fighter_objects;
    std::map<uint32_t,DrawOwner> draw_queue;
    void UpdateBackdrop(size_t index,bool scenery){
        if(!BackdropEnabled())return;
        if(scenery)backdrop[index]=vram[index]|backdrop_owner;
        else backdrop[index]&=~backdrop_owner;
    }
    struct BloodDraw { uint32_t cursor,page,camera,clip_start,clip_end; bool visible;
        template<class A> friend void Fields(A& a,BloodDraw& d){a(d.cursor,d.page,d.camera,d.clip_start,d.clip_end,d.visible);}
    } blood_draw{};
    using BloodPalette=std::array<uint16_t,12>;
    // The kept colours and, per splash pixel, the floor it first covered: pen | kTaken (0 = not drawn yet), Sum then.
    struct BloodStain { BloodPalette colors{}; std::array<uint16_t,35*11> pen{}; std::array<uint8_t,35*11> floor{};
        template<class A> friend void Fields(A& a,BloodStain& s){a(s.colors,s.pen,s.floor);} };
    std::array<uint8_t,35*11> blood_pixels{};
    std::map<std::tuple<int16_t,int16_t,bool>,BloodStain> blood_stains;
    std::vector<uint16_t> blood_layer;
    std::vector<uint16_t> blood_pen;    // per video RAM word under a stain: the floor of its splash pixel
    std::vector<uint8_t> blood_floor;
    // Save-state value fields (mk2_save_state.hpp). Which layers exist is the edition, checked by the file header;
    // blood_pixels is decoded from the program ROM by EnableBlood and stays with the live session.
    template<class A> friend void Fields(A& a,BootVideo& v){
        a(v.palette_ram,v.vram,v.shift_register,v.dma_palette,v.dma_registers,v.backdrop_list,v.world_object_list,
          v.backdrop,v.fighters,v.fighter_objects,v.draw_queue,v.blood_draw,v.blood_stains,v.blood_layer,v.blood_pen,v.blood_floor);
        v.WideFields(a);
    }
    // Widescreen state (W4), kept when the session has the wide layer: a session without it drops a file's, and one with
    // it starts over from VRAM when the file has none (a state saved without widescreen). The shadow pass never spans a
    // chunk boundary, so its own per-pass values are not state.
    template<class A> void WideFields(A& a){
        const bool enabled=WideEnabled();bool present=enabled;a(present);
        if(present){wide.LayerFields(a);wide.EvenFields(a);a(margin_streamer,margin_streaming,wide_walk_entries,wide_walk_drawn,wide_shadow_post,
                                          wide_real_post,wide_post_key,wide_post_keyed,wide_pending,wide_moving,wide_frame,wide_flights);}
        if constexpr(A::reading){
            if(enabled==present)return;
            wide.Reset();wide_walk_entries.clear();wide_pending.clear();wide_moving.clear();wide_frame=0;wide_flights=0;
            wide_walk_drawn=wide_shadow_post=wide_real_post=wide_post_keyed=false;
            wide_post_key=0;margin_streamer=margin_streaming=false;
            if(enabled)EnableWide();
        }
    }
    static unsigned PaletteOffset(uint32_t address) {
        if (address < 0x01800000 || address > 0x0187fff0 || (address & 15))
            throw std::runtime_error("MK2 palette access outside aligned mapped words");
        return (address - 0x01800000) / 16;
    }
    static unsigned VramOffset(uint32_t address) {
        if (address > 0x003ffff0 || (address & 15))
            throw std::runtime_error("MK2 VRAM access outside aligned mapped words");
        return address / 8;
    }
};
}
