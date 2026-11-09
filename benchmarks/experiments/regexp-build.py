import json, shlex, subprocess, shutil, sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/regexp-analysis'
repo=root.parents[2]
build=root.parent/'integrated-object-layout'
src=build/'quickjs-patched/libregexp.c'
name=sys.argv[1]
out=root/name; out.mkdir(exist_ok=True)
cc=next(c for c in json.loads((build/'compile_commands.json').read_text()) if c['file']==str(src))
args=shlex.split(cc['command'])
obj=out/'libregexp.c.o'
args[args.index('-o')+1]=str(obj)
args[args.index(str(src))]=str(out/'libregexp.c')
args+=['-iquote',str(src.parent)]
cmd=subprocess.check_output(['ninja','-C',str(build),'-t','commands','tjs-cli'],text=True)
link=shlex.split(cmd.splitlines()[-1])[2:-2]
archive=Path('quickjs-build/libqjs.a')
if str(archive) not in link:
 archive=next(Path(x) for x in link if x.endswith('/libqjs.a'))
with (out/'build.log').open('w') as log:
 subprocess.run(args,cwd=cc['directory'],stdout=log,stderr=log,check=True)
 shutil.copyfile(build/archive,out/'libqjs.a')
 subprocess.run(['/usr/bin/ar','-r',str(out/'libqjs.a'),str(obj)],stdout=log,stderr=log,check=True)
 link[link.index('-o')+1]=str(out/'tjs')
 link[link.index(str(archive))]=str(out/'libqjs.a')
 subprocess.run(link,cwd=build,stdout=log,stderr=log,check=True)
print('built',name,flush=True)
