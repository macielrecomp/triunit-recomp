#include "mk5_audio.hpp"
#include "mk5_audio_internal.hpp"
#include "mk5_program.hpp"
#include "williams_resampler.hpp"
#include "sinc_resampler.hpp"
#include "studio_master.hpp"
#include "mk5_audio_time.hpp"
#include "mk5_remastered_music.hpp"
#include "../mk2/mk2_state_io.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
namespace mk5audio {
struct WilliamsAudio::Impl: ymfm::ymfm_interface {
 Cpu6809State c;std::array<uint8_t,8192> ram{};std::array<uint8_t,43> hidden{};
 std::vector<uint8_t> u3;WilliamsSamples samples;Oki6295 oki;PresentationYm2151 ym;
 std::unique_ptr<RemasteredMusic> remastered;
 std::vector<int16_t> presentation_pcm;
 // Remastered presentation: band-limited reconstruction of the FM and DAC, whole (clean_*) and without the music
 // (effects_*), then the studio master. The ADPCM voices keep the reference kernel until they are auditioned on their
 // own: band-limited at 3.6 kHz they sounded darker to the user. The canonical PCM below keeps the reference kernel.
 SincResampler clean_fm{3579545/64},clean_dac{48000*32},effects_fm{3579545/64},effects_dac{48000*32};
 StudioMaster studio{StudioSettings::Mk1()};
 bool dac_music=false;
 std::vector<AudioEvent> events;std::vector<AudioCommand> commands;std::vector<int16_t> pcm;
 uint8_t bank=0,latch=0,talkback=0,dac=0;bool irq=false,firq=false,reset=false,frontier=false,reset_vector_pending=true;
 uint16_t source_pc=0;uint64_t requested=0,chip_cycle=0,pcm_phase=0,bus_cycle=0,output_samples=0;
 WilliamsResampler fm_stream{3579545/64},fm_right_stream{3579545/64},oki_stream{1000000/132},dac_stream{48000*32};
 std::array<YmTime,2> timers{};YmTime timer_origin{0,0},busy_end{0,0};int32_t fm_sample=0,oki_sample=0;
 struct Yield {};
 struct Access {uint16_t address;uint8_t value;bool write;};
 std::array<Access,40> completed_accesses{};size_t access_count=0,access_cursor=0;
 Cpu6809State instruction_start{};uint8_t instruction_bank=0,interrupt_kind=0;
 uint64_t horizon=0;bool pending_instruction=false,replaying=false;
 using ScheduleTime=unsigned __int128;
 // Shared scheduler timestamps use the reference's integer attoseconds.
 // Its YM wrapper forms a delay as clocks * floor(1e18 / clock_hz).
 // Keeping only the exact rational oscillator phase loses the sub-cycle
 // ordering when accumulated truncation puts expiry just before a CPU edge.
 static constexpr uint64_t schedule_unit=10000000000ULL;
 static constexpr uint64_t ym_attoseconds_per_clock=1000000000000000000ULL/3579545;
 bool shared=false,direct_history=false,cpu_running=false,mode_pending=false,check_pending=false;
 uint8_t mode_data=0;ScheduleTime base_time=0,mode_time=0,check_time=0;
 ScheduleTime raster_time=ScheduleTime(6325)*schedule_unit,stream_time=ScheduleTime(2000000)*schedule_unit;
 ScheduleTime clear_time=~ScheduleTime(0),boost_end=0;uint64_t main_requested=0;
 uint64_t event_sequence=0,mode_sequence=0,check_sequence=0;
 std::array<ScheduleTime,2> shared_timers{~ScheduleTime(0),~ScheduleTime(0)};
 ScheduleTime shared_timer_origin=0,shared_busy_end=0;
 std::array<uint64_t,2> timer_sequence{};
 bool command_pending=false;uint16_t pending_value=0;
 uint64_t pending_quanta=0,command_sequence=0;ScheduleTime command_time=0;
 ScheduleTime clock_time(uint64_t cycles)const{return ScheduleTime(cycles)*50*schedule_unit;}
 ScheduleTime timer_time(unsigned n)const{return shared?shared_timers[n]:(timers[n].cycle==UINT64_MAX?~ScheduleTime(0):clock_time(timers[n].cycle)+ScheduleTime(timers[n].fraction)*50*schedule_unit/3579545);}
 void ymfm_sync_mode_write(uint8_t data)override{
  if(!shared){m_engine->engine_mode_write(data);return;}
  if(mode_pending)throw std::logic_error("YM mode callback already pending");
  mode_pending=true;mode_data=data;mode_time=clock_time(chip_cycle);mode_sequence=++event_sequence;
 }
 void ymfm_sync_check_interrupts()override{
  if(!shared || !cpu_running){m_engine->engine_check_interrupts();return;}
  if(!check_pending){check_pending=true;check_time=clock_time(chip_cycle);check_sequence=++event_sequence;}
 }
 Impl(std::vector<uint8_t> a,std::vector<uint8_t> b,std::vector<uint8_t> d):u3(std::move(a)),samples(std::move(b),std::move(d)),oki(samples),ym(*this){
  if(u3.size()!=0x40000)throw std::invalid_argument("U3 size");
  ym.reset();c.pc=r16(0xfffe);
 }
 void ymfm_set_timer(uint32_t n,int32_t clocks)override{timers.at(n)=clocks<0?YmTime{}:timer_origin.AfterClocks(uint32_t(clocks));if(shared)shared_timers.at(n)=clocks<0?~ScheduleTime(0):shared_timer_origin+ScheduleTime(uint32_t(clocks))*ym_attoseconds_per_clock;timer_sequence.at(n)=++event_sequence;}
 void ymfm_set_busy_end(uint32_t clocks)override{busy_end=YmTime{chip_cycle,0}.AfterClocks(clocks);if(shared)shared_busy_end=clock_time(chip_cycle)+ScheduleTime(clocks)*ym_attoseconds_per_clock;}
 bool ymfm_is_busy()override{return shared?clock_time(chip_cycle)<shared_busy_end:!busy_end.Due(chip_cycle);}
 void ymfm_update_irq(bool asserted)override{firq=asserted;}
 void source_samples(){
  while(fm_stream.Written()*2000000<=chip_cycle*(3579545/64)){ymfm::ym2151::output_data data;ym.generate(&data);fm_sample=data.data[0]+data.data[1];fm_stream.Push(float(data.data[0]));fm_right_stream.Push(float(data.data[1]));if(remastered){ymfm::ym2151::output_data fx;ym.CurrentOutput(fx,ram[0x21a]);clean_fm.Push(float(data.data[0]+data.data[1]));effects_fm.Push(float(fx.data[0]+fx.data[1]));}}
  while(oki_stream.Written()*2000000<=chip_cycle*(1000000/132)){oki_sample=oki.StepSamples(1)[0];oki_stream.Push(float(oki_sample)/2);}
  while(dac_stream.Written()*2000000<=chip_cycle*(48000*32)){dac_stream.Push(float(int32_t(dac)*256-32768));if(remastered){clean_dac.Push(float(int32_t(dac)*256-32768));effects_dac.Push(float(dac_music?0:int32_t(dac)*256-32768));}}
 }
 void chips(uint64_t until){
  if(!chip_cycle){source_samples();if(!output_samples){pcm.push_back(0);if(remastered){presentation_pcm.push_back(0);presentation_pcm.push_back(0);}++output_samples;}}
  while(chip_cycle<until){
   ++chip_cycle;
   if(!shared)for(unsigned t=0;t<2;++t)if(timers[t].Due(chip_cycle)){timer_origin=timers[t];timers[t]=YmTime{};m_engine->engine_timer_expired(t);}
   timer_origin={chip_cycle,0};
   source_samples();
   pcm_phase+=48000;
   if(pcm_phase>=2000000){pcm_phase-=2000000;float value=fm_stream.Sample(output_samples)*.10f;value+=fm_right_stream.Sample(output_samples)*.10f;value+=dac_stream.Sample(output_samples)*.10f;value+=oki_stream.Sample(output_samples)*.15f;
    const auto original=int16_t(std::clamp(int32_t(value),-32768,32767));
    if(remastered){const float voices=oki_stream.Sample(output_samples)*.15f;
     // Under the user's music only the effects are mastered; otherwise the whole board.
     const float board=remastered->Replaces()?effects_fm.Sample(output_samples)*.10f+effects_dac.Sample(output_samples)*.10f+voices
                                             :clean_fm.Sample(output_samples)*.10f+clean_dac.Sample(output_samples)*.10f+voices;
     const auto master=studio.Process(board/32768);
     const auto stereo=remastered->Render({master[0]*32768,master[1]*32768});presentation_pcm.insert(presentation_pcm.end(),stereo.begin(),stereo.end());}
    ++output_samples;pcm.push_back(original);}
  }
 }
 // Bus taps in the reference observe a cycle after it has been consumed.
 void begin_bus(unsigned cost){bus_cycle=c.cycles+cost;}
 void await_cycle(uint64_t cycle){if(cycle>horizon)throw Yield{};chips(cycle);}
 bool access_replayed(uint16_t address,bool write,uint8_t& value){
  if(!replaying)return false;
  if(access_cursor<access_count){const auto& old=completed_accesses[access_cursor++];
   if(old.address!=address || old.write!=write || (write && old.value!=value))throw std::logic_error("non-deterministic audio continuation");
   value=old.value;return true;
  }
  return false;
 }
 void remember_access(uint16_t address,bool write,uint8_t value){
  if(!replaying)return;
  if(access_count==completed_accesses.size())throw std::logic_error("audio instruction access budget");
  completed_accesses[access_count++]={address,value,write};++access_cursor;
 }
 void log(uint16_t address,uint8_t value,bool write){events.push_back({bus_cycle,source_pc,address,value,bank,write});}
 uint8_t r8(uint16_t a){
  uint8_t cached=0;if(access_replayed(a,false,cached))return cached;
  if(replaying)await_cycle(bus_cycle);
  if(a<0x2000){auto v=ram[a];remember_access(a,false,v);return v;}
  if(a>=0xfb9c && a<=0xfbc6){auto v=hidden[a-0xfb9c];remember_access(a,false,v);return v;}
  if(a>=0xc000){auto v=u3[0x3c000+a-0xc000];remember_access(a,false,v);return v;}
  if(a>=0x4000){auto v=u3[size_t(bank)*0x8000+a-0x4000];remember_access(a,false,v);return v;}
  if(replaying)await_cycle(bus_cycle);else chips(bus_cycle);
  uint8_t v=0xff;
  switch(a&0xfc00){case 0x2400:v=ym.read(a&1);break;case 0x2c00:v=oki.Status();break;case 0x3000:v=latch;irq=false;if(shared)clear_time=clock_time(bus_cycle)+ScheduleTime(1000)*schedule_unit;break;default:break;}
  log(a,v,false);remember_access(a,false,v);return v;
 }
 uint16_t r16(uint16_t a){bool io=replaying || (a>=0x2000 && a<0x4000);if(io && bus_cycle>c.cycles)--bus_cycle;uint16_t hi=r8(a);if(io)++bus_cycle;return uint16_t((hi<<8)|r8(uint16_t(a+1)));}
 void w8(uint16_t a,uint8_t v){
  if(access_replayed(a,true,v))return;
  if(replaying)await_cycle(bus_cycle);
  if(a<0x2000){log(a,v,true);
   // Original ROM E501 activates a DAC stream. Bit 7 of its process type
   // distinguishes the music process from effect/voice processes (08/88).
   if(remastered&&source_pc==0xe501&&a==uint16_t((c.dp<<8)|0x2a)&&size_t(c.u)+4<ram.size())dac_music=(ram[c.u+4]&0x80)!=0;
   ram[a]=v;remember_access(a,true,v);return;}
  if(a>=0xfb9c && a<=0xfbc6){log(a,v,true);hidden[a-0xfb9c]=v;remember_access(a,true,v);return;}
  if(a>=0x2000 && a<0x4000){if(replaying)await_cycle(bus_cycle);else chips(bus_cycle);}
  log(a,v,true);
  switch(a&0xfc00){case 0x2000:bank=v&7;break;case 0x2400:ym.write(a&1,v);break;case 0x2800:dac=v;break;case 0x2c00:oki.Write(v);break;case 0x3400:samples.SelectBank(v);break;case 0x3c00:talkback=v;break;default:throw std::runtime_error("unmapped audio write");}
  remember_access(a,true,v);
 }
 void w16(uint16_t a,uint16_t v){bool io=replaying || (a>=0x2000 && a<0x4000);if(io && bus_cycle>c.cycles)--bus_cycle;w8(a,uint8_t(v>>8));if(io)++bus_cycle;w8(uint16_t(a+1),uint8_t(v));}
 uint16_t d()const{return uint16_t((c.a<<8)|c.b);}void setd(uint16_t v){c.a=uint8_t(v>>8);c.b=uint8_t(v);}
 void z16(uint16_t v){c.cc=uint8_t((c.cc&~4)|(v==0?4:0));}
 uint8_t nz8(uint8_t v){c.cc=uint8_t((c.cc&~14)|(v&0x80?8:0)|(v==0?4:0));return v;}
 uint16_t nz16(uint16_t v){c.cc=uint8_t((c.cc&~14)|(v&0x8000?8:0)|(v==0?4:0));return v;}
 uint8_t add8(uint8_t a,uint8_t b,unsigned carry){unsigned r=unsigned(a)+b+carry;c.cc=uint8_t((c.cc&~0x2f)|((a^b^r)&16?32:0)|(r&128?8:0)|(uint8_t(r)==0?4:0)|((~(a^b)&(a^r)&128)?2:0)|(r&256?1:0));return uint8_t(r);}
 uint16_t add16(uint16_t a,uint16_t b,unsigned carry){uint32_t r=uint32_t(a)+b+carry;c.cc=uint8_t((c.cc&~15)|(r&32768?8:0)|(uint16_t(r)==0?4:0)|((~(a^b)&(a^r)&32768)?2:0)|(r&65536?1:0));return uint16_t(r);}
 uint8_t sub8(uint8_t a,uint8_t b,unsigned carry){unsigned r=unsigned(a)-b-carry;c.cc=uint8_t((c.cc&~15)|(r&128?8:0)|(uint8_t(r)==0?4:0)|(((a^b)&(a^r)&128)?2:0)|(r&256?1:0));return uint8_t(r);}
 uint16_t sub16(uint16_t a,uint16_t b,unsigned carry){uint32_t r=uint32_t(a)-b-carry;c.cc=uint8_t((c.cc&~15)|(r&32768?8:0)|(uint16_t(r)==0?4:0)|(((a^b)&(a^r)&32768)?2:0)|(r&65536?1:0));return uint16_t(r);}
 enum class Unary{CLR,COM,NEG,INC,DEC,TST,LSR,ASR,ASL,LSL,ROL,ROR};
 uint8_t unary(uint8_t v,Unary op){
  unsigned carry=c.cc&1;uint8_t r=v;
  switch(op){
   case Unary::CLR:c.cc=uint8_t((c.cc&~15)|4);return 0;
   case Unary::COM:r=nz8(uint8_t(~v));c.cc|=1;return r;
   case Unary::NEG:return sub8(0,v,0);
   case Unary::INC:r=uint8_t(v+1);nz8(r);if(v==0x7f)c.cc|=2;return r;
   case Unary::DEC:r=uint8_t(v-1);nz8(r);if(v==0x80)c.cc|=2;return r;
   case Unary::TST:nz8(v);return v;
   case Unary::LSR:r=v>>1;c.cc=uint8_t((c.cc&~13)|(r==0?4:0)|(v&1));return r;
   case Unary::ASR:r=uint8_t(int8_t(v)>>1);break;
   case Unary::ROR:r=uint8_t((v>>1)|(carry<<7));break;
   case Unary::ASL:case Unary::LSL:r=uint8_t(v<<1);nz8(r);c.cc=uint8_t((c.cc&~3)|(v>>7)|(((v^r)&128)?2:0));return r;
   case Unary::ROL:r=uint8_t((v<<1)|carry);nz8(r);c.cc=uint8_t((c.cc&~3)|(v>>7)|(((v^r)&128)?2:0));return r;
  }
  c.cc=uint8_t((c.cc&~13)|(r&128?8:0)|(r==0?4:0)|(v&1));return r;
 }
 void push8(uint16_t& sp,uint8_t v){w8(--sp,v);++bus_cycle;}void push16(uint16_t& sp,uint16_t v){push8(sp,uint8_t(v));push8(sp,uint8_t(v>>8));}
 uint8_t pop8(uint16_t& sp){auto v=r8(sp);++sp;++bus_cycle;return v;}uint16_t pop16(uint16_t& sp){uint16_t v=pop8(sp);return uint16_t((v<<8)|pop8(sp));}
 void finish(unsigned cycles){if(replaying)await_cycle(c.cycles+cycles);c.cycles+=cycles;++c.instructions;chips(c.cycles);}
 bool interrupt(){
  if(interrupt_kind==2){bus_cycle=c.cycles+4;c.cc&=0x7f;push16(c.s,c.pc);push8(c.s,c.cc);c.cc|=0x50;bus_cycle=c.cycles+9;c.pc=r16(0xfff6);++c.firq_entries;finish(10);return true;}
  if(interrupt_kind==1){bus_cycle=c.cycles+4;c.cc|=0x80;push16(c.s,c.pc);push16(c.s,c.u);push16(c.s,c.y);push16(c.s,c.x);push8(c.s,c.dp);push8(c.s,c.b);push8(c.s,c.a);push8(c.s,c.cc);c.cc|=0x10;bus_cycle=c.cycles+18;c.pc=r16(0xfff8);++c.irq_entries;finish(19);return true;}
  return false;
 }
 void execute(){source_pc=instruction_start.pc;bus_cycle=c.cycles;if(interrupt())return;
#include "mk5_audio_graph.inc"
 }
 void step(){
  if(!pending_instruction){instruction_start=c;instruction_bank=bank;access_count=0;pending_instruction=true;
   // A fractional timer expiry is dispatched while the CPU is waiting for
   // its next clock edge. A timer exactly on that edge remains ordered after
   // the CPU's cycle, as in the reference scheduler.
   if(!shared)for(unsigned t=0;t<2;++t)if(timers[t].cycle==c.cycles && timers[t].fraction){timer_origin=timers[t];timers[t]=YmTime{};m_engine->engine_timer_expired(t);}
   timer_origin={chip_cycle,0};
   interrupt_kind=firq && !(c.cc&0x40)?2:irq && !(c.cc&0x10)?1:0;
  }
  c=instruction_start;access_cursor=0;replaying=true;
  try{execute();replaying=false;pending_instruction=false;}
  catch(const Yield&){replaying=false;throw;}
 }
 void advance(uint64_t quanta){
  if(quanta<requested)throw std::invalid_argument("audio time reversed");
  if(quanta>UINT64_MAX-50)throw std::overflow_error("audio clock");
  requested=quanta;uint64_t target=quanta/50;horizon=target;
  if(reset){if(target>c.cycles){c.cycles=target;chips(target);}return;}
  // RESET enters the four-cycle vector sequence (dummy, high, low, dummy)
  // before MAIN. These are bus cycles, not a translated ROM instruction.
  if(reset_vector_pending && c.cycles<target){if(c.cycles+4>target){chips(target);return;}c.cycles+=4;chips(c.cycles);reset_vector_pending=false;}
  try{while((pending_instruction || c.cycles<target) && !frontier && !(shared && (mode_pending||check_pending)))step();}catch(const Yield&){}
  if(!(shared && (mode_pending||check_pending)))chips(target);
 }
 void deliver(uint64_t q,uint16_t value){
  if(frontier)throw std::runtime_error("command at unresolved sound frontier");
  const uint64_t local=shared?chip_cycle:q/50;
  commands.push_back({q,local,value});bool asserted=!(value&0x100);
  if(remastered)remastered->Command(value);
  if(asserted){if(!shared)bank=0;irq=firq=false;reset=true;reset_vector_pending=false;pending_instruction=false;c.cycles=local;}
  else if(reset){reset=false;c.cycles=shared?local:(q+49)/50;c.cc|=0x50;c.dp=0;c.pc=r16(0xfffe);reset_vector_pending=true;}
  latch=uint8_t(value);irq=true;
 }
 void command(uint64_t q,uint16_t value){if(shared)throw std::logic_error("direct board write in shared history");direct_history=true;advance(q);deliver(q,value);}
 uint64_t checkpoint(uint64_t target)const{
  if(!pending_instruction || interrupt_kind)return target;
  const uint64_t offset=target-instruction_start.cycles;
  switch((uint32_t(instruction_start.pc<0xc000?instruction_bank:0)<<16)|instruction_start.pc){
#include "mk5_audio_checkpoints.inc"
   default:break;
  }
  return target;
 }
 ScheduleTime next_timer()const{
  ScheduleTime next=std::min(raster_time,stream_time);
  next=std::min(next,clear_time);
  for(unsigned t=0;t<2;++t)next=std::min(next,timer_time(t));
  if(mode_pending)next=std::min(next,mode_time);
  if(check_pending)next=std::min(next,check_time);
  if(command_pending)next=std::min(next,command_time);
  return next;
 }
 void dispatch(){
  // Newly inserted timers follow existing timers at equal expiry, exactly as
  // timer_list_insert; an overshooting CPU never reorders overdue callbacks.
  while(true){
   ScheduleTime when=~ScheduleTime(0);uint64_t sequence=UINT64_MAX;unsigned event=5;
   auto consider=[&](ScheduleTime time,uint64_t order,unsigned kind){if(time<when || (time==when && order<sequence)){when=time;sequence=order;event=kind;}};
   for(unsigned t=0;t<2;++t)consider(timer_time(t),timer_sequence[t],t);
   if(mode_pending)consider(mode_time,mode_sequence,2);
   if(check_pending)consider(check_time,check_sequence,3);
   if(command_pending)consider(command_time,command_sequence,4);
   if(when>base_time || event==5)break;
   if(event<2){shared_timer_origin=shared_timers[event];shared_timers[event]=~ScheduleTime(0);timer_origin=timers[event];timers[event]=YmTime{};m_engine->engine_timer_expired(event);}
   else if(event==2){mode_pending=false;shared_timer_origin=mode_time;timer_origin={uint64_t(mode_time/(50*schedule_unit)),0};m_engine->engine_mode_write(mode_data);}
   else if(event==3){check_pending=false;m_engine->engine_check_interrupts();}
   else{command_pending=false;deliver(pending_quanta,pending_value);boost_end=command_time+ScheduleTime(10000)*schedule_unit;}
  }
  if(clear_time<=base_time)clear_time=~ScheduleTime(0);
  while(raster_time<=base_time)raster_time+=ScheduleTime(6325)*schedule_unit;
  while(stream_time<=base_time)stream_time+=ScheduleTime(2000000)*schedule_unit;
 }
 bool sound_slice(ScheduleTime target){
  const uint64_t cycles=uint64_t(target/(50*schedule_unit));
  if(cycles<=chip_cycle)return false;
  cpu_running=true;
  advance(std::max(requested,cycles*50));
  if(!(mode_pending||check_pending)){
   const auto end=checkpoint(cycles);
   if(end>cycles)advance(std::max(requested,end*50));
  }
  cpu_running=false;
  // abort_timeslice steals the unconsumed budget. It must not become a
  // minimum for the following run after a newly inserted YM callback.
  requested=chip_cycle*50;
  return true;
 }
 void shared_advance(uint64_t q,bool has_command=false,uint16_t value=0){
  if(direct_history)throw std::logic_error("shared scheduler requires a fresh board history");
  if(q<main_requested)throw std::invalid_argument("main audio time reversed");
  if(q>UINT64_MAX-6325)throw std::overflow_error("main audio clock");
  shared=true;main_requested=q;const ScheduleTime until=ScheduleTime(q)*schedule_unit;
  bool queued=false;
  while(true){
   dispatch();
   const auto timer=next_timer();
   ScheduleTime budget=timer;
   if(base_time<boost_end)budget=std::min(budget,base_time+ScheduleTime(50)*schedule_unit);
   bool main_ran=false;
   if(!queued && budget>until){
    if(!has_command)break;
    queued=true;main_ran=true;command_pending=true;pending_value=value;
    pending_quanta=q;command_time=until;command_sequence=++event_sequence;
   }
   ScheduleTime target=queued?std::min(budget,until):budget;
   if(main_ran){
    // The T-unit resets the bank and cancels its clear timer in the main
    // write handler. CPU reset/input-line callbacks synchronize afterward.
    if(!(value&0x100)){bank=0;clear_time=~ScheduleTime(0);}
    // CPUmain executes first. MOVE_RA consumes its original three cycles
    // after the MMIO callback before its aborted timeslice returns.
    target=std::min(budget,until+ScheduleTime(3*16)*schedule_unit);
   }
   const bool ran=sound_slice(target);
   const auto local=clock_time(chip_cycle);
   if(ran && local<target)target=std::max(base_time,local);
   base_time=target;
   // Main and YM requests share expiry/insertion ordering, including ties.
   dispatch();
   if(queued && !command_pending)break;
  }
 }
};
WilliamsAudio::WilliamsAudio(std::vector<uint8_t>a,std::vector<uint8_t>b,std::vector<uint8_t>d){
 if(Sha256(a)!="ce0b5ee6c0ce7daf23dfcc4fb78a11bcc5caa600eeb3e6d7badbdf0fb880b396" || Sha256(b)!="a0512682b093a849a73ef4c7315695753bd713f1e6fef45ff10998a9de4e896b" || Sha256(d)!="f5200b5b3e5abb47c2220a57d9fb0d8fc7f6e1c1e617ce82f71d078a00697fcd")throw std::invalid_argument("original Williams audio ROM authentication failed");
 impl=std::make_unique<Impl>(std::move(a),std::move(b),std::move(d));
}
WilliamsAudio::~WilliamsAudio()=default;
void WilliamsAudio::AdvanceTo(uint64_t q){if(impl->shared)throw std::logic_error("direct advance in shared history");impl->direct_history=true;impl->advance(q);}
void WilliamsAudio::TmsWrite(uint64_t q,uint16_t v){impl->command(q,v);}
void WilliamsAudio::AdvanceMainTo(uint64_t q){impl->shared_advance(q);}
void WilliamsAudio::TmsInstructionWrite(uint64_t q,uint16_t v){impl->shared_advance(q,true,v);}
const Cpu6809State& WilliamsAudio::Cpu()const{return impl->c;}
const std::array<uint8_t,8192>& WilliamsAudio::Ram()const{return impl->ram;}
const std::array<uint8_t,43>& WilliamsAudio::HiddenRam()const{return impl->hidden;}
const std::vector<AudioEvent>& WilliamsAudio::Events()const{return impl->events;}
const std::vector<AudioCommand>& WilliamsAudio::Commands()const{return impl->commands;}
const std::vector<int16_t>& WilliamsAudio::Pcm()const{return impl->pcm;}
void WilliamsAudio::EnableRemasteredMusic(const std::filesystem::path& directory){if(impl->output_samples||impl->remastered)throw std::logic_error("remastered music requires a new board");impl->remastered=std::make_unique<RemasteredMusic>(directory);}
const std::vector<int16_t>& WilliamsAudio::PresentationPcm()const{return impl->presentation_pcm;}
std::string WilliamsAudio::RemasteredDiagnostics()const{if(!impl->remastered)return "\"music_mode\":\"original\",\"output_channels\":1";const auto& m=*impl->remastered;return "\"music_mode\":\"remastered\",\"output_channels\":2,\"remastered_starts\":"+std::to_string(m.Starts())+",\"remastered_frames\":"+std::to_string(m.Rendered())+",\"remastered_clipped\":"+std::to_string(m.Clipped())+",\"remastered_active_command\":"+std::to_string(m.ActiveCommand())+",\"remastered_loops\":"+std::to_string(m.Loops())+",\"remastered_finishes\":"+std::to_string(m.Finishes())+",\"remastered_commands\":"+m.CommandsJson();}
void WilliamsAudio::DiscardDiagnostics(){impl->events.clear();impl->commands.clear();impl->pcm.clear();impl->presentation_pcm.clear();}
std::string WilliamsAudio::MusicFingerprint()const{return impl->remastered?impl->remastered->Fingerprint():std::string{};}
template<class A> void Fields(A& a,Cpu6809State& c){a(c.pc,c.s,c.u,c.x,c.y,c.a,c.b,c.dp,c.cc,c.cycles,c.instructions,c.irq_entries,c.firq_entries);}
template<class A> void Fields(A& a,AudioEvent& e){a(e.cycle,e.pc,e.address,e.value,e.bank,e.write);}
template<class A> void Fields(A& a,AudioCommand& c){a(c.requested_quanta,c.effective_cycle,c.value);}
template<class A> void Fields(A& a,YmTime& t){a(t.cycle,t.fraction);}
template<class A,class Impl> static void AudioFields(A& a,Impl& s){
 a(s.c,s.ram,s.hidden,s.samples,s.oki,s.presentation_pcm,s.clean_fm,s.clean_dac,s.effects_fm,s.effects_dac,s.studio,s.dac_music,
   s.events,s.commands,s.pcm,s.bank,s.latch,s.talkback,s.dac,s.irq,s.firq,s.reset,s.frontier,s.reset_vector_pending,
   s.source_pc,s.requested,s.chip_cycle,s.pcm_phase,s.bus_cycle,s.output_samples,s.fm_stream,s.fm_right_stream,s.oki_stream,
   s.dac_stream,s.timers,s.timer_origin,s.busy_end,s.fm_sample,s.oki_sample,s.completed_accesses,s.access_count,
   s.access_cursor,s.instruction_start,s.instruction_bank,s.interrupt_kind,s.horizon,s.pending_instruction,s.replaying,
   s.shared,s.direct_history,s.cpu_running,s.mode_pending,s.check_pending,s.mode_data,s.base_time,s.mode_time,s.check_time,
   s.raster_time,s.stream_time,s.clear_time,s.boost_end,s.main_requested,s.event_sequence,s.mode_sequence,s.check_sequence,
   s.shared_timers,s.shared_timer_origin,s.shared_busy_end,s.timer_sequence,s.command_pending,s.pending_value,s.pending_quanta,
   s.command_sequence,s.command_time);
 // YM2151: ymfm's own save_restore (extended for exact restores, THIRD_PARTY_NOTICES.md).
 std::vector<uint8_t> ym;
 if constexpr(!A::reading){ymfm::ymfm_saved_state state(ym,true);s.ym.save_restore(state);}
 a(ym);
 if constexpr(A::reading){ymfm::ymfm_saved_state state(ym,false);s.ym.save_restore(state);}
 if(s.remastered)a(*s.remastered);
}
void Fields(mk2::state::Writer& a,WilliamsAudio& w){AudioFields(a,*w.impl);}
void Fields(mk2::state::Reader& a,WilliamsAudio& w){AudioFields(a,*w.impl);}
uint8_t WilliamsAudio::Bank()const{return impl->bank;}uint8_t WilliamsAudio::Talkback()const{return impl->talkback;}
std::string WilliamsAudio::Frontier()const{if(!impl->frontier)return "";std::ostringstream s;s<<unsigned(impl->bank)<<':'<<std::hex<<impl->c.pc;return s.str();}
}
