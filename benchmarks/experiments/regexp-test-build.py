from pathlib import Path
import subprocess, sys
root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/regexp-analysis'
src=root.parent/'integrated-object-layout/quickjs-patched'
out=root/sys.argv[1]
base=src/'libregexp.c'
exports=['exec','compile','get_alloc_count','check_bytecode','get_capture_count','get_flags','get_groupnames','parse_escape','id_continue_table_ascii','id_start_table_ascii']
flags=['clang','-std=c11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(src)]
with (out/'correctness-build.log').open('w') as log:
 subprocess.run(flags+['-Dlre_'+x+'=baseline_'+x for x in exports]+['-c',str(base),'-o',str(out/'regexp-baseline.o')],stdout=log,stderr=log,check=True)
 subprocess.run(flags+[str(out/'libregexp.c'),str(src/'libunicode.c'),str(Path(__file__).with_name('regexp-correctness.c')),str(out/'regexp-baseline.o'),'-o',str(out/'regexp-correctness')],stdout=log,stderr=log,check=True)
print('built sanitizer differential',sys.argv[1])
