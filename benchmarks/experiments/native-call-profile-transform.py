"""Diagnostic-only native builtin frequency counters; never use for scoring."""


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:120])
    return s.replace(old, new, 1)


GENERIC = ['charCodeAt', 'charAt', 'codePointAt', 'at', 'substring', 'substr', 'slice']
NAMES = ['total', 'charCodeAt', 'eligible', 'in_bounds', 'out_bounds',
         'charAt', 'codePointAt', 'at', 'substring', 'substr', 'slice',
         'indexOf', 'lastIndexOf', 'includes', 'startsWith', 'endsWith', 'other']


def transform(source):
    assert 'native_call_profile' not in source
    declarations = ''.join(
        f'static JSValue js_string_{name}(JSContext *ctx, JSValueConst this_val,\n'
        '                                     int argc, JSValueConst *argv);\n'
        for name in GENERIC)
    declarations += ''.join(
        f'static JSValue js_string_{name}(JSContext *ctx, JSValueConst this_val,\n'
        '                                     int argc, JSValueConst *argv, int magic);\n'
        for name in ['indexOf', 'includes'])
    source = once(source, 'static int JS_InitAtoms(JSRuntime *rt);\n',
                  declarations + '\nstatic int JS_InitAtoms(JSRuntime *rt);\n')
    source = once(source, '    /* Shape hash table */\n',
                  f'    uint64_t native_call_profile[{len(NAMES)}];\n\n    /* Shape hash table */\n')
    code = r'''
    rt->native_call_profile[0]++;
    if (cproto == JS_CFUNC_generic &&
        p->u.cfunc.c_function.generic == js_string_charCodeAt) {
        rt->native_call_profile[1]++;
        if (!(flags & JS_CALL_FLAG_CONSTRUCTOR) && arg_count == 1 &&
            p->u.cfunc.magic == 0 && argc >= 1 &&
            JS_VALUE_GET_TAG(this_obj) == JS_TAG_STRING &&
            JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
            JSString *str = JS_VALUE_GET_STRING(this_obj);
            int idx = JS_VALUE_GET_INT(argv[0]);
            rt->native_call_profile[2]++;
            rt->native_call_profile[idx >= 0 && idx < str->len ? 3 : 4]++;
        }
    }
'''
    for i, name in enumerate(GENERIC[1:], start=5):
        code += (f'    else if (cproto == JS_CFUNC_generic &&\n'
                 f'             p->u.cfunc.c_function.generic == js_string_{name})\n'
                 f'        rt->native_call_profile[{i}]++;\n')
    code += r'''    else if (cproto == JS_CFUNC_generic_magic &&
             p->u.cfunc.c_function.generic_magic == js_string_indexOf &&
             (p->u.cfunc.magic == 0 || p->u.cfunc.magic == 1))
        rt->native_call_profile[11 + p->u.cfunc.magic]++;
    else if (cproto == JS_CFUNC_generic_magic &&
             p->u.cfunc.c_function.generic_magic == js_string_includes &&
             p->u.cfunc.magic >= 0 && p->u.cfunc.magic <= 2)
        rt->native_call_profile[13 + p->u.cfunc.magic]++;
    else
        rt->native_call_profile[16]++;

'''
    source = once(source,
        '    cproto = p->u.cfunc.cproto;\n    arg_count = p->u.cfunc.length;\n',
        '    cproto = p->u.cfunc.cproto;\n    arg_count = p->u.cfunc.length;\n' + code)
    fmt = 'NATIVE_CALL_PROFILE' + ''.join(f' {name}=%" PRIu64 "' for name in NAMES) + '\\n'
    output = f'    fprintf(stderr, "{fmt}",\n' + ',\n'.join(
        f'            rt->native_call_profile[{i}]' for i in range(len(NAMES))) + ');\n'
    source = once(source, 'void JS_RunGC(JSRuntime *rt)\n{\n',
                  'void JS_RunGC(JSRuntime *rt)\n{\n' + output)
    return source
