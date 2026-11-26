"""TriUnit Recomp setup: one entry point for every supported game.

    python tools/setup/setup.py --game mk3 --roms <folder with the user's ROM files>
    python tools/setup/setup.py --game mk3 --roms <folder> --no-compile      # authenticate and generate only
    python tools/setup/setup.py --game mk3 --roms <folder> --verify-only     # per-file check report, nothing written
    python tools/setup/setup.py --requirements                               # Windows, Python and compiler found here
    --progress <file>   keeps <file> (JSON, replaced atomically) at the current step; the launcher reads it

Steps (plan 2026-11-15-conformidade-legal.md, Fase 2):
  1. verify    every file of the supported revision by name, size, CRC32, SHA-1 and SHA-256; then the program image
  2. stage     link (or copy, across volumes) the verified files into roms/<game>/ (ignored by Git)
  3. decoder   compile the TMS34010 decoder from the pinned MAME sources (tools/setup/decoder.py)
  4. generate  translate the program image into C++ in generated/<game>/<program_sha256>/ (ignored by Git);
               MK2 also writes its factory CMOS seed there (the game's own factory boot, generated executor)
  5. accept    compare the generated output with the acceptance digests (tools/acceptance/)
  6. compile   build the game executable from the generated units with the project toolchain
  7. seed      MK3: the built executable runs the program's factory-settings restore from a cleared CMOS (the CMOS seed)
  8. install   the launcher package of the game from the executable just built (tools/setup/install.py)
The receipt (setup.json in the output folder) keeps only identities, digests, counts and timings.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,sys,threading,time,traceback

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.setup import paths

ROOT=paths.ROOT
GAMES=('mk1','mk2','mk3')
ACCEPTED=ROOT/'tools/acceptance/generated.json'
VALIDATED_CC='16.1.0'   # g++ version that validated the routes (docs/PUBLISHING.md)
# Raised when a release changes what the setup builds: older records then ask for a new build (launcher_setup.hpp
# kSetupVersion, recomplauncher.json prepare.version).
SETUP_VERSION='14'
MIN_PYTHON=(3,12)       # Path.is_junction (link guards of the setup, the package writers and temporary_tree)
STEPS=('verify','stage','decoder','generate','accept','compile','seed','install')
PROGRESS=dict(file=None,started=0.0,state={},lock=threading.Lock())   # lock: the two compile workers report units


def progress(step=None,**fields):
    """Current step of the setup in the --progress file (best effort: a reader holding the file never fails the setup)."""
    with PROGRESS['lock']:
        state=PROGRESS['state']
        if step:state.update(step=step,units=None)
        state.update(fields,elapsed=round(time.perf_counter()-PROGRESS['started'],1))
        file=PROGRESS['file']
        if not file:return
        for _ in range(20):
            try:write_text(file.parent,file.name,json.dumps(state)+'\n');return
            except PermissionError:time.sleep(.05)


def units(done,total):progress(units=dict(done=done,total=total))


class SetupError(Exception):
    """A failure the launcher shows as written: `code` (launcher_main.cpp failureLine), one plain sentence and a hint.
    `detail`: one technical line (first compiler error, failing check) for the log and the launcher's Copy report."""
    def __init__(self,code,message,hint='',detail=''):
        super().__init__(message);self.code,self.message,self.hint,self.detail=code,message,hint,detail


DOING=dict(verify='checking the ROM files',stage='copying the ROM files',decoder='building the decoder',
           generate='generating the C++',accept='checking the generated C++',compile='compiling the game',
           seed='creating the factory settings',install='installing the game')
REPORT_HINT='Open the log for details, or use Copy report and send it to the project.'
DISK_HINT='Free some space on the drive of this folder and build again.'


def classify(error,step):
    """Any exception of a setup run as a SetupError (KeyboardInterrupt and the like stay as they are)."""
    if isinstance(error,SetupError):return error
    if not isinstance(error,Exception) and not (isinstance(error,SystemExit) and isinstance(error.code,str)):return error
    text=str(error).strip()
    if isinstance(error,OSError) and (error.errno==28 or getattr(error,'winerror',None) in (39,112)):
        return SetupError('disk_full','The disk is full.',DISK_HINT,text)
    if step=='verify':
        return SetupError('rom_mismatch','Your ROM files do not match the supported revision.',
                          'Run Verify on the ROM Setup page to see which file is missing or different.',text)
    return SetupError('internal',f'Something went wrong while {DOING.get(step,"preparing the game")}.',REPORT_HINT,
                      text.splitlines()[0][:300] if text else type(error).__name__)


def build_failed(built):
    """SetupError for a failed build result (cpp_build / q.command); the whole compiler output goes to the log."""
    output=built.get('stdout') or built.get('output_tail') or ''
    print(output,file=sys.stderr)
    first=next((line.strip() for line in output.splitlines()
                if 'error:' in line or 'undefined reference' in line or 'No space left' in line),'')
    version=compiler_version();tested=f'g++ {version}'+('' if version==VALIDATED_CC else f' (tested: {VALIDATED_CC})')
    if 'No space left' in output:return SetupError('disk_full','The disk is full.',DISK_HINT,first)
    if built.get('timed_out'):
        return SetupError('timed_out',f'The build with {tested} took longer than the time limit.',
                          'Close other heavy programs and build again.',first)
    hint=('This compiler is not the tested version. ' if version!=VALIDATED_CC else '')+REPORT_HINT
    if 'ld returned' in output or 'undefined reference' in output:
        return SetupError('link_failed',f'The game could not be linked with {tested}.',hint,first)
    return SetupError('compile_failed',f'The game could not be compiled with {tested}.',hint,first)


def compiler_version():
    import subprocess
    from tools.quality import run as q
    if not q.DEFAULT_CC.is_file():return None
    try:return subprocess.run([str(q.DEFAULT_CC),'-dumpfullversion'],capture_output=True,text=True,timeout=30).stdout.strip() or None
    except (OSError,subprocess.SubprocessError):return None


def requirements():
    """What the setup needs on this machine: 64-bit Windows, this Python and the validated g++."""
    import platform
    from tools.quality import run as q
    version=compiler_version()
    found=dict(windows_64bit=sys.platform=='win32' and platform.machine().endswith('64'),python=platform.python_version(),
               compiler=dict(path=q.DEFAULT_CC.as_posix(),version=version,validated=VALIDATED_CC))
    found['python_supported']=sys.version_info>=MIN_PYTHON
    found['ok']=found['windows_64bit'] and found['python_supported'] and version==VALIDATED_CC
    return found


def report(game,folder):
    """Every file of the manifest checked on its own (no stop at the first error), then the program image by the game's
    own verify. Reads only; status per file: ok, missing, link, or the first check that differs."""
    from tools.mk3.roms import digests
    manifest=json.loads(paths.manifest(game).read_text('utf-8'));folder=Path(folder);files=[]
    for rom in manifest['roms']:
        path=folder/rom['path'];status='ok'
        if path.is_symlink():status='link'
        elif not path.is_file():status='missing'
        elif path.stat().st_size!=rom['size']:status='size'
        else:
            seen=digests(path.read_bytes())
            status=next((k for k in ('crc32','sha1','sha256') if k in rom and seen[k]!=rom[k]),'ok')
        files.append(dict(name=rom['path'],status=status))
    out=dict(game=game,revision=manifest['revision'],checks=[k for k in ('size','crc32','sha1','sha256')
             if all(k in r for r in manifest['roms'])],files=files,program='not checked')
    if all(f['status']=='ok' for f in files):
        try:
            if game=='mk1':from tools.mk1.roms import verify;verify(folder)
            elif game=='mk2':from tools.mk2.run import verify_roms;verify_roms(folder)
            else:from tools.mk3.roms import verify;verify(folder)
            out['program']='ok'
        except (ValueError,SystemExit,OSError) as error:out['program']=str(error)
    out['ok']=out['program']=='ok'
    return out


SUBFOLDERS={'mk1':('mk1','mk'),'mk2':('mk2',),'mk3':('mk3',)}   # roms/<game>/ (the staging layout), then the MAME set name


def locate(game,folder):
    """The folder that holds the game's files: `folder` itself or its subfolder named after the game, so one folder with
    a subfolder per game serves every game. The candidate with the most manifest files wins; ties keep `folder`."""
    folder=Path(folder);names=[r['path'] for r in json.loads(paths.manifest(game).read_text('utf-8'))['roms']]
    best,most=folder,-1
    for candidate in (folder,*(folder/s for s in SUBFOLDERS[game])):
        if candidate.is_symlink() or getattr(candidate,'is_junction',lambda:False)() or not candidate.is_dir():continue
        found=sum((candidate/n).is_file() for n in names)
        if found>most:best,most=candidate,found
    return best


def units_digest(texts):
    """The digest generate.py --check records: name, length and text of every unit, sorted by name."""
    return hashlib.sha256(''.join(f'{n}|{len(t)}|{t}' for n,t in sorted(texts.items())).encode()).hexdigest()


def stage(game,source,names):
    """Link the verified files into roms/<game>/ unless `source` already is that folder. Never overwrites a different file."""
    target=ROOT/'roms'/game;source=Path(source).resolve()
    if source==target.resolve():return dict(staged=0,folder=f'roms/{game}')
    target.mkdir(parents=True,exist_ok=True);done=0
    for name in names:
        src,dst=source/name,target/name
        if dst.exists():
            if dst.stat().st_size==src.stat().st_size and _sha(dst)==_sha(src):continue
            raise SetupError('rom_conflict',f'roms/{game}/{name} already exists with other contents.',
                             'Move that file out of the roms folder and build again.')
        try:os.link(src,dst)
        except OSError:shutil.copy2(src,dst)
        done+=1
    return dict(staged=done,folder=f'roms/{game}')


def _sha(path):
    from tools.quality import run as q
    return q.sha(Path(path))


def write_text(folder,name,text):
    path=folder/name;partial=folder/(name+'.partial')
    partial.write_text(text,encoding='utf-8',newline='\n');os.replace(partial,path);return path


def accept(game,files):
    """files: {logical name: sha256}. Returns the names whose digest differs from tools/acceptance (absent = not pinned)."""
    pinned=json.loads(ACCEPTED.read_text('utf-8')).get(game,{}) if ACCEPTED.is_file() else {}
    legacy=json.loads((ROOT/'tools/acceptance/manifest.json').read_text('utf-8'))['files']
    out={}
    for name,digest in files.items():
        want=pinned.get(name) or (legacy.get(name) or {}).get('sha256')
        out[name]='not pinned' if want is None else ('equal' if want==digest else 'DIFFERS')
    return out


def toolchain():
    """Identity of the compiler the generated C++ is built with (validated toolchain; see docs/PUBLISHING.md)."""
    from tools.quality import run as q
    import subprocess
    cc=q.DEFAULT_CC
    if not cc.is_file():
        raise SetupError('compiler_missing','The C++ compiler (g++) was not found.',
                         f'Install MSYS2 and its MinGW-w64 g++ (tested: {VALIDATED_CC}); the README has the steps.',str(cc))
    version=subprocess.run([str(cc),'--version'],capture_output=True,text=True).stdout.splitlines()[0]
    return dict(compiler=cc.name,version=version,sha256=_sha(cc))


def compile_inputs(args,generated):
    """Digest of everything a game's compile reads: each file under native/, the generated files, the compiler command
    (flags, sources, libraries), the compiler itself and the warning policy. Equal digest: the previous build is reused."""
    from tools.quality import run as q
    files=[(f.relative_to(ROOT).as_posix(),q.sha(f)) for f in sorted((ROOT/'native').rglob('*')) if f.is_file()]
    files+=[(Path(f).name,q.sha(Path(f))) for f in sorted(generated)]
    text=json.dumps(dict(files=files,args=[str(a) for a in args],compiler=q.sha(q.DEFAULT_CC),werror=q.WARNINGS_AS_ERRORS))
    return hashlib.sha256(text.encode()).hexdigest()


def previous(game,key,exe):
    """The last receipt of this game when its compile read the same inputs and its executable is still that build."""
    try:old=json.loads((paths.output(game)/'setup.json').read_text('utf-8'))
    except (OSError,ValueError):return None
    built=old.get('compile') or {}
    return old if built.get('inputs')==key and Path(exe).is_file() and _sha(exe)==built.get('sha256') else None


def kept_seed(old):
    """The seed the previous run of the same build wrote, when the file still holds it."""
    seed=(old or {}).get('seed') or {}
    file=ROOT/seed.get('file','-')
    return dict(seed,reused=True) if seed.get('sha256') and file.is_file() and _sha(file)==seed['sha256'] else None


def reused_units(units_dir,marker,pinned):
    """MK3 units already generated for this program when they still hash to the pinned acceptance digest."""
    try:old=json.loads(marker.read_text('utf-8'))
    except (OSError,ValueError):return None
    if not pinned or old.get('units_sha256')!=pinned or not units_dir.is_dir():return None
    texts={f.name:f.read_text('utf-8') for f in units_dir.iterdir() if f.is_file()}
    return dict(old,reused=True) if len(texts)==old.get('units') and units_digest(texts)==pinned else None


def setup_mk3(roms,compile_exe):
    from tools.mk3 import roms as mk3_roms,generate,preview
    from tools.setup import decoder,mk3
    receipt=dict(game='mk3',toolchain=toolchain());t=time.perf_counter()
    manifest=json.loads(mk3_roms.MANIFEST.read_text('utf-8'))
    progress('verify');receipt['verify']=mk3_roms.verify(roms,manifest)
    progress('stage');receipt['stage']=stage('mk3',roms,[r['path'] for r in manifest['roms']])
    progress('decoder');receipt['decoder']=decoder.tms34010().name
    progress('generate');out=paths.output('mk3');units_dir=out/'tms';marker=out/'tms.json'
    pinned=(json.loads(ACCEPTED.read_text('utf-8')).get('mk3',{}) if ACCEPTED.is_file() else {}).get('mk3/tms_units')
    generated=reused_units(units_dir,marker,pinned)
    if generated is None:
        marker.unlink(missing_ok=True)
        if units_dir.exists():shutil.rmtree(units_dir)
        units_dir.mkdir(parents=True)
        if not paths.owned(units_dir):raise RuntimeError('generated/ is linked outside the project')
        rom=mk3_roms.program_image(ROOT/'roms/mk3',mk3_roms.definition())
        rows,proof=generate.scan(rom);texts,generation=generate.units(rom,rows)
        for name,text in texts.items():write_text(units_dir,name,text)
        generated=dict(units=len(texts),units_sha256=units_digest(texts),cases=generation['cases'],
                       candidate_pcs=generation['candidate_pcs'],scan={k:proof[k] for k in ('positions','truncated')})
        write_text(out,marker.name,json.dumps(generated,indent=1)+'\n')
    adsp=mk3.adsp_dir()
    receipt['generate']=dict(generated,adsp=sorted(mk3.ADSP_INCS))
    files={'mk3/tms_units':receipt['generate']['units_sha256'],**{f'native/mk3/{n}':_sha(adsp/n) for n in mk3.ADSP_INCS}}
    progress('accept');receipt['accept']=accept('mk3',files)
    receipt['generate_seconds']=round(time.perf_counter()-t,1)
    if compile_exe:
        from tools.quality import cpp_build,run as q
        progress('compile');t=time.perf_counter();scratch=out/'build'
        flags=[*generate.FLAGS,'-I'+str(ROOT/'native/include'),'-I'+str(ROOT/'native/mk3'),'-I'+str(ROOT/'native/mk2'),mk3.include()]
        sources=[ROOT/p for p in preview.SOURCES]+[ROOT/'native/mk3'/n for n in generate.RUNTIME]+sorted(units_dir.glob('*.cpp'))
        key=compile_inputs([*flags,*sources,*preview.LIBRARIES],[*units_dir.glob('*'),*(adsp/n for n in mk3.ADSP_INCS)])
        old=previous('mk3',key,out/'mk3.exe')
        if old:receipt['compile']=dict(old['compile'],reused=True)
        else:
            if scratch.exists():shutil.rmtree(scratch)
            scratch.mkdir()
            try:
                cc=q.DEFAULT_CC;env={'PATH':str(cc.parent)+os.pathsep+os.environ['PATH']}
                built=cpp_build.build(cc,flags,sources,preview.LIBRARIES,scratch/'mk3.exe',scratch,env=env,timeout=3600,compile_from_scratch=True)
                if not built['passed']:raise build_failed(built)
                os.replace(scratch/'mk3.exe',out/'mk3.exe')
            finally:shutil.rmtree(scratch,ignore_errors=True)
            receipt['compile']=dict(executable=str((out/'mk3.exe').relative_to(ROOT)).replace('\\','/'),sha256=_sha(out/'mk3.exe'),
                                    seconds=round(time.perf_counter()-t,1),workers=built.get('workers'),inputs=key)
        # The factory CMOS seed: the program's own restore from a cleared CMOS, run by the executable just built.
        progress('seed');receipt['seed']=kept_seed(old) or mk3.factory_seed(out/'mk3.exe')
        receipt['accept'].update(accept('mk3',{'native/runtime/mk3-factory-default-cmos.bin':receipt['seed']['sha256']}))
    return receipt


def setup_mk2(roms,compile_exe):
    from tools.mk2 import run as mk2_run,package_native
    from tools.setup import decoder,mk2
    receipt=dict(game='mk2',toolchain=toolchain());t=time.perf_counter()
    manifest=mk2_run.read_manifest()
    progress('verify');receipt['verify']=mk2_run.verify_roms(roms,manifest)
    progress('stage');receipt['stage']=stage('mk2',roms,[r['path'] for r in manifest['roms']])
    progress('decoder');receipt['decoder']=decoder.tms34010().name
    progress('generate');out=mk2.inc_dir(ROOT/'roms/mk2')
    files=mk2.files()
    receipt['generate']=dict(includes=len(files),files=dict(files))
    seed=mk2.factory_seed(ROOT/'roms/mk2')
    files[mk2.SEED_LOGICAL]=_sha(seed)
    receipt['generate']['factory_seed']=dict(file=str(seed.relative_to(ROOT)).replace('\\','/'),sha256=files[mk2.SEED_LOGICAL])
    progress('accept');receipt['accept']=accept('mk2',files)
    receipt['generate_seconds']=round(time.perf_counter()-t,1)
    if compile_exe:
        from tools.quality import run as q
        progress('compile');t=time.perf_counter();scratch=out/'build'
        args=[*package_native.FLAGS,'-static',*[ROOT/p for p in package_native.SOURCES],*package_native.LIBRARIES,'-Wl,--no-insert-timestamp']
        key=compile_inputs(args,[*out.glob('*.inc'),*out.glob('*.cpp')])
        old=previous('mk2',key,out/'mk2-native.exe')
        if old:receipt['compile']=dict(old['compile'],reused=True)
        else:
            if scratch.exists():shutil.rmtree(scratch)
            scratch.mkdir()
            try:
                env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ['PATH'],'TEMP':str(scratch),'TMP':str(scratch)}
                built=q.command([q.DEFAULT_CC,*args,'-o',scratch/'mk2-native.exe'],scratch,cwd=ROOT,env=env,timeout=3600)
                if not built['passed']:raise build_failed(built)
                os.replace(scratch/'mk2-native.exe',out/'mk2-native.exe')
            finally:shutil.rmtree(scratch,ignore_errors=True)
            receipt['compile']=dict(executable=str((out/'mk2-native.exe').relative_to(ROOT)).replace('\\','/'),
                                    sha256=_sha(out/'mk2-native.exe'),seconds=round(time.perf_counter()-t,1),
                                    workers=built.get('workers'),inputs=key)
    return receipt


def setup_mk1(roms,compile_exe):
    from tools.mk1 import roms as mk1_roms
    from tools.setup import decoder,mk1
    receipt=dict(game='mk1',toolchain=toolchain());t=time.perf_counter()
    manifest=mk1_roms.manifest()
    progress('verify');receipt['verify']=mk1_roms.verify(roms,manifest)
    progress('stage');receipt['stage']=stage('mk1',roms,[r['path'] for r in manifest['roms']])
    progress('decoder');receipt['decoder']=decoder.tms34010().name
    progress('generate');out=mk1.inc_dir(ROOT/'roms/mk1')
    files=mk1.files()
    receipt['generate']=dict(includes=len(files),files=files)
    progress('accept');receipt['accept']=accept('mk1',files)
    receipt['generate_seconds']=round(time.perf_counter()-t,1)
    if compile_exe:
        from tools.mk1 import build
        progress('compile');t=time.perf_counter();exe=out/'mk5-native-audio.exe'
        key=compile_inputs([*build.FLAGS,*build.sources(),*build.LIBRARIES],[*out.glob('*.inc'),*out.glob('*.cpp')])
        old=previous('mk1',key,exe)
        if old:receipt['compile']=dict(old['compile'],reused=True)
        else:
            built=build.engine(out)
            if not built['passed']:raise build_failed(built)
            receipt['compile']=dict(executable=str(exe.relative_to(ROOT)).replace('\\','/'),sha256=_sha(exe),
                                    seconds=round(time.perf_counter()-t,1),workers=built.get('workers'),inputs=key)
        # CMOS seed: the game's factory restore from a blank CMOS, run by the compiled engine (tools/setup/mk1.make_seed).
        progress('seed');t=time.perf_counter();receipt['seed']=kept_seed(old)
        if not receipt['seed']:
            seed,digest=mk1.make_seed(exe)
            receipt['seed']=dict(file=str(seed.relative_to(ROOT)).replace('\\','/'),sha256=digest,seconds=round(time.perf_counter()-t,1))
        receipt['accept'].update(accept('mk1',{'mk1/factory-cmos':receipt['seed']['sha256']}))
    return receipt


SETUP={'mk1':setup_mk1,'mk2':setup_mk2,'mk3':setup_mk3}


def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game',choices=GAMES)
    ap.add_argument('--roms',type=Path,help='folder with the ROM files of your own board')
    ap.add_argument('--no-compile',action='store_true',help='stop after generating and checking the C++')
    ap.add_argument('--verify-only',action='store_true',help='check every ROM file and report; nothing is written')
    ap.add_argument('--requirements',action='store_true',help='report the Windows, Python and compiler found')
    ap.add_argument('--progress',type=Path,help='JSON file kept at the current step (read by the launcher)')
    a=ap.parse_args(argv)
    if a.requirements:
        found=requirements();print(json.dumps(found,indent=1));return 0 if found['ok'] else 1
    if not a.game or not a.roms:ap.error('--game and --roms are required')
    if not a.roms.is_dir():ap.error(f'not a folder: {a.roms}')
    a.roms=locate(a.game,a.roms)
    if a.progress:PROGRESS['file']=a.progress.absolute();a.progress.parent.mkdir(parents=True,exist_ok=True)
    PROGRESS.update(started=time.perf_counter(),state=dict(game=a.game,mode='verify' if a.verify_only else 'setup',
                                                           steps=STEPS,state='running'))
    progress('verify')
    try:
        if sys.version_info<MIN_PYTHON:
            raise SetupError('python_old',f'Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]} or later is required; this PC has {sys.version.split()[0]}.',
                             'Install a current Python from python.org and build again.')
        if a.verify_only:
            found=report(a.game,a.roms);print(json.dumps(found,indent=1))
            if found['ok']:progress(state='done',report=found);return 0
            progress(state='failed',report=found,code='rom_mismatch',message='Some ROM files are missing or different.',
                     hint='The list on the ROM Setup page shows each file and its result.');return 1
        from tools.quality import cpp_build,run as q
        cpp_build.on_unit=units
        q.WARNINGS_AS_ERRORS=False   # a newer g++ adds warnings; the code it generates is the same (CI keeps -Werror)
        receipt=SETUP[a.game](a.roms,not a.no_compile)
        receipt['setup_version']=SETUP_VERSION
        out=paths.output(a.game);out.mkdir(parents=True,exist_ok=True)
        (out/'setup.json').write_text(json.dumps(receipt,indent=1)+'\n',encoding='utf-8')
        bad=[k for k,v in receipt.get('accept',{}).items() if v=='DIFFERS']
        if bad:
            print(json.dumps(receipt,indent=1))
            raise SetupError('acceptance_differs','The generated C++ does not match the reference digests.',
                             'Your ROM files passed the check, so this is a problem in this release. '+REPORT_HINT,', '.join(bad))
        if 'compile' in receipt:
            # 8. install: the launcher package from the executable just built (tools/setup/install.py).
            from tools.setup import install
            progress('install');t=time.perf_counter()
            receipt['install']=dict(install.INSTALL[a.game]((ROOT/receipt['compile']['executable']).parent),seconds=round(time.perf_counter()-t,1))
            if receipt['install']['executable_sha256']!=receipt['compile']['sha256']:raise RuntimeError('installed executable differs from the build')
            (out/'setup.json').write_text(json.dumps(receipt,indent=1)+'\n',encoding='utf-8')
        print(json.dumps(receipt,indent=1))
        progress(state='done');return 0
    except BaseException as error:
        failure=classify(error,PROGRESS['state'].get('step'))
        if not isinstance(failure,SetupError):progress(state='failed',code='cancelled',message='Cancelled.');raise
        if failure is not error:traceback.print_exc()
        # Last lines of logs/setup-<game>.log: the launcher's Copy report takes its tail.
        print(f'\nSetup failed [{failure.code}] while {DOING.get(PROGRESS["state"].get("step"),"preparing")}: {failure.message}'+
              (f'\n{failure.detail}' if failure.detail else ''),file=sys.stderr)
        progress(state='failed',code=failure.code,message=failure.message,hint=failure.hint,detail=failure.detail);return 1


if __name__=='__main__':raise SystemExit(main())
