"""Compare complete cold DAC/speaker/PCM timelines through translated frontiers; keep only a receipt."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.mk2 import dac_reference as original, native, run, batch_reference, reference
from tools.quality import run as quality


def validate_blocks(meta,blocks=56):
    dac=[[n*2000000,48000 if n<4 else 31250,
          961 if n==1 else 960 if n<4 else 626 if n==4 else 625] for n in range(1,blocks+1)]
    speaker=[[n*2000000,961 if n==1 else 960] for n in range(1,blocks+1)]
    if meta.get('dac_blocks')!=dac or meta.get('speaker_blocks')!=speaker:
        raise ValueError('Missing, extra or mistimed prolonged DAC/speaker block')


def verify(through='next-wait'):
    result={'passed':False,'through':through,
            'integral_cpu_dsp_qualification':False}
    try:
        if through not in ('next-wait','first-fight'):raise ValueError('Select next-wait or first-fight PCM frontier')
        expected=native.frontier(through)
        end_cycle=expected['cycle'];blocks=end_cycle*16//2000000
        source_count,speaker_count=blocks*625+1,blocks*960+1
        result['scope']=f'cold reset through {through}; complete original {blocks*20}ms DAC31250/speaker48000/PCM16 timeline'
        result['roms']=run.verify_roms(run.ROOT/'roms/mk2')
        if quality.sha(batch_reference.MAME)!=batch_reference.MAME_SHA:raise ValueError('MAME identity differs')
        paths=list(dict.fromkeys(run.SOURCES+run.HEADERS+run.VERIFIERS+[
            'native/tests/mk2_continuation_audio_tests.cpp','native/mk2/mk2_audio.hpp',
            'native/audio/williams_resampler.hpp','native/audio/sinc_resampler.hpp','native/audio/studio_master.hpp','tools/mk2/native.py',
            'tools/mk2/dac_reference.py','tools/mk2/continuation_audio_reference.py']))
        before={name:quality.sha(__import__('tools.setup.mk2',fromlist=['source']).source(name)) for name in paths}
        with quality.scratch_tree() as (scratch,cleanup):
            result['cleanup']=cleanup
            native.prepare_reference_roms(run.ROOT/'roms/mk2',scratch/'mk2',run.read_manifest())
            lua=original.LUA
            if lua.count('assert(source_end==6251)')!=1:raise ValueError('Original hook boundary changed')
            lua=lua.replace('assert(source_end==6251)',f'assert(source_end=={source_count})')+native.REFERENCE_COIN_INPUT_LUA
            if through=='first-fight':
                lua+='\nlocal start_requested=0\n'+batch_reference.START_INPUT_LUA.replace('input_events','events')
                lua+=batch_reference.SELECTION_INPUT_LUA.replace('input_events','events').replace('input_port','port')
            (scratch/'audio.lua').write_text(lua,encoding='ascii')
            registers=[f'a{n}' for n in range(15)]+['sp']+[f'b{n}' for n in range(15)]
            fmt='MK2_AUDIO_CPU '+' '.join(['%X']*34)+'\\n'
            script='focus :maincpu\n'
            for pc,cycle in ((0xffa5aef0,506023),(0xffa5b130,571254)):
                script+=f'bpset {pc:x},totalcycles=={cycle:x},{{printf "MK2_AUDIO_INPUT %X\\n",w@1a80000;g}}\n'
            script+=f'bpset ff80dd30,totalcycles=={end_cycle:x},{{printf "{fmt}",pc,st,totalcycles,'+','.join(registers)+';quit}\ngo\n'
            (scratch/'audio.cmd').write_text(script,encoding='ascii')
            sources=[name for name in run.SOURCES if '/tests/' not in name]
            build=quality.command([quality.DEFAULT_CC,'-std=c++17','-O2','-Wall','-Wextra','-Werror',
                '-I'+str(run.ROOT/'native/include'),run.ROOT/'native/tests/mk2_continuation_audio_tests.cpp',
                *[run.ROOT/name for name in sources],'-lbcrypt','-o',scratch/'native.exe'],
                scratch,env=native.environment(),timeout=240)
            result['build']=quality.compact(build)
            if not build['passed']:raise RuntimeError('Continuation PCM build failed: '+build['stdout'][-3000:])
            execution=quality.command([scratch/'native.exe',run.ROOT/'roms/mk2',scratch/'native-pcm.bin']+
                (['first-fight'] if through=='first-fight' else []),
                scratch,env=native.environment(),timeout=180)
            result['native_execution']=quality.compact(execution)
            if not execution['passed']:raise RuntimeError('Continuation PCM failed: '+execution['stdout'][-3000:])
            result['native']=json.loads(execution['stdout'])
            capture=quality.command([batch_reference.MAME,'mk2','-noreadconfig','-nowriteconfig','-rompath',scratch,
                '-video','none','-sound','none','-nothrottle','-skip_gameinfo','-noplugins','-frameskip','0',
                '-debug','-debugger','windows','-debuglog','-debugscript','audio.cmd','-autoboot_delay','0',
                '-autoboot_script','audio.lua','-nvram_directory',scratch/'nvram','-cfg_directory',scratch/'cfg',
                '-snapshot_directory',scratch/'snap','-homepath',scratch/'home','-seconds_to_run',str(end_cycle*16//100000000+1)],
                scratch,cwd=scratch,timeout=180)
            result['reference_execution']=quality.compact(capture)
            if not capture['passed'] or capture['discarded_output_bytes']:raise RuntimeError('MAME prolonged PCM capture failed: '+capture['stdout'][-3000:])
            lines=(scratch/'debug.log').read_text().splitlines()
            for line in capture['stdout'].splitlines()+lines:reference.reject_mame_diagnostic(line)
            cpu=[[int(value,16) for value in line.split()[1:]] for line in lines if line.startswith('MK2_AUDIO_CPU ')]
            if len(cpu)!=1 or not native.frontier_matches(cpu[0],expected):
                raise ValueError('MAME terminal CPU boundary differs')
            if cpu[0]!=result['native']['cpu34']:raise ValueError('Complete terminal CPU34 differs')
            meta=json.loads((scratch/'audio-meta.json').read_text())
            if meta.get('inputs')!=native.REFERENCE_INPUTS[:20 if through=='first-fight' else 12]:raise ValueError('Original legal input timeline differs')
            validate_blocks(meta,blocks)
            raw=(scratch/'native-pcm.bin').read_bytes()
            speaker=(scratch/'reference-speaker.bin').read_bytes()
            result['speaker48000']=original.compare_float32(raw[:speaker_count*4],speaker,speaker_count,'speaker48000')
            result['dac31250']=original.compare_float32(raw[speaker_count*4:(speaker_count+source_count)*4],
                (scratch/'reference-dac.bin').read_bytes(),source_count,'DAC31250')
            result['pcm16']=original.compare_pcm16(raw[(speaker_count+source_count)*4:],speaker)
            if not result['speaker48000']['nonzero_samples'] or not result['dac31250']['nonzero_samples']:
                raise ValueError('Silent-only comparison is insufficient')
            if before!={name:quality.sha(__import__('tools.setup.mk2',fromlist=['source']).source(name)) for name in paths}:raise ValueError('Source drift during prolonged PCM proof')
            if quality.sha(batch_reference.MAME)!=batch_reference.MAME_SHA:raise ValueError('MAME changed during proof')
            result.update(passed=True,inputs=before,inputs_equal=True,cpu34=cpu[0],reference_meta=meta,
                reference_executable_sha256=batch_reference.MAME_SHA,
                lua_sha256=quality.sha(scratch/'audio.lua'),debug_script_sha256=quality.sha(scratch/'audio.cmd'))
    except Exception as error:result.update(passed=False,error=str(error))
    original.finalize_result(result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--through',choices=('next-wait','first-fight'),default='next-wait')
    parser.add_argument('--out',type=Path)
    args=parser.parse_args();result=verify(args.through)
    out=args.out or quality.ROOT/f'build/native/mk2-continuation-20261101/audiovisual/{args.through}-pcm.json'
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:result.get(key) for key in ('passed','error','speaker48000','dac31250','pcm16','cleanup')}))
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
