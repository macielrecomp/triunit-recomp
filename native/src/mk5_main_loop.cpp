#include "mk5_task_body.hpp"
#include "mk5_audio_link.hpp"
#include <stdexcept>

void Mk5TaskBodyRunner::AttachVideo(const VerifiedMk5GlyphBank& bank,bool remastered){guard();if(video)throw std::runtime_error("MK5 video already attached");video=std::make_unique<Mk5LiveVideo>(bank,s,m,irq,di,remastered?&p:nullptr);}

#include "mk5_main_loop_shards.inc"
void Mk5TaskBodyRunner::execute_main(){
#include "mk5_main_loop_driver.inc"
}
TaskBodyResult Mk5TaskBodyRunner::ContinueMain(const TaskBodyInputs& in,uint32_t di_limit){
    guard();if(!di_limit || di_limit>64 || !in.instruction_budget || in.instruction_budget>500000)throw std::runtime_error("MK5 main-loop: invalid bounded continuation");
    if(!main_mode){
        if(!completed || s.next_original_pc!=0xffc955a0)throw std::runtime_error("MK5 main-loop: completed live first-loop runner required");
        main_mode=true;completed=false;main_di_target=di.completed_handlers+di_limit;main_di_limit=di_limit;
    }else if(di.completed_handlers>=main_di_target){main_di_target=di.completed_handlers+di_limit;main_di_limit=di_limit;}
    else if(di_limit!=main_di_limit)throw std::runtime_error("MK5 main-loop: resume must preserve the pending DI limit");
    inputs=in;remaining=in.instruction_budget;result={};uint64_t before=s.reference_cycles;
    while(true){
        TaskBodyStop stop;
        try{
            if(s.next_original_pc==0xffc98bd0){
                if(!inputs.active_di || !video)throw TaskBodyStop::IrqHandler;
                if(irq.entries().empty() || irq.entries().back().target_pc!=0xffc98bd0 || irq.entries().back().source_mask!=0x400 || !irq.entries().back().stacked || s.status!=0x10 || read(0xffc98bd0,s.a[15],32)!=irq.entries().back().saved_status || read(0xffc98bd0,s.a[15]+32,32)!=irq.entries().back().return_pc)throw std::runtime_error("MK5 active DI requires real entry");
                active_handler=true;
            }
            interrupt_leave=false;execute_main();interrupt_leave=false;   // it returns when a shard left for an interrupt entry
            stop=TaskBodyStop::IrqHandler;
        }catch(TaskBodyStop thrown){interrupt_leave=false;stop=thrown;
        }catch(const std::exception&){interrupt_leave=false;if(!shadow)throw;shadow_failed();continue;}
        // Widescreen: the shadow pass redirected the next PC, or ended by a fault (it never stops a call).
        if(stop==TaskBodyStop::Shadow)continue;
        if(shadow){shadow_failed();continue;}
        if(stop==TaskBodyStop::IrqHandler && s.next_original_pc==0xffc98bd0 && inputs.active_di && video)continue;
        result.stop=stop;break;
    }
    mk5audio::GameAudioSync(s.reference_cycles);
    result.pc=s.next_original_pc;result.reference_cycles=s.reference_cycles-before;expected_pc=s.next_original_pc;expected_sp=s.a[15];expected_cycles=s.reference_cycles;return result;
}
