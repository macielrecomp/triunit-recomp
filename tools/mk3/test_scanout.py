"""J1 reserve: Wolf-Unit scanout (native/mk3/mk3_scanout.hpp) over VideoTiming's scanline timer.

Scope: synthetic registers, video RAM and palette (native/tests/mk3_scanout_tests.cpp): two frames of the 506 x 289
raster, the visible 400 x 254 area compared pen by pen with expectations written from get_display_params and
midtunit scanline_update, a disabled display, the multi-line update_partial rule, Present() through xRGB_555 and three
negative controls. The Original comparison is tools/mk3/test_scanout_original.py.
"""
import json,os,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.quality import run as q

ROOT=Path(__file__).resolve().parents[2]
RECEIPT=ROOT/'build/native/mk3-m2/scanout.json'


class Scanout(unittest.TestCase):
    def test_scanout_rules(self):
        cc=q.DEFAULT_CC;env={'PATH':str(cc.parent)+os.pathsep+os.environ['PATH']}
        with q.scratch_tree() as (temp,cleanup):
            exe=temp/'mk3-scanout.exe'
            made=q.command([cc,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-I'+str(ROOT/'native/mk3'),'-I'+str(ROOT/'native/include'),
                            ROOT/'native/tests/mk3_scanout_tests.cpp','-o',exe],temp,cwd=ROOT,env=env,timeout=180)
            self.assertTrue(made['passed'],q.compact(made))
            run=q.command([exe],temp,cwd=ROOT,env=env,timeout=60)
            self.assertTrue(run['passed'],q.compact(run))
            result=json.loads(run['stdout'].strip().splitlines()[-1])
        self.assertFalse(cleanup['exists_after'])
        self.assertEqual(result,{'compared_pens':203200,'failures':0})
        RECEIPT.parent.mkdir(parents=True,exist_ok=True)
        RECEIPT.write_text(json.dumps(dict(passed=True,**result,negative_controls=3,
            timings_seconds=dict(build=made['wall_seconds'],run=run['wall_seconds']),
            scope='synthetic rule checks; Original frames: tools/mk3/test_scanout_original.py (j2-host.json)',cleanup=dict(exists_after=cleanup['exists_after'])),indent=1)+'\n',encoding='utf-8')


if __name__=='__main__':unittest.main()
