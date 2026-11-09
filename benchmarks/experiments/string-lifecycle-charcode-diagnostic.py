from pathlib import Path
import importlib.util
root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/string-lifecycle-analysis'
spec=importlib.util.spec_from_file_location('lifecycle',Path(__file__).with_name('string-lifecycle-diagnostic-transform.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
source=(root.parent/'integrated-regexp-anchor/quickjs-patched/quickjs.c').read_text()
assert m.hashlib.sha256(source.encode()).hexdigest() == '4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58', 'baseline source changed'
s=m.transform(source)
s=s.replace('static bool string_lifecycle_started;', '''static unsigned long long string_lifecycle_charcode[4][2];
static unsigned long long string_lifecycle_charcode_bounds[2], string_lifecycle_charcode_kind[3];
static bool string_lifecycle_started;''')
s=s.replace('    for (int k = 0; k < 12; k++) {','''    for (int k = 0; k < 4; k++)
        fprintf(stderr, "STRING_LIFECYCLE charcode receiver=%d int=%llu other=%llu\\n", k,
            string_lifecycle_charcode[k][1], string_lifecycle_charcode[k][0]);
    fprintf(stderr, "STRING_LIFECYCLE charcode inbounds=%llu outbounds=%llu normal=%llu slice=%llu indirect=%llu\\n",
        string_lifecycle_charcode_bounds[1], string_lifecycle_charcode_bounds[0],
        string_lifecycle_charcode_kind[0], string_lifecycle_charcode_kind[1], string_lifecycle_charcode_kind[2]);
    for (int k = 0; k < 12; k++) {''')
marker='''static JSValue js_string_charCodeAt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val, ret;
    JSString *p;
    int idx, c;
'''
assert s.count(marker)==1
s=s.replace(marker,marker+'''
    {
        int kind = 3, is_int = argc > 0 && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT;
        JSString *view = NULL;
        if (JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING) {
            kind = 0;
            view = JS_VALUE_GET_STRING(this_val);
        } else if (JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING_ROPE) {
            JSStringRope *rope = JS_VALUE_GET_STRING_ROPE(this_val);
            kind = 2;
            if (JS_VALUE_GET_TAG(rope->left) == JS_TAG_STRING &&
                JS_VALUE_GET_TAG(rope->right) == JS_TAG_STRING &&
                JS_VALUE_GET_STRING(rope->right)->len == 0) {
                kind = 1;
                view = JS_VALUE_GET_STRING(rope->left);
            }
        }
        string_lifecycle_charcode[kind][is_int]++;
        if (view && is_int) {
            int index = JS_VALUE_GET_INT(argv[0]);
            string_lifecycle_charcode_bounds[index >= 0 && (uint32_t)index < view->len]++;
            string_lifecycle_charcode_kind[view->kind]++;
        }
    }
''')
out=root/'string-lifecycle-charcode-diagnostic';out.mkdir(parents=True, exist_ok=True)
(out/'quickjs.c').write_text(s)
