"""Compile and run the focused suatom lowering regression."""
import argparse, os, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cxx',default=shutil.which('clang++'))
    ap.add_argument('--out',type=Path,default=ROOT/'build/test/native-suatom')
    a=ap.parse_args()
    if not a.cxx: ap.error('pass --cxx or put clang++ on PATH')
    a.out.mkdir(parents=True,exist_ok=True); env=dict(os.environ)
    env['PATH']=str(Path(a.cxx).resolve().parent)+os.pathsep+env.get('PATH','')
    for opt in ('0','2'):
        exe=a.out.resolve()/('suatom-O'+opt+('.exe' if os.name=='nt' else ''))
        cmd=[a.cxx,'-std=c++20','-O'+opt,'-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime/third_party/eglantine'),str(ROOT/'tests/native_suatom_test.cpp'),'-o',str(exe)]
        build=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=120)
        (a.out/('compile-O'+opt+'.log')).write_text(build.stdout+build.stderr)
        if build.returncode: print(build.stdout+build.stderr); return build.returncode
        run=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=30)
        print('O'+opt,run.stdout.strip())
        if run.returncode: print(run.stderr); return run.returncode
    return 0
if __name__=='__main__': raise SystemExit(main())
