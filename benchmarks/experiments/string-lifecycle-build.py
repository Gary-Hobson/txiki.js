import hashlib, json, shlex, shutil, subprocess, sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/string-lifecycle-analysis'
build=root.parent/'integrated-regexp-anchor'
src=build/'quickjs-patched/quickjs.c'
assert hashlib.sha256(src.read_bytes()).hexdigest() == '4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58', 'baseline source changed'
name=sys.argv[1];out=root/name;out.mkdir(parents=True, exist_ok=True)
cc=next(c for c in json.loads((build/'compile_commands.json').read_text()) if c['file']==str(src))
args=shlex.split(cc['command']);obj=out/'quickjs.c.o'
args[args.index('-o')+1]=str(obj)
args[args.index(str(src))]=str(out/'quickjs.c')
args+=['-iquote',str(src.parent)]
cmd=subprocess.check_output(['ninja','-C',str(build),'-t','commands','tjs-cli'],text=True)
link=shlex.split(cmd.splitlines()[-1])[2:-2]
archive=next(Path(x) for x in link if x.endswith('/libqjs.a'))
with (out/'build.log').open('w') as log:
 subprocess.run(args,cwd=cc['directory'],stdout=log,stderr=log,check=True)
 shutil.copyfile(build/archive,out/'libqjs.a')
 subprocess.run(['/usr/bin/ar','-r',str(out/'libqjs.a'),str(obj)],stdout=log,stderr=log,check=True)
 link[link.index('-o')+1]=str(out/'tjs')
 link[link.index(str(archive))]=str(out/'libqjs.a')
 subprocess.run(link,cwd=build,stdout=log,stderr=log,check=True)
print('built',name,flush=True)
