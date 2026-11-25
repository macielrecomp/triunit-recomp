"""MK3 save states (F5/F7) through the live loop of mk3.exe (native/mk3/mk3_preview_main.cpp), headless, with the legal
round start tape (tools/mk3/fight_route.tape) as the inputs. In each edition (Original; Remastered with its DMA layers;
Remastered with the widescreen layer, --wide):
- save at frame S, digest the state at frame E, load (from memory) and replay to E: every frame line of S+1..E (raster,
  previous frame, palette, machine time and cycle, host PCM, Remastered composite) and the state digest at E identical;
- a new process loads the slot file at an earlier frame and replays the same frames;
- refusals: a slot of the other edition and a damaged slot leave the session running;
- a Remastered slot saved without widescreen loads in a widescreen session and the other way round: the game goes on.
--state-test drives the same request queue that F5/F7 of the window feed. No window, GPU or audio device.
"""
from pathlib import Path
import shutil,sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.mk3 import preview

ROOT=preview.ROOT
TAPE=ROOT/'tools/mk3/fight_route.tape'
S,E,LOAD_AT=1000,2560,5   # across the tape's inputs (frames 1094-1646) and the round start (DCS 0x233D near frame 2480)


class SaveStateTests(unittest.TestCase):
    @unittest.skipUnless((ROOT/'roms/mk3').is_dir(),'requires authenticated ROMs')
    def test_load_replays_every_frame_PCM_and_state_in_and_across_sessions(self):
        with preview.Preview() as p:
            slots={}
            for edition,wide in (('original',()),('remastered',()),('remastered',('--wide',))):
                name=edition+('-wide' if wide else '');cwd=p.temp/name;cwd.mkdir()
                run=lambda plan,frames=E+1,where=cwd,wide=wide:p.run('--hashes','--tape',TAPE,'--edition',edition,'--frames',frames,
                                                                   '--state-test',plan,*wide,cwd=where,timeout=1800)
                a=run(f'save@{S},hash@{E},load@{E},hash@{E}')
                states=[(s['save_state'],s.get('frame'),s.get('from')) for s in a['states']]
                self.assertEqual(states,[('saved',S,None),('hash',E,None),('loaded',S,'memory'),('hash',E,None)],a['states'])
                self.assertTrue(a['states'][0]['file'])
                self.assertEqual(a['states'][1]['sha256'],a['states'][3]['sha256'],'state at E differs after the load')
                frames=a['frames'];self.assertEqual([f['frame'] for f in frames],[*range(1,E+1),*range(S+1,E+2)])
                first,replay=frames[S:E],frames[E:2*E-S]
                self.assertEqual(first,replay,'load changed a frame line')
                self.assertTrue(all('pcm' in f for f in first) and len({f['pcm'] for f in first})>1)
                if edition=='remastered':self.assertTrue(any(f.get('rm','0'*16)!='0'*16 for f in first),'no Remastered composite in range')
                if wide:self.assertTrue(all('wide' in f for f in first) and len({f['wide']['rgb'] for f in first})>1,'no wide frame in range')
                slot=next((cwd/'saves/mk3').glob('*/'+('slot1-remastered.state' if edition=='remastered' else 'slot1.state')))
                self.assertGreater(slot.stat().st_size,1<<20);slots[name]=slot
                b=run(f'load@{LOAD_AT}')
                self.assertEqual([(s['save_state'],s.get('from')) for s in b['states']],[('loaded','file')],b['states'])
                self.assertEqual(b['frames'][LOAD_AT:LOAD_AT+E-S],first,'a new session replayed other frames')
                print(name,'state bytes',a['states'][0]['bytes'],'frames compared',2*(E-S))
            # The Original slot offered to a Remastered session, then a damaged Remastered slot.
            cross=p.temp/'cross';target=cross/slots['remastered'].relative_to(p.temp/'remastered')
            target.parent.mkdir(parents=True);shutil.copyfile(slots['original'],target)
            refused=lambda:p.run('--hashes','--edition','remastered','--frames',LOAD_AT+2,'--state-test',f'load@{LOAD_AT}',cwd=cross)['states']
            self.assertEqual(refused(),[{'save_state':'refused','error':'MK3 save state belongs to another edition'}])
            data=bytearray(slots['remastered'].read_bytes());data[-1]^=1;target.write_bytes(data)
            self.assertEqual(refused(),[{'save_state':'refused','error':'MK3 save state payload is damaged'}])
            # With and without widescreen: either slot loads in the other session, whose frames then go on.
            for slot,extra in (('remastered',('--wide',)),('remastered-wide',())):
                shutil.copyfile(slots[slot],target)
                c=p.run('--hashes','--tape',TAPE,'--edition','remastered','--frames',S+60,'--state-test',f'load@{LOAD_AT}',*extra,cwd=cross,timeout=1800)
                self.assertEqual([(s['save_state'],s.get('from')) for s in c['states']],[('loaded','file')],c['states'])
                self.assertEqual(c['summary']['frontier'],'none');self.assertEqual(c['frames'][-1]['frame'],S+60)



class RunAheadTests(unittest.TestCase):
    """Run-ahead (F6), headless with the legal round start tape: every real frame line (raster, palette, machine time,
    host PCM, Remastered composite) is the same with and without --run-ahead, and each frame run ahead of a real one is
    the next real frame, in both editions."""
    @unittest.skipUnless((ROOT/'roms/mk3').is_dir(),'requires authenticated ROMs')
    def test_real_frames_unchanged_and_each_frame_ahead_is_the_next(self):
        with preview.Preview() as p:
            for edition,wide in (('original',()),('remastered',()),('remastered',('--wide',))):
                # --state-test adds the host PCM to every frame line in both runs; its one digest at E is the last line.
                run=lambda *extra:p.run('--hashes','--tape',TAPE,'--edition',edition,'--frames',E,'--state-test',f'hash@{E}',*wide,*extra,timeout=1800)
                off,on=run(),run('--run-ahead')
                self.assertEqual(on['frames'],off['frames'],'run-ahead changed a real frame line')
                self.assertTrue(all('pcm' in f for f in on['frames']) and len({f['pcm'] for f in on['frames']})>1)
                real={f['frame']:f['rgb'] for f in off['frames']}
                self.assertEqual([a['ahead'] for a in on['ahead']],list(range(2,E+1)))
                self.assertEqual([a['rgb'] for a in on['ahead']],[real[a['ahead']] for a in on['ahead']],'a frame ahead differs from the real one')
                print(edition,'frames compared',len(on['frames']),'frames ahead',len(on['ahead']))



class TrainingTests(unittest.TestCase):
    """Training mode (F8), headless with the legal round start tape into the fight, where the CPU attacks P1: every frame
    line is the same without --training (the boxes only read RAM and registers), the fight frames hold both fighters'
    body boxes around their objects, and the strike check reports attack boxes."""
    @unittest.skipUnless((ROOT/'roms/mk3').is_dir(),'requires authenticated ROMs')
    def test_boxes_read_only_and_on_the_fighters(self):
        frames=4000
        with preview.Preview() as p:
            run=lambda *extra:p.run('--hashes','--tape',TAPE,'--edition','original','--frames',frames,*extra,timeout=1800)['frames']
            off,on=run(),run('--training')
            self.assertEqual([{k:v for k,v in f.items() if k!='boxes'} for f in on],off,'training mode changed a frame line')
            fight=[f['boxes'] for f in on if sum(b[4]==0 for b in f['boxes'])==2]
            strikes=sum(b[4]==1 for f in on for b in f['boxes'])
            self.assertGreater(len(fight),300,'too few fight frames with both body boxes')
            self.assertGreater(strikes,0,'the strike check reported no attack box')
            for boxes in fight:
                for x,y,w,h,kind in boxes:self.assertTrue(0<w<400 and 0<h<254 and -400<x<800 and -254<y<508,boxes)
            print('frames',len(on),'fight frames',len(fight),'strikes',strikes)


if __name__=='__main__':unittest.main()
