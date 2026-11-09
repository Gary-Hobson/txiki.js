from pathlib import Path
import hashlib,subprocess,shlex
root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/string-lifecycle-analysis'
src=root.parent/'integrated-regexp-anchor/quickjs-patched'
out=root/'string-lifecycle-charcode'
base=(src/'quickjs.c').read_text()
assert hashlib.sha256(base.encode()).hexdigest() == '4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58', 'baseline source changed'
start=base.index('static JSValue js_string_charCodeAt(')
end=base.index('static JSValue js_string_charAt(',start)
reference=base[start:end].replace('js_string_charCodeAt','js_string_charCodeAt_reference')
fixture=out/'string-lifecycle-native-combined.c'
fixture.write_text('#include "quickjs.c"\n'+reference+Path(__file__).with_name('string-lifecycle-charcode-native.c').read_text())
flags=['-std=gnu11','-O1','-g','-DNDEBUG','-D_GNU_SOURCE','-DFORCE_GC_AT_MALLOC','-funsigned-char','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(src)]
args=['clang',*flags,str(fixture),*[str(src/f) for f in ['dtoa.c','libregexp.c','libunicode.c']],'-lm','-o',str(out/'string-lifecycle-native')]
with (out/'native-build.log').open('w') as log:
 log.write(shlex.join(args)+'\n');log.flush()
 subprocess.run(args,stdout=log,stderr=log,check=True)
print('built string lifecycle ASAN+UBSAN+GC-stress native')
