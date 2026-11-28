"""MK2 save states, Original and Remastered with music: save at a live chunk, replay m chunks after a load with identical
rasters, presentation layers, ticks, DAC bits, host PCM and state."""
import json,math,os,struct,sys,unittest,wave
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.mk2 import profile,run
from tools.quality import run as q

class SaveStateTests(unittest.TestCase):
    @unittest.skipUnless((q.ROOT/'roms/mk2').is_dir(),'requires authenticated ROMs')
    def test_load_replays_every_raster_DAC_sample_and_state_in_and_across_sessions(self):
        identities=run.verify_roms(q.ROOT/'roms/mk2')
        sources=[p for p in run.SOURCES if p!='native/tests/mk2_foundation_tests.cpp']+['native/mk2/mk2_music.cpp','native/tests/mk2_save_state_tests.cpp']
        with q.scratch_tree()as(scratch,cleanup):
            exe=scratch/'save-state.exe';env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ.get('PATH','')}
            build=q.command([q.DEFAULT_CC,'-std=c++17','-O1','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+str(q.ROOT/'native/include'),
                *[q.ROOT/p for p in sources],'-lbcrypt','-o',exe],scratch,env=env,timeout=240)
            self.assertTrue(build['passed'],build['stdout'])
            # Synthetic tones stand in for the user's music; they are written under scratch and removed with it.
            result=q.command([exe,q.ROOT/'roms/mk2',profile.FACTORY_SEED,scratch/'music'],scratch,env=env,timeout=600)
            self.assertTrue(result['passed'],result['stdout'])
            print(result['stdout'],end='')
            self.assertEqual(identities,run.verify_roms(q.ROOT/'roms/mk2'))
        self.assertFalse(cleanup['exists_after'])


def write_tracks(folder,gain_db):
    """Synthetic user tracks (tones, 48 kHz stereo, 2 s, repeat window), written under the test scratch."""
    folder.mkdir(parents=True)
    for slot in range(1,9):
        with wave.open(str(folder/f'music-{slot:02d}.wav'),'wb') as w:
            w.setnchannels(2);w.setsampwidth(2);w.setframerate(48000)
            w.writeframes(b''.join(struct.pack('<hh',*(int(6000*math.sin(n*(0.01+0.002*(slot+c)))) for c in (0,1)))
                                   for n in range(96000)))
    slots={f'music-{s:02d}':{'loop_begin':0.5,'loop_end':1.5,'gain_db':gain_db} for s in range(1,9)}
    (folder/'slots.json').write_text(json.dumps({'slots':slots}))


class WindowStateTests(unittest.TestCase):
    """F5/F7 through the live loop of mk2_native_check (the path the game window drives), with the test window: the
    request queue, the slot file under saves/mk2/<program sha>/, a load from memory, and a load of that file by a new
    session, in the Original edition and in the Remastered edition with music (--audio into a silent test sink, so the
    PCM accounting across a load is checked too). A Remastered slot is refused with other music tracks. CPU, DSP and
    CMOS are the real native implementation; no GPU or audio device."""
    @unittest.skipUnless((q.ROOT/'roms/mk2').is_dir(),'requires authenticated ROMs')
    def test_live_loop_saves_loads_and_reloads_the_slot_in_a_new_session(self):
        run.verify_roms(q.ROOT/'roms/mk2')
        sources=[q.ROOT/p for p in run.SOURCES if p!='native/tests/mk2_foundation_tests.cpp']
        sources+=[q.ROOT/p for p in ('native/mk2/mk2_native_check.cpp','native/mk2/mk2_music.cpp','native/tests/mk2_test_audio_sink.cpp',
                                    'native/tests/mk2_host_close_window.cpp')]
        with q.scratch_tree()as(scratch,cleanup):
            exe=scratch/'state-window.exe';env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ.get('PATH','')}
            build=q.command([q.DEFAULT_CC,'-std=c++17','-O1','-municode','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+str(q.ROOT/'native/include'),*sources,
                '-lbcrypt','-luser32','-lgdi32','-lwinmm','-o',exe],scratch,env=env,timeout=300)
            self.assertTrue(build['passed'],q.compact(build))
            profile.prepare(scratch/'profile')
            write_tracks(scratch/'tracks',0);write_tracks(scratch/'other-tracks',-1)
            argv=[exe,q.ROOT/'roms/mk2','--window','--host-controls','--through','cmos-live','--profile',scratch/'profile']
            def session(plan,samples,tracks=None,wide=False):
                extra={'MK1_MUSIC_MODE':'remastered','MK1_MUSIC_DIRECTORY':str(scratch/tracks)}if tracks else{}
                if wide:extra.update(MK2_WIDESCREEN='1',MK2_TEST_PRESENT_HASHES='1')
                result=q.command(argv+(['--audio']if tracks else[]),scratch,cwd=scratch,timeout=120,output_limit=1<<20,
                                 env=dict(env,MK2_TEST_STATE=plan,MK2_TEST_CLOSE_AFTER_SAMPLES=str(samples),**extra))
                out=result['stdout'];lines=[l[:300] for l in out.splitlines() if 'cmos_hex' not in l]
                # The session ends at the profile guard: CMOS is committed only after a real D3D12 presentation, which
                # the test window does not claim. Any other failure (a state error or PCM accounting included) fails.
                self.assertFalse(result['timed_out'],lines)
                self.assertEqual((result['exit_code'],'native profile requires unambiguous presentation metrics' in out),(1,True),lines)
                return out
            for name,tracks in (('slot1.state',None),('slot1-remastered.state','tracks')):
                first=session('save@40,load@80',120,tracks)
                slot=next((scratch/'saves/mk2').glob('*/'+name))
                self.assertIn('MK2 state saved: saves/mk2/',first)
                self.assertIn('MK2 state loaded from memory',first)
                self.assertGreater(slot.stat().st_size,1<<20)
                second=session('load@10',60,tracks)
                self.assertIn('MK2 state loaded from saves/mk2/',second)
                self.assertNotIn('not loaded',second)
            # Widescreen (W4): the Remastered slot saved without it loads into a widescreen session (its wide layer starts
            # over from VRAM); a widescreen session saves and loads its own; a session without widescreen loads that one too.
            self.assertIn('MK2 state loaded from saves/mk2/',session('load@10',60,'tracks',wide=True))
            wide=session('save@40,load@80',120,'tracks',wide=True)
            self.assertIn('MK2 state saved: saves/mk2/',wide);self.assertIn('MK2 state loaded from memory',wide)
            # With a constant input the frames after the load repeat, over all their columns, the frames after the save.
            rows=wide.splitlines();after=lambda mark:[json.loads(l)['all'] for l in rows[next(i for i,l in enumerate(rows) if mark in l):]
                                                     if l.startswith('{"present"')][:30]
            self.assertEqual(after('MK2 state loaded from memory'),after('MK2 state saved'),'a widescreen load does not resume the saved frames')
            self.assertEqual(len(after('MK2 state saved')),30)
            for widescreen in (True,False):
                again=session('load@10',60,'tracks',wide=widescreen)
                self.assertIn('MK2 state loaded from saves/mk2/',again);self.assertNotIn('not loaded',again)
            retracked=session('load@10',60,'other-tracks')
            self.assertIn('MK2 save state not loaded: MK2 save state was made with other music tracks',retracked)
        self.assertFalse(cleanup['exists_after'])

def shown_ahead(test,off,on):
    """`on` (run-ahead) must show the frames of `off` one ahead, after the frames both show before run-ahead starts."""
    test.assertGreater(len(on),100,len(on))
    k=next(i for i,(a,b) in enumerate(zip(on,off)) if a!=b)
    n=min(len(on),len(off)-1)
    test.assertGreaterEqual(n-k,50,'too few frames after run-ahead started')
    test.assertEqual(on[k:n],off[k+1:n+1],'a frame shown with run-ahead is not the next real frame')
    return n-k


def lines(out,key):
    return [json.loads(l)[key] if key=='present' else json.loads(l) for l in out.splitlines() if l.startswith('{"'+key+'"')]


def same_pcm(test,off,on):
    """The PCM delivered with run-ahead is the PCM without it, at every checkpoint both sessions reached."""
    a={p['at']:p['pcm'] for p in lines(off,'pcm')};b={p['at']:p['pcm'] for p in lines(on,'pcm')}
    common=sorted(set(a)&set(b));test.assertGreaterEqual(len(common),5,common)
    test.assertEqual([a[k] for k in common],[b[k] for k in common],'run-ahead changed the delivered PCM')
    return len(common)


class RunAheadTests(unittest.TestCase):
    """Run-ahead (F6) through the live loop with the test window and a constant input, Original, Remastered with music and
    Remastered widescreen (its 534 columns):
    every frame shown with run-ahead is the next real frame of a session without it, and the PCM delivered is the same."""
    @unittest.skipUnless((q.ROOT/'roms/mk2').is_dir(),'requires authenticated ROMs')
    def test_shows_the_next_real_frame_and_keeps_the_audio(self):
        run.verify_roms(q.ROOT/'roms/mk2')
        sources=[q.ROOT/p for p in run.SOURCES if p!='native/tests/mk2_foundation_tests.cpp']
        sources+=[q.ROOT/p for p in ('native/mk2/mk2_native_check.cpp','native/mk2/mk2_music.cpp','native/tests/mk2_test_audio_sink.cpp',
                                    'native/tests/mk2_host_close_window.cpp')]
        with q.scratch_tree()as(scratch,cleanup):
            exe=scratch/'run-ahead.exe';env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ.get('PATH','')}
            build=q.command([q.DEFAULT_CC,'-std=c++17','-O1','-municode','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+str(q.ROOT/'native/include'),*sources,
                '-lbcrypt','-luser32','-lgdi32','-lwinmm','-o',exe],scratch,env=env,timeout=300)
            self.assertTrue(build['passed'],q.compact(build))
            profile.prepare(scratch/'profile');write_tracks(scratch/'tracks',0)
            argv=[exe,q.ROOT/'roms/mk2','--window','--host-controls','--through','cmos-live','--profile',scratch/'profile','--audio']
            remastered={'MK1_MUSIC_MODE':'remastered','MK1_MUSIC_DIRECTORY':str(scratch/'tracks')}
            for edition,extra in (('original',{'MK1_MUSIC_MODE':'original'}),('remastered',remastered),
                                  ('widescreen',dict(remastered,MK2_WIDESCREEN='1'))):
                def session(ahead):
                    flags=dict(extra,MK2_TEST_PRESENT_HASHES='1',MK2_TEST_PCM_HASHES='1',MK2_TEST_CLOSE_AFTER_SAMPLES='400')
                    if ahead:flags['MK2_TEST_RUN_AHEAD']='1'
                    result=q.command(argv,scratch,cwd=scratch,timeout=300,output_limit=4<<20,env=dict(env,**flags))
                    out=result['stdout']
                    # As WindowStateTests: the session ends at the profile guard of the test window, after the live loop.
                    self.assertEqual((result['exit_code'],'native profile requires unambiguous presentation metrics' in out),(1,True),
                                     [l[:300] for l in out.splitlines() if 'cmos_hex' not in l][-20:])
                    return out
                off,on=session(False),session(True)
                # All the columns (a widescreen frame's 534 included) of each shown frame and its backdrop.
                shown=lambda out:[json.loads(l)['all'] for l in out.splitlines() if l.startswith('{"present"')]
                frames=shown_ahead(self,shown(off),shown(on));checkpoints=same_pcm(self,off,on)
                print(edition,'frames compared',frames,'PCM checkpoints',checkpoints)
        self.assertFalse(cleanup['exists_after'])



class TrainingTests(unittest.TestCase):
    """Training mode (F8) over the translated first-fight route with the test window: the frames shown are the frames
    without it (the overlay only reads RAM), and the fight frames carry both fighters' body boxes from the strike rule."""
    @unittest.skipUnless((q.ROOT/'roms/mk2').is_dir(),'requires authenticated ROMs')
    def test_boxes_read_only_and_on_the_fighters(self):
        run.verify_roms(q.ROOT/'roms/mk2')
        sources=[q.ROOT/p for p in run.SOURCES if p!='native/tests/mk2_foundation_tests.cpp']
        sources+=[q.ROOT/p for p in ('native/mk2/mk2_native_check.cpp','native/mk2/mk2_music.cpp','native/tests/mk2_test_audio_sink.cpp',
                                    'native/tests/mk2_host_close_window.cpp')]
        with q.scratch_tree()as(scratch,cleanup):
            exe=scratch/'training.exe';env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ.get('PATH','')}
            build=q.command([q.DEFAULT_CC,'-std=c++17','-O1','-municode','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+str(q.ROOT/'native/include'),*sources,
                '-lbcrypt','-luser32','-lgdi32','-lwinmm','-o',exe],scratch,env=env,timeout=300)
            self.assertTrue(build['passed'],q.compact(build))
            def session(training):
                flags=dict(MK2_TEST_PRESENT_HASHES='1')
                if training:flags['MK2_TEST_TRAINING']='1'
                result=q.command([exe,q.ROOT/'roms/mk2','--window','--through','first-fight'],scratch,cwd=scratch,timeout=600,
                                 output_limit=8<<20,env=dict(env,**flags))
                return [json.loads(l) for l in result['stdout'].splitlines() if l.startswith('{"present"')]
            off,on=session(False),session(True)
            self.assertGreater(len(off),100,len(off))
            self.assertEqual([f['present'] for f in on],[f['present'] for f in off],'training mode changed a shown frame')
            self.assertTrue(all(not f['boxes'] for f in off),'boxes without training mode')
            fight=[f['boxes'] for f in on if sum(b[4]==0 for b in f['boxes'])==2]
            self.assertGreater(len(fight),20,'no fight frames with both body boxes')
            for boxes in fight:
                for x,y,w,h,kind in boxes:self.assertTrue(0<w<400 and 0<h<254 and -400<x<800 and -254<y<508,boxes)
            print('frames',len(on),'fight frames',len(fight),'strikes',sum(b[4]==1 for f in on for b in f['boxes']))
        self.assertFalse(cleanup['exists_after'])



class WideCaptureTests(unittest.TestCase):
    """Widescreen over the translated first-fight route: the 16:9 (534-column) capture and the shadow pass that composes it leave
    the frames shown unchanged, and the composed frame's visible centre is the scanout's own frame in every frame whose wide
    camera is the arcade's (in a fight the far planes move at an even pace: mk5wide::Layer::even, docs/WIDESCREEN.md)."""
    @unittest.skipUnless((q.ROOT/'roms/mk2').is_dir(),'requires authenticated ROMs')
    def test_centre_is_the_frame_and_capture_is_read_only(self):
        run.verify_roms(q.ROOT/'roms/mk2')
        sources=[q.ROOT/p for p in run.SOURCES if p!='native/tests/mk2_foundation_tests.cpp']
        sources+=[q.ROOT/p for p in ('native/mk2/mk2_native_check.cpp','native/mk2/mk2_music.cpp','native/tests/mk2_test_audio_sink.cpp',
                                    'native/tests/mk2_host_close_window.cpp')]
        with q.scratch_tree()as(scratch,cleanup):
            exe=scratch/'wide.exe';env={'PATH':str(q.DEFAULT_CC.parent)+os.pathsep+os.environ.get('PATH','')}
            build=q.command([q.DEFAULT_CC,'-std=c++17','-O1','-municode','-pthread','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+str(q.ROOT/'native/include'),*sources,
                '-lbcrypt','-luser32','-lgdi32','-lwinmm','-o',exe],scratch,env=env,timeout=300)
            self.assertTrue(build['passed'],q.compact(build))
            def session(wide,**extra):
                flags=dict(MK2_TEST_PRESENT_HASHES='1',**extra)
                if wide:flags['MK2_WIDE']='1'
                result=q.command([exe,q.ROOT/'roms/mk2','--window','--through','first-fight'],scratch,cwd=scratch,timeout=600,
                                 output_limit=8<<20,env=dict(env,**flags))
                return result['stdout'],result['wall_seconds']
            (off,off_s),(on,on_s)=session(False),session(True)
            self.assertEqual(lines(on,'present'),lines(off,'present'),'the capture changed a shown frame')
            wide=lines(on,'wide');self.assertGreater(len(wide),100,len(wide))
            # The composed frame's visible centre is the arcade's frame in every frame whose world comes from the real draws.
            same=[w for w in wide if not w['whole']];self.assertGreater(len(same),100,len(same))
            self.assertEqual([(w['wide'],w['centre_diff']) for w in same if w['centre']!=w['plain']][:8],[],'the visible centre differs from the frame')
            self.assertEqual(len(wide),len(lines(on,'present')),'one capture per shown frame')
            # The shadow composes all 534 columns: no fault, margin pixels beyond the arcade's own spill in the fights, and
            # every margin pixel from a draw of the game meant to cover its column.
            self.assertEqual(wide[-1]['faults'],0,'a shadow pass was left incomplete')
            self.assertGreater(sum(w['added']>0 for w in wide),300,'the shadow added no margin art')
            self.assertEqual(sum(w['outside']+w['generated'] for w in wide),0,'a margin pixel of no draw meant for its column')
            print('seconds without/with',round(off_s,2),round(on_s,2),'frames',len(wide),'masked (quartiles)',sorted(w['masked'] for w in wide)[::len(wide)//4],
                  'margin pixels added (quartiles)',sorted(w['added'] for w in wide)[::len(wide)//4])
            # Widescreen presentation (Remastered, MK2_WIDESCREEN=1): 534-column frames whose centre is the Remastered frame
            # (pixels and backdrop) in every frame whose world comes from the real draws.
            (plain,_),(widescreen,_)=session(False,MK1_MUSIC_MODE='remastered'),session(True,MK1_MUSIC_MODE='remastered',MK2_WIDESCREEN='1')
            frames=lambda out:[json.loads(l) for l in out.splitlines() if l.startswith('{"present"')]
            plain,shown,layer=frames(plain),frames(widescreen),lines(widescreen,'wide')
            self.assertEqual((len(shown),len(layer)),(len(plain),len(plain)))
            self.assertEqual({(f['width']) for f in shown},{534});self.assertEqual({f['width'] for f in plain},{400})
            exact=[n for n,w in enumerate(layer) if not w['whole']];self.assertGreater(len(exact),100,len(exact))
            self.assertEqual([shown[n]['present'] for n in exact],[plain[n]['present'] for n in exact],'the widescreen centre differs')
            self.assertEqual([f['boxes'] for f in shown],[f['boxes'] for f in plain])   # no boxes without training mode
        self.assertFalse(cleanup['exists_after'])


if __name__=='__main__':unittest.main()
