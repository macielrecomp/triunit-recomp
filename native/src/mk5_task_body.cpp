#include "mk5_task_body.hpp"
#include "mk5_audio_link.hpp"
#include "mk5_read_map.hpp"
#include "mk5_hitbox.hpp"
#include "mk5_widescreen.hpp"
#include <algorithm>
#include <stdexcept>

namespace {
constexpr uint32_t kResetVector=0xffffffe0;
constexpr uint32_t kTaskEntry=0xffc8bf50,kFirstEint=0xffc952d0,kDiHandler=0xffc98bd0,kMainLoop=0xffc955a0;
uint32_t rom32(const VerifiedMk5Program& p,uint32_t a){size_t i=(a-0xff800000u)/8;uint32_t v=0;for(unsigned k=0;k<4;++k)v|=uint32_t(p.bytes.at(i+k))<<(8*k);return v;}
struct Flag {bool& value;bool before;Flag(bool& v,bool now):value(v),before(v){v=now;}~Flag(){value=before;}};
}
Mk5TaskBodyRunner::Mk5TaskBodyRunner(const VerifiedMk5Program& program,Mk5BoardState& state,Mk5MemoryBuffers& memory,Mk5IrqController& controller,BootDiState& di_state)
    :p(program),m(memory),irq(controller),di(di_state),f(state),s(state.check.cpu){
    if(p.bytes.size()!=1048576 || p.sha256!=kMk5ProgramSha256 || Sha256(p.bytes)!=kMk5ProgramSha256)
        throw std::runtime_error("MK5 runner: verified program required");
    if(!irq.IsBoundTo(s,m) || irq.Started() || m.ram.size()!=262144 || m.vram.size()!=524288 || m.palette.size()!=32768 || f.cmos.size()!=8192 ||
       s.status!=0x10 || s.next_original_pc || s.reference_cycles || !s.writes.empty() || !f.byte_writes.empty() || !f.check.returns.empty() || di.completed_handlers)
        throw std::runtime_error("MK5 runner: power-on CPU, buffers and unstarted controller required");
    // TMS34010 reset: ST=10h, PC from the reset vector (34010 data sheet).
    s.next_original_pc=rom32(p,kResetVector)&0xfffffff0;
    expected_pc=s.next_original_pc;expected_sp=s.a[15];expected_cycles=0;
}
void Mk5TaskBodyRunner::video_update(){if(video)video->Update();}
void Mk5TaskBodyRunner::guard()const{
    if(!irq.IsBoundTo(s,m) || m.ram.size()!=262144 || m.vram.size()!=524288 || m.palette.size()!=32768 || f.cmos.size()!=8192 || s.next_original_pc!=expected_pc || s.a[15]!=expected_sp || s.reference_cycles!=expected_cycles)
        throw std::runtime_error("MK5 task-body: exclusive continuation/buffers were changed");
    if(!boot_mode && ((s.a[15]&15) || s.a[15]<0x010958b0 || s.a[15]>0x013fffe0))
        throw std::runtime_error("MK5 task-body: task stack outside guest RAM");
}
void Mk5TaskBodyRunner::pre(uint32_t pc){
    if(shadow){shadow_pre(pc);return;}
    // The real walk's first instruction after the shadow pass: counted and checked before the pass.
    if(resume_walk && pc==mk5widescreen::kWalkStart){resume_walk=false;s.next_original_pc=pc;return;}
    s.next_original_pc=pc;last_pre_pc=pc;++graph_steps;
    video_update();
    // A subroutine run and the quiet task-phase DI handler are one call/event
    // for the budget and the instruction history of the calling graph.
    if(uncounted()){if(irq.CheckBoundary(pc))throw TaskBodyStop::IrqHandler;return;}
    if(boot_mode){
        // The declared raster epoch begins at the first EINT of the reset path.
        if(pc==kFirstEint && !irq.Started())throw TaskBodyStop::Epoch;
        if(pc==kTaskEntry)throw TaskBodyStop::TaskEntry;
    }
    if(main_mode && inputs.stop_after_decompression && pc==0xffc8d4b0)throw TaskBodyStop::DecompressorComplete;
    if(main_mode && di.completed_handlers>=main_di_target)throw TaskBodyStop::DiLimit;
    if(pc==kMainLoop && !main_mode && !boot_mode){++result.main_loops;throw TaskBodyStop::MainLoop;}
    if(!remaining)throw TaskBodyStop::Budget;
    if(pc==kMainLoop && main_mode){if(main_started)++result.main_loops;main_started=true;}
    --remaining;++result.instruction_count;
    if(irq.CheckBoundary(pc))throw TaskBodyStop::IrqHandler;
    if(pc==mk5hitbox::kMk1.strike_pc)mk5hitbox::Strike(mk5hitbox::kMk1,s.a.data(),m.ram);   // training mode, read-only
    if((pc==0xffde7ad0 || pc==0xffc7a700 || pc==0xffc9f010 || pc==0xffca3d00) && video)
        video->ObserveBlood(pc,s);
    // Constructor returns for selection poses, finishers, afterimages and Test Your Might.
    switch(pc){
    case 0xffda2c30:case 0xffda2e90:case 0xffcc6360:
    case 0xffda5720:case 0xffda1070:case 0xffdbe060:case 0xffdad570:
    case 0xffce5660:case 0xffce5a20:
        if(video)video->BindFighter(s.a[8]);
        break;
    case 0xffdb0ff0:
        if(video)video->BindHeldProp(s.a[8]);
        break;
    }
    if(main_mode && video && video->WideEnabled())wide_pre(pc);
}
// Widescreen frame events of the real run, once per executed instruction (after every stop pre can raise).
void Mk5TaskBodyRunner::wide_pre(uint32_t pc){
    namespace w=mk5widescreen;auto& v=*video;
    const auto word=[&](uint32_t bit){return m.ram[(bit-0x01000000)/16];};
    const auto ram_long=[&](uint32_t bit){return uint32_t(word(bit))|uint32_t(word(bit+16))<<16;};
    switch(pc){
    case w::kDisplayIrq:v.WideEndUnbuilt();return;
    case w::kBuilder:v.WideFrame(ram_long(w::kPageOrigin)>>16&0x1ff,int16_t(word(w::kCameraX))==v.WideLayerState().held[0]);return;   // after the page clear
    case w::kStreamer:v.wide_streaming=true;return;
    case w::kWrapTest:{   // a wrapping group (A8): its parts and the ring's length, from the MOVI's immediate
        const int length=int(rom32(p,w::kWrapLength)&0xffff);uint32_t part=s.a[8];
        for(unsigned n=0;n<w::kWrapParts && part>=0x01000000 && part<0x01400000 && !(part&15);++n){v.WideWrap(part,length);part=ram_long(part+w::kPartLink);}
        return;
    }
    case w::kStreamerEnd:v.wide_streaming=false;return;
    case w::kWalkEnd:v.WideRealWalk(false);return;
    case w::kWalkStart:break;
    default:return;
    }
    v.WideRealWalk(true);
    if(ram_long(w::kClipTopLeft)!=0 || ram_long(w::kClipBottomRight)!=w::kArcadeBottomRight)return;
    v.WideBegin();
    const unsigned mode=word(w::kMode);
    const int low=int16_t(word(w::kCameraMin)),high=int16_t(word(w::kCameraMax));
    const int camera=int16_t(word(w::kCameraX));const auto& layer=v.WideLayerState();
    v.WideOffset(w::Shifting(mode)||w::Demo(mode,low,high,layer.arena,layer.fight,camera==layer.held[0]),camera,low,high,mode!=w::kAttractMode);
    // A moving world updates each plane's ratio (its velocity over the world's).
    for(unsigned slot=0;slot<w::kScrollSlots;++slot)
        v.WideLayerState().Velocity(slot,int32_t(ram_long(w::kScrollFirst+slot*0x20-w::kVelocityGap)),int32_t(ram_long(w::kWorldScroll-w::kVelocityGap)));
    shadow_a=s.a;shadow_b=s.b;shadow_status=s.status;shadow_returns=f.check.returns.size();shadow_writes=s.writes.size();
    shadow_stack_reads=f.check.stack_read_count;shadow_lowest_sp=f.check.lowest_sp;shadow_quanta=irq.elapsed_quanta();
    shadow_cycles=s.reference_cycles;shadow_steps=0;shadow_journal.clear();shadow=true;shadow_refused=false;
    shadow_window(pc);
    shadow_walk=v.wide_streaming;   // not while the interrupted run is inside the real streamer: its planes are half built
    if(!shadow_walk){push(pc,w::kStreamerReturn);s.next_original_pc=w::kStreamer;throw TaskBodyStop::Shadow;}
}
// The shadow's window, through the journal: the wide clip and, with a window offset d (mk5wide::WindowShift), every scroll
// moved by its ratio times d (mk5wide::ScrollOffset, Layer::PlaneScroll), so the streamer and the walk build the frame of camera x + d.
void Mk5TaskBodyRunner::shadow_window(uint32_t pc){
    namespace w=mk5widescreen;
    write(pc,w::kClipTopLeft,32,w::WideTopLeft());write(pc,w::kClipBottomRight,32,w::WideBottomRight());
    auto& layer=video->WideLayerState();const int camera=int16_t(m.ram[(w::kCameraX-0x01000000)/16]);
    const auto ram_scroll=[&](uint32_t scroll){const size_t i=(scroll-0x01000000)/16;return int32_t(m.ram[i]|uint32_t(m.ram[i+1])<<16);};
    const int64_t offset=mk5wide::ScrollOffset(ram_scroll(w::kWorldScroll),layer.shift,layer.AtLimit(camera));
    for(unsigned slot=0;slot<w::kScrollSlots;++slot){
        const uint32_t scroll=w::kScrollFirst+slot*0x20;const auto arcade=ram_scroll(scroll);
        const auto wide=slot==w::kWorldSlot?int32_t(arcade+offset):layer.PlaneScroll(slot,arcade,camera,offset);
        if(wide!=arcade){write(pc,scroll,32,uint32_t(wide));video->WideWhole();}
    }
}
void Mk5TaskBodyRunner::shadow_pre(uint32_t pc){
    namespace w=mk5widescreen;
    constexpr uint64_t kShadowSteps=4000000;
    s.next_original_pc=pc;
    if(++shadow_steps>kShadowSteps)throw TaskBodyStop::Untranslated;
    if(!shadow_walk){
        if(pc==w::kStreamerReturn){   // the streamer returned: the walk from its start, with the real registers
            s.a=shadow_a;s.b=shadow_b;s.status=shadow_status;s.next_original_pc=w::kWalkStart;shadow_walk=true;
            throw TaskBodyStop::Shadow;
        }
        if(pc==w::kStreamerPopStatus){s.status=pop();s.next_original_pc=pc+16;throw TaskBodyStop::Shadow;}   // POPST
        if(!w::StreamerCode(pc))throw TaskBodyStop::Untranslated;
        return;
    }
    if(pc==w::kWalkEnd){shadow_stop(video->WideList() && !shadow_refused);throw TaskBodyStop::Shadow;}
    if(!w::WalkCode(pc))throw TaskBodyStop::Untranslated;
    if(pc==w::kFixedTest){video->WideFixedObject((s.a[7]&1)!=0);return;}
    for(const auto call:w::kListCalls)if(pc==call){   // a list starts: A4 its camera x
        if(!video->WideList())shadow_refused=true;
        video->WideListCamera(int16_t(s.a[4]),w::FarSlot(s.a[0]));return;
    }
}
// Undo the pass: RAM from the journal, the registers and the host histories; time never moved.
void Mk5TaskBodyRunner::shadow_restore(){
    for(auto it=shadow_journal.rbegin();it!=shadow_journal.rend();++it)m.ram[it->first]=it->second;
    shadow_journal.clear();s.a=shadow_a;s.b=shadow_b;s.status=shadow_status;
    f.check.returns.resize(shadow_returns);s.writes.resize(shadow_writes);
    f.check.stack_read_count=shadow_stack_reads;f.check.lowest_sp=shadow_lowest_sp;
    if(irq.elapsed_quanta()!=shadow_quanta || s.reference_cycles!=shadow_cycles)
        throw std::logic_error("MK1 widescreen shadow pass reached the interrupt controller");
}
void Mk5TaskBodyRunner::shadow_stop(bool complete){
    shadow_restore();shadow=false;video->WideEnd(complete);
    if(!complete){++video->wide_faults;video->wide_fault_pc=s.next_original_pc;}
    resume_walk=true;s.next_original_pc=mk5widescreen::kWalkStart;
}
// A fault: in the streamer, this frame's walk runs without it; in the walk, the pass ends incomplete.
void Mk5TaskBodyRunner::shadow_failed(){
    namespace w=mk5widescreen;
    if(shadow_walk){const uint32_t pc=s.next_original_pc;shadow_stop(false);video->wide_fault_pc=pc;return;}
    shadow_restore();++video->wide_streamer_skips;
    shadow_window(w::kWalkStart);
    shadow_walk=true;s.next_original_pc=w::kWalkStart;
}
void Mk5TaskBodyRunner::step(uint32_t pc,uint32_t next,uint32_t cycles){
    if(shadow){s.next_original_pc=next;return;}
    if(!uncounted())result.instructions.push_back({pc,next,cycles});
    if(immediate_irq){immediate_irq=false;irq.AdvanceCycles(cycles);video_update();throw TaskBodyStop::IrqHandler;}
    s.next_original_pc=next;irq.AdvanceCycles(cycles);
    video_update();
    if(pc==0xffce2250 && !uncounted())++result.yields;
    if(irq.CheckBoundary(pc))throw TaskBodyStop::IrqHandler;
}
void Mk5TaskBodyRunner::z(uint32_t v){s.status=(s.status&~uint32_t(0x20000000))|(v?0:0x20000000);}
void Mk5TaskBodyRunner::nz(uint32_t v){s.status=(s.status&~uint32_t(0xb0000000))|(v&0x80000000);z(v);}
void Mk5TaskBodyRunner::load(uint32_t& r,uint32_t v){r=v;nz(v);}
void Mk5TaskBodyRunner::add(uint32_t& r,uint32_t v){uint32_t a=r,q=a+v;s.status&=0x0fffffff;s.status|=q&0x80000000;z(q);if(q<a)s.status|=0x40000000;if((~(a^v)&(a^q))&0x80000000)s.status|=0x10000000;r=q;}
void Mk5TaskBodyRunner::compare(uint32_t a,uint32_t v){uint32_t q=a-v;s.status&=0x0fffffff;s.status|=q&0x80000000;z(q);if(v>a)s.status|=0x40000000;if(((a^v)&(a^q))&0x80000000)s.status|=0x10000000;}
void Mk5TaskBodyRunner::sub(uint32_t& r,uint32_t v){uint32_t a=r;compare(a,v);r=a-v;}
void Mk5TaskBodyRunner::addc(uint32_t& r,uint32_t v){uint32_t carry=(s.status>>30)&1;uint32_t a=r,q=a+v+carry;s.status&=0x0fffffff;s.status|=q&0x80000000;z(q);if(uint64_t(a)+v>0xffffffff)s.status|=0x40000000;if((~(a^v)&(a^q))&0x80000000)s.status|=0x10000000;r=q;}
void Mk5TaskBodyRunner::shift(uint32_t& r,unsigned n,bool left){n&=31;s.status&=~uint32_t(0x60000000);if(n){s.status|=((left?r>>(32-n):r>>(n-1))&1)<<30;r=left?r<<n:r>>n;}z(r);}
void Mk5TaskBodyRunner::sla(uint32_t& r,unsigned n){n&=31;uint32_t a=r;s.status&=0x0fffffff;if(n){uint32_t mask=0xffffffffu<<(31-n);uint32_t bits=a&mask;if(bits && bits!=mask)s.status|=0x10000000;s.status|=((a>>(32-n))&1)<<30;r<<=n;}s.status|=r&0x80000000;z(r);}
void Mk5TaskBodyRunner::rotate(uint32_t& r,unsigned n){n&=31;s.status&=~uint32_t(0x60000000);if(n){s.status|=((r>>(32-n))&1)<<30;r=(r<<n)|(r>>(32-n));}z(r);}
void Mk5TaskBodyRunner::xy(uint32_t& r,uint32_t v,bool subtract){
    uint16_t x=uint16_t(subtract?r-v:r+v),y=uint16_t(subtract?(r>>16)-(v>>16):(r>>16)+(v>>16));
    s.status&=0x0fffffff;
    if(subtract){
        if(int16_t(v)==int16_t(r))s.status|=0x80000000;
        if(int16_t(v>>16)>int16_t(r>>16))s.status|=0x40000000;
        if(int16_t(v>>16)==int16_t(r>>16))s.status|=0x20000000;
        if(int16_t(v)>int16_t(r))s.status|=0x10000000;
    }else{
        if(!x)s.status|=0x80000000;
        if(y&0x8000)s.status|=0x40000000;
        if(!y)s.status|=0x20000000;
        if(x&0x8000)s.status|=0x10000000;
    }
    r=uint32_t(x)|(uint32_t(y)<<16);
}
unsigned Mk5TaskBodyRunner::fw(unsigned f0)const{unsigned bits=(s.status>>(f0*6))&31;return bits?bits:32;}
uint32_t Mk5TaskBodyRunner::port_read(uint32_t pc,uint32_t a,unsigned width){
    // IN0/IN1/IN2/DSW are adjacent 16-bit ports (pinned midtunit main_map);
    // the span after DSW reads unmapped HIGH. One snapshot per call/phase.
    const BootDiInputs* ports=boot_mode?&boot_inputs.ports:(inputs.quiet_di?&*inputs.quiet_di:nullptr);
    uint32_t v=0;bool dsw=false;
    for(unsigned b=0;b<width;++b){
        unsigned bit=a-0x01600000+b,word=bit/16,offset=bit%16;uint32_t value=0xffff;
        if(word<3){if(!ports)throw TaskBodyStop::DiInput;value=word==0?ports->in01:word==1?ports->in01>>16:ports->in2_dsw;}
        else if(word==3){
            if(boot_mode)value=boot_inputs.ports.in2_dsw>>16;
            else if(dsw_override)value=*dsw_override;
            else if(inputs.main_dsw)value=*inputs.main_dsw;
            else throw TaskBodyStop::DswInput;
            if(!dsw){dsw=true;result.dsw_read={pc,0x01600030,uint16_t(value),(value&0x8000)?0xffff0000u|value:value};}
        }
        v|=((value>>offset)&1u)<<b;
    }
    return v;
}
uint16_t Mk5TaskBodyRunner::vram_word(uint32_t a){
    if(video)return uint16_t(video->CpuRead(a,16));
    // T-Unit CPU view of VRAM: one byte per pixel word, low byte in the data
    // bank, high (palette) byte otherwise (pinned midtunit_v vram_r).
    uint16_t v=0;for(unsigned b=0;b<16;b+=8){uint16_t pixel=m.vram.at((a+b)/8);v|=uint16_t((data_bank?pixel&255:pixel>>8)<<b);}
    return v;
}
uint32_t Mk5TaskBodyRunner::read(uint32_t pc,uint32_t a,unsigned width,bool sign){
    uint32_t v=0;
    if(width<1 || width>32)throw TaskBodyStop::Untranslated;
    // The shadow pass reads no device with state or host effects (CMOS, cabinet ports, sound board, protection).
    if(shadow && ((a>=0x01400000 && a<0x01420000) || (a>=0x01600000 && a<0x01600050) || (a>=0x01d00000 && a<0x01d00010) ||
                  (a>=0x01b00000 && a<0x01b70000)))throw TaskBodyStop::Untranslated;
    if(a>=0x01000000 && uint64_t(a)+width<=0x01400000){
        for(unsigned b=0;b<width;++b){uint32_t at=a+b;v|=uint32_t((m.ram.at((at-0x01000000)/16)>>(at&15))&1)<<b;}
    }else if(a>=0x01400000 && a<0x01420000 && !(a&15) && width==8){v=uint8_t(f.cmos.at((a-0x01400000)/16));
    }else if(video && uint64_t(a)+width<=0x00400000){v=video->CpuRead(a,width);
    }else if(!video && !main_mode && uint64_t(a)+width<=0x00400000){
        for(unsigned b=0;b<width;++b){uint32_t at=a+b;uint16_t pixel=m.vram.at(at/8);v|=uint32_t(((data_bank?pixel&255:pixel>>8)>>(at&7))&1)<<b;}
    }else if(main_mode && video && a>=0x02000000 && uint64_t(a)+width<=0x08000000ULL){
        v=video->CpuRead(a,width);
    }else if(main_mode && a>=0x00400000 && uint64_t(a)+width<=0x01000000ULL){
        // Pinned midtunit main_map declares unmap_value_high for this hole.
        // Preserve the calculated address; only fully unmapped read spans qualify.
        v=width==32?0xffffffffu:((uint32_t(1)<<width)-1);
    }else if(main_mode && a>=0x01a80100 && uint64_t(a)+width<=0x01b00000ULL){
        // Pinned main_map: unmapped span after DMA and before MK protection.
        // Known DMA/protection spans remain governed by their existing branches.
        v=width==32?0xffffffffu:((uint32_t(1)<<width)-1);
    }else if(main_mode && ((a>=0x01420000 && uint64_t(a)+width<=0x01480000ULL) || (a>=0x01880000 && uint64_t(a)+width<=0x01a80000ULL))){
        // Pinned main_map: fully unmapped HIGH span between CMOS and CMOS enable.
        // Preserve address, state and all mapped neighbors; reads have no side effects.
        v=width==32?0xffffffffu:((uint32_t(1)<<width)-1);
    }else if(main_mode && a>=0x01480000 && uint64_t(a)+width<=0x01500000ULL){
        // Pinned CMOS-enable window has a write handler and unmapped HIGH reads.
        // Reading it does not arm CMOS writes or change its existing write path.
        v=width==32?0xffffffffu:((uint32_t(1)<<width)-1);
    }else if(main_mode && a>=0x1f800000 && uint64_t(a)+width<=0x20000000ULL){
        // Pinned midtunit.cpp main_map: MK reads the program ROM mirror here.
        // Preserve the calculated CPU address/registers; map data reads only.
        for(unsigned b=0;b<width;++b){uint32_t at=a-0x1f800000+b;v|=uint32_t((p.bytes.at(at/8)>>(at&7))&1)<<b;}
    }else if(a>=0xff800000 && uint64_t(a)+width<=0x100000000ULL){
        for(unsigned b=0;b<width;++b){uint32_t at=a+b;v|=uint32_t((p.bytes.at((at-0xff800000)/8)>>(at&7))&1)<<b;}
    }else if(a>=0xc0000000 && a<=0xc00001f0 && !(a&15) && width==16){
        // Before the declared epoch only previously requested registers exist.
        if(!irq.Started() && (a==0xc00001c0 || !irq.Requested((a-0xc0000000)/16)))throw TaskBodyStop::Untranslated;
        v=a==0xc00001c0?irq.SampleHcount():irq.io().at((a-0xc0000000)/16);
    }
    else if(main_mode && a==0xc00001d0 && width==8){
        // The attract renderer reads field0=8 from live VCOUNT. Sample the
        // existing raster register; byte extraction does not advance time.
        v=irq.io()[29]&0xff;
    }
    else if(!main_mode && a>=0x01600000 && uint64_t(a)+width<=0x01600050ULL){v=port_read(pc,a,width);}
    else if((a==0x01600000 || a==0x01600020) && width==32 && main_mode){if(!inputs.active_di)throw TaskBodyStop::DiInput;v=a==0x01600000?inputs.active_di->in01:inputs.active_di->in2_dsw;}
    else if(main_mode && (a==0x01600010 || a==0x01600020) && width==16){if(!inputs.active_di)throw TaskBodyStop::DiInput;v=a==0x01600010?inputs.active_di->in01>>16:inputs.active_di->in2_dsw&0xffff;}
    else if(main_mode && a>=0x01600000 && uint64_t(a)+width<=0x01600030ULL){
        // IN0/IN1/IN2 are adjacent16-bit ports. P2's original field starts at bit8.
        // Keep direct DSW/main_dsw and earlier scalar contracts independent.
        if(!inputs.active_di)throw TaskBodyStop::DiInput;
        for(unsigned b=0;b<width;++b){unsigned bit=a-0x01600000+b;uint32_t word=bit<32?inputs.active_di->in01:inputs.active_di->in2_dsw;v|=((word>>(bit<32?bit:bit-32))&1u)<<b;}
    }
    else if(a==0x01600030 && width==16){
        if(dsw_override)v=*dsw_override;else{if(!inputs.main_dsw)throw TaskBodyStop::DswInput;v=*inputs.main_dsw;}
        result.dsw_read={pc,a,uint16_t(v),(v&0x8000)?0xffff0000u|v:v};
    }
    else if(main_mode && a>=0x01b00000 && uint64_t(a)+width<=0x01b70000 && !(a&15) && width==16){v=protection.Read();}
    else if(a==0x01d00000 && width==8){
        if(boot_mode){mk5audio::GameAudioSync(s.reference_cycles);v=boot_inputs.sound_status;}
        else{if(!inputs.sound_status)throw TaskBodyStop::SoundInput;mk5audio::GameAudioSync(s.reference_cycles);v=main_mode?sound_status.Read():*inputs.sound_status;}
    }
    else if(video && a>=0x01a80000 && a<0x01a80100 && !(a&15) && width==16){v=shadow?video->ShadowRead((a-0x01a80000)/16):video->Read((a-0x01a80000)/16);}
    else if(a==0x01a80010 && width==16){
        // Busy-wait reads the live last normalized DMA command, not a fake idle.
        bool found=false;for(auto i=s.writes.rbegin();i!=s.writes.rend();++i)if(i->address_bit==a){v=i->value;found=true;break;}if(!found)throw TaskBodyStop::Untranslated;
    }else if(main_mode && Mk5ReadIsUnmapped(a,width)){
        // main_map.unmap_value_high(): all other fully unmapped read fields.
        // Mapped overlaps and unsupported writes retain their exact barriers.
        v=width==32?0xffffffffu:((uint32_t(1)<<width)-1);
    }else throw TaskBodyStop::Untranslated;
    if(sign && width<32 && (main_mode || boot_graph || width==8 || (s.status&0x20)) && (v&(1u<<(width-1))))v|=~((1u<<width)-1);
    (void)pc;return v;
}
void Mk5TaskBodyRunner::write(uint32_t pc,uint32_t a,unsigned width,uint32_t v){
    if(width<1 || width>32)throw TaskBodyStop::Untranslated;
    if(shadow){   // RAM through the journal; the DMA registers draw the wide layer only; nothing else
        if(a>=0x01000000 && uint64_t(a)+width<=0x01400000){
            for(uint32_t at=a&~15u;uint64_t(at)<uint64_t(a)+width;at+=16){const uint32_t i=(at-0x01000000)/16;shadow_journal.push_back({i,m.ram[i]});}
            for(unsigned b=0;b<width;++b){uint32_t at=a+b;auto& word=m.ram[(at-0x01000000)/16];unsigned shift=at&15;word=uint16_t((word&~(1u<<shift))|(((v>>b)&1)<<shift));}
            return;
        }
        if(video && a>=0x01a80000 && a<0x01a80100 && !(a&15) && (width==16 || width==32)){
            for(unsigned b=0;b<width;b+=16)if(!video->ShadowWrite((a+b-0x01a80000)/16,uint16_t(v>>b)))shadow_refused=true;
            return;
        }
        throw TaskBodyStop::Untranslated;
    }
    if(a>=0x01000000 && uint64_t(a)+width<=0x01400000){
        for(unsigned b=0;b<width;++b){uint32_t at=a+b;auto& word=m.ram.at((at-0x01000000)/16);unsigned shift=at&15;word=uint16_t((word&~(1u<<shift))|(((v>>b)&1)<<shift));}
        if(width==32 && (a==0x01051230 || a==0x01051510 || a==0x01008000) && video)
            video->FighterRamWrite(pc,a,v);
        // Word events preserve packed-byte neighbours; source semantics remain
        // available in the immutable native graph.
        for(uint32_t at=a&~15u;uint64_t(at)<uint64_t(a)+width;at+=16)s.writes.push_back({pc,at,m.ram.at((at-0x01000000)/16)});
        if(a==s.a[15])f.check.lowest_sp=std::min(f.check.lowest_sp,a);
    }else if(a==0x01480000 && width==8){cmos_write_enabled=true;f.byte_writes.push_back({pc,a,uint8_t(v),false,0,0,s.writes.size()});s.writes.push_back({pc,a,uint16_t(v&255)});
    }else if(a>=0x01400000 && a<0x01420000 && !(a&15) && width==8){
        if(!cmos_write_enabled)throw std::runtime_error("MK5 live CMOS write without enable");
        cmos_write_enabled=false;auto& cell=f.cmos.at((a-0x01400000)/16);uint16_t before=cell;cell=uint16_t((cell&0xff00)|(v&255));
        bool touched=std::any_of(f.byte_writes.begin(),f.byte_writes.end(),[&](const auto& w){return w.applied_to_cmos && w.address_bit==a;});
        if(!touched)++f.unique_words_written;
        f.byte_writes.push_back({pc,a,uint8_t(v),true,before,cell,s.writes.size()});++f.memory_write_count;s.writes.push_back({pc,a,cell});
    }else if(video && a<0x00400000){video->CpuWrite(a,width,v,!(pc==mk5widescreen::kNullWrite && !a));
    }else if(a<0x00400000 && uint64_t(a)+width<=0x00400000 && !(a&7) && (width==8 || width==16 || width==32)){
        // Pinned midtunit_v vram_w: in the data bank each CPU byte becomes a
        // pixel whose upper byte is the DMA palette byte of its parity.
        for(unsigned b=0;b<width;b+=8){
            auto& pixel=m.vram.at((a+b)/8);unsigned shift=((a+b)&8)?8:0;
            pixel=data_bank?uint16_t(((v>>b)&255)|(((dma_palette>>shift)&255)<<8)):uint16_t((pixel&255)|(((v>>b)&255)<<8));
        }
    }else if(a>=0xc0000000 && a<0xc0000200 && width==1){
        uint32_t aligned=a&~15u;uint16_t value=uint16_t((irq.io().at((a-0xc0000000)/16)&~(1u<<(a&15)))|((v&1)<<(a&15)));
        s.next_original_pc=instruction_next;immediate_irq=irq.WriteIo16(pc,aligned,value);
    }else if(a>=0xc0000000 && a<=0xc00001f0 && width==16 && !(a&15)){
        s.next_original_pc=instruction_next;
        immediate_irq=irq.WriteIo16(pc,a,uint16_t(v));
    }else if(a>=0x01800000 && uint64_t(a)+width<=0x01880000 && !(a&15) && (width==16 || width==32)){
        for(unsigned b=0;b<width;b+=16){m.palette.at((a+b-0x01800000)/16)=uint16_t(v>>b);s.writes.push_back({pc,a+b,uint16_t(v>>b)});}
    }else if(video && a>=0x01a80000 && a<0x01a80100 && !(a&15) && (width==16 || width==32)){
        for(unsigned b=0;b<width;b+=16){video->Write(pc,(a+b-0x01a80000)/16,uint16_t(v>>b));s.writes.push_back({pc,a+b,uint16_t(v>>b)});}
    }else if(a>=0x01a80000 && a<0x01a80100 && !(a&15) && (width==16 || width==32)){
        // DMA registers without the live engine: requests are recorded and
        // adopted at attach; a stop command lowers the DMA interrupt line.
        for(unsigned b=0;b<width;b+=16){
            uint32_t at=a+b;uint16_t value=uint16_t(v>>b);
            if(at==0x01a80010){if(value&0x8000)throw TaskBodyStop::Untranslated;if(irq.Started())irq.SetExternalLine(0,false);}
            if(at==0x01a80080)dma_palette=value;
            s.writes.push_back({pc,at,value});
        }
    }else if(main_mode && a>=0x01b00000 && uint64_t(a)+width<=0x01b70000 && !(a&15) && width==16){protection.Write(uint16_t(v));s.writes.push_back({pc,a,uint16_t(v)});}
    else if((a==0x01d01020 || a==0x01d01030 || a==0x01f00000 || a==0x01d81070) && width==16){if(a==0x01d01030){sound_status.Command();mk5audio::GameAudioWrite(s.reference_cycles,uint16_t(v));}if(a==0x01f00000){data_bank=(v&32)!=0;if(video)video->Control(uint16_t(v));}s.writes.push_back({pc,a,uint16_t(v)});}
    else throw TaskBodyStop::Untranslated;
}
void Mk5TaskBodyRunner::push(uint32_t pc,uint32_t v){if(s.a[15]<0x010958d0)throw std::runtime_error("MK5 task stack exhausted");s.a[15]-=32;write(pc,s.a[15],32,v);f.check.lowest_sp=std::min(f.check.lowest_sp,s.a[15]);}
uint32_t Mk5TaskBodyRunner::pop(){uint32_t v=read(s.next_original_pc,s.a[15],32);s.a[15]+=32;++f.check.stack_read_count;return v;}
uint32_t Mk5TaskBodyRunner::shift_load(uint32_t bit){
    if(shadow)throw TaskBodyStop::Untranslated;
    if(video)return video->ShiftLoad(bit);
    // VRAM-to-shift-register transfer (DPYCTL SRT): one 1024-pixel row.
    size_t index=bit>>3;if(index+1024>m.vram.size())throw std::runtime_error("MK5 shiftreg range");
    std::copy_n(m.vram.begin()+index,1024,di.shiftreg.begin());return di.shiftreg[0];
}
uint32_t Mk5TaskBodyRunner::pixblt_b_xy(uint32_t pc){
    // TMS34010 PIXBLT B,XY (pinned 34010gfx.hxx pixblt_b, non-linear
    // destination): binary source expanded to COLOR1/COLOR0, replace or
    // transparent replace, no window, executed in one step with its cycles.
    if(shadow)throw TaskBodyStop::Untranslated;
    const auto& io=irq.io();
    for(unsigned r:{8u,11u,20u,21u})if(!irq.Started() && !irq.Requested(r))throw TaskBodyStop::Untranslated;
    const unsigned psize=io[21];const uint16_t control=io[11];
    const unsigned rop=(control>>10)&31,window=(control>>6)&3;const bool transparent=(control&0x20)!=0;
    if((psize!=1 && psize!=2 && psize!=4 && psize!=8 && psize!=16) || rop || window || (io[8]&0x800) || (s.status&0x02000000))throw TaskBodyStop::Untranslated;
    const int dx=int16_t(s.b[7]),dy=int16_t(s.b[7]>>16);
    if(dx<=0 || dy<=0)throw TaskBodyStop::Untranslated;
    const unsigned pixelshift=psize==1?0:psize==2?1:psize==4?2:psize==8?3:4,ppw=16/psize;
    const uint32_t convdp=1u<<(~unsigned(io[20])&31),pixel_mask=(1u<<psize)-1;
    uint32_t saddr=s.b[0];
    uint32_t daddr=uint32_t(int32_t(int16_t(s.b[2]>>16))*int32_t(convdp)+(int32_t(int16_t(s.b[2]))<<pixelshift))+s.b[4];
    daddr&=~(psize-1);
    int left=int((ppw-((daddr&15)/psize))&(ppw-1)),right=int(((daddr+uint32_t(dx)*psize)&15)/psize),full=dx-left-right;
    if(full<0){left=dx;right=0;full=0;}else full/=int(ppw);
    const int op_timing=transparent?4:2;
    int words=full+(left?1:0)+(right?1:0);
    const uint32_t cycles=6+uint32_t((words*op_timing+words*int(psize)/16*2)*dy+2);
    const uint16_t color0=uint16_t(s.b[8]),color1=uint16_t(s.b[9]);
    for(int y=0;y<dy;++y){
        uint32_t sword=saddr>>4,dword=daddr>>4;
        uint16_t srcword=uint16_t(read(pc,sword++<<4,16,false)),srcmask=uint16_t(1u<<(saddr&15));
        auto pixels=[&](uint16_t dstword,uint16_t dstmask,int count){
            for(int x=0;x<count;++x){
                uint16_t pixel=uint16_t(((srcword&srcmask)?color1:color0)&dstmask);
                if(!transparent || pixel)dstword=uint16_t((dstword&~dstmask)|pixel);
                srcmask=uint16_t(srcmask<<1);if(!srcmask){srcword=uint16_t(read(pc,sword++<<4,16,false));srcmask=1;}
                dstmask=uint16_t(dstmask<<psize);
            }
            return dstword;
        };
        if(left){uint16_t w=vram_word(dword<<4);write(pc,dword<<4,16,pixels(w,uint16_t(pixel_mask<<(daddr&15)),left));++dword;}
        for(int n=0;n<full;++n){uint16_t w=transparent?vram_word(dword<<4):0;write(pc,dword<<4,16,pixels(w,uint16_t(pixel_mask),int(ppw)));++dword;}
        if(right){uint16_t w=vram_word(dword<<4);write(pc,dword<<4,16,pixels(w,uint16_t(pixel_mask),right));++dword;}
        saddr+=s.b[1];daddr+=s.b[3];
    }
    s.b[0]+=uint32_t(dy)*s.b[1];
    s.b[2]=(s.b[2]&0xffff)|(uint32_t(uint16_t((s.b[2]>>16)+uint32_t(dy)))<<16);
    s.status&=~uint32_t(0x02000000);
    return cycles;
}
void Mk5TaskBodyRunner::native_block(uint32_t pc){
    // The task and main graphs reach these routines (DSW decode, coinage and
    // control audit) through the power-on graph, run here as a subroutine
    // until it pops the caller's frame, inside the same IRQ-free horizon as
    // before. Budget and instruction counts see one call, as the graphs expect.
    if(shadow || (main_mode && (s.status&0x0fffffff)!=0x00200030))throw TaskBodyStop::Untranslated;
    if(pc==0xffe2a930 && !inputs.dsw && !boot_mode)throw TaskBodyStop::DswInput;
    uint64_t max=pc==0xffe2a930?311:10000;
    if(!irq.CanRunIrqFree(max))throw TaskBodyStop::TemporalBarrier;
    if(subroutine)throw std::runtime_error("MK5 nested power-on subroutine");
    uint64_t before=s.reference_cycles;uint32_t sp=s.a[15];
    {
        Flag running(subroutine,true);subroutine_sp=sp;
        const uint16_t* override=(!boot_mode && inputs.dsw)?&*inputs.dsw:nullptr;dsw_override=override;
        try{execute_boot();}catch(...){dsw_override=nullptr;throw;}
        dsw_override=nullptr;
    }
    if(s.a[15]!=sp+32)throw TaskBodyStop::Untranslated;
    if(s.reference_cycles-before>max)throw std::runtime_error("MK5 power-on subroutine exceeded its IRQ-free horizon");
    result.instructions.push_back({pc,s.next_original_pc,s.reference_cycles-before});
    if(pc==0xffe2a930)result.normalized_dsw=uint16_t(s.a[0]);
}
void Mk5TaskBodyRunner::execute(){
#include "mk5_task_body_graph.inc"
}
void Mk5TaskBodyRunner::execute_boot(){
    Flag in_boot(boot_graph,true);
#include "mk5_boot_graph.inc"
}
void Mk5TaskBodyRunner::dispatch(){
    // A PC belongs to exactly one of the power-on and task-body graphs. Each
    // returns (power-on) or stops Untranslated (task body) at a PC outside it;
    // an instruction-level frontier is the label just entered.
    for(;;){
        uint64_t start=graph_steps;
        execute_boot();
        try{execute();}
        catch(TaskBodyStop stop){
            if(stop!=TaskBodyStop::Untranslated || graph_steps==start || s.next_original_pc==last_pre_pc)throw;
        }
    }
}
TaskBodyResult Mk5TaskBodyRunner::Boot(const Mk5BootInputs& in){
    guard();if(!boot_mode)throw std::runtime_error("MK5 power-on: first task already entered");
    if(!in.instruction_budget)throw std::runtime_error("MK5 power-on instruction budget must be positive");
    boot_inputs=in;remaining=in.instruction_budget;result={};uint64_t before=s.reference_cycles;
    while(true){
        try{dispatch();}
        catch(TaskBodyStop stop){
            if(stop==TaskBodyStop::Epoch){irq.Start(in.pending,in.next_line,in.quanta_to_next);continue;}
            if(stop==TaskBodyStop::IrqHandler && s.next_original_pc==kDiHandler && !active_handler){
                const auto& e=irq.entries().back();
                if(e.target_pc!=kDiHandler || e.source_mask!=0x400 || !e.stacked || s.status!=0x10)throw std::runtime_error("MK5 power-on DI requires real entry");
                active_handler=true;continue;
            }
            if(stop==TaskBodyStop::TaskEntry){boot_mode=false;booted=true;}
            result.stop=stop;break;
        }
    }
    result.pc=s.next_original_pc;result.reference_cycles=s.reference_cycles-before;expected_pc=s.next_original_pc;expected_sp=s.a[15];expected_cycles=s.reference_cycles;return result;
}
TaskBodyResult Mk5TaskBodyRunner::Run(const TaskBodyInputs& in){
    guard();if(boot_mode || main_mode || completed)throw std::runtime_error("MK5 task-body: boot to the first task first; main loop uses the authorized main continuation");
    if(in.instruction_budget>500000 || !in.instruction_budget)throw std::runtime_error("MK5 task instruction budget must be 1..500000");
    inputs=in;remaining=in.instruction_budget;result={};uint64_t before=s.reference_cycles,handlers=di.completed_handlers;
    while(true){
        try{
            if(s.next_original_pc==kDiHandler && !active_handler){
                if(!inputs.quiet_di)throw TaskBodyStop::DiInput;
                if(m.ram.at((0x010080c0-0x01000000)/16)!=0)throw TaskBodyStop::IrqHandler;
                if(irq.entries().empty())throw std::runtime_error("MK5 quiet DI requires real entry");
                const auto& e=irq.entries().back();
                if(e.target_pc!=kDiHandler || e.source_mask!=0x400 || !e.stacked || s.status!=0x10)throw std::runtime_error("MK5 quiet DI requires real entry");
                active_handler=true;
            }
            dispatch();
        }catch(TaskBodyStop stop){
            if(stop==TaskBodyStop::IrqHandler && s.next_original_pc==kDiHandler && inputs.quiet_di && m.ram.at((0x010080c0-0x01000000)/16)==0)continue;
            result.stop=stop;break;
        }
    }
    completed=result.stop==TaskBodyStop::MainLoop;result.quiet_handlers=uint32_t(di.completed_handlers-handlers);
    result.pc=s.next_original_pc;result.reference_cycles=s.reference_cycles-before;expected_pc=s.next_original_pc;expected_sp=s.a[15];expected_cycles=s.reference_cycles;return result;
}
