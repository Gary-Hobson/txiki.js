from pathlib import Path
import hashlib


def transform(src):
    old='''static JSValue js_string_charCodeAt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val, ret;
    JSString *p;
    int idx, c;
'''
    assert src.count(old)==1
    return src.replace(old,old+'''
    if (argc > 0 && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        p = NULL;
        if (JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING) {
            p = JS_VALUE_GET_STRING(this_val);
        } else if (JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING_ROPE) {
            JSStringRope *r = JS_VALUE_GET_STRING_ROPE(this_val);
            if (JS_VALUE_GET_TAG(r->left) == JS_TAG_STRING &&
                JS_VALUE_GET_TAG(r->right) == JS_TAG_STRING &&
                JS_VALUE_GET_STRING(r->right)->len == 0)
                p = JS_VALUE_GET_STRING(r->left);
        }
        if (p) {
            /* The native call owns this_val; the cached rope owns its flat
               child. No allocation or coercion can invalidate this view. */
            idx = JS_VALUE_GET_INT(argv[0]);
            if ((uint32_t)idx >= p->len)
                return JS_NAN;
            return js_int32(string_get(p, idx));
        }
    }
''')

if __name__=='__main__':
    root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/string-lifecycle-analysis'
    out=root/'string-lifecycle-charcode';out.mkdir(parents=True, exist_ok=True)
    source = (root.parent/'integrated-regexp-anchor/quickjs-patched/quickjs.c').read_text()
    assert hashlib.sha256(source.encode()).hexdigest() == '4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58', 'baseline source changed'
    (out/'quickjs.c').write_text(transform(source))
