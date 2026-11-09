"""Portable pure charCodeAt subdomain before native-frame publication.

The original JS_CallInternal poll and js_call_c_function stack check remain.
Other C calls only test the actual function pointer; ordinary JS calls do not
gain a new guard. No bytecode, object layout, or public C API changes.
"""


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:120])
    return s.replace(old, new, 1)


def transform(source):
    assert 'string_intrinsic_hits' not in source
    declaration = ('static JSValue js_string_charCodeAt(JSContext *ctx, JSValueConst this_val,\n'
                   '                                     int argc, JSValueConst *argv);\n')
    source = once(source, 'static int JS_InitAtoms(JSRuntime *rt);\n',
                  declaration + '\nstatic int JS_InitAtoms(JSRuntime *rt);\n')
    source = once(source, '    /* Shape hash table */\n',
        '#ifdef QJS_STRING_INTRINSIC_DIAGNOSTICS\n'
        '    uint64_t string_intrinsic_hits;\n'
        '#endif\n\n    /* Shape hash table */\n')
    signature = ('static JSValue js_call_c_function(JSContext *ctx, JSValueConst func_obj,\n'
                 '                                  JSValueConst this_obj,\n'
                 '                                  int argc, JSValueConst *argv, int flags)\n{\n')
    start = source.index(signature)
    end = source.index('\nstatic JSValue js_call_bound_function(', start)
    body = source[start:end]
    body = once(body, '    func = p->u.cfunc.c_function;\n', '')
    fast = r'''    /* Recognize the actual builtin, never a property name or method cache.
       This pure subdomain needs no callee frame/realm, conversion or temporary
       string ownership. Caller cleanup remains the original call path. */
    func = p->u.cfunc.c_function;
    if (func.generic == js_string_charCodeAt &&
        cproto == JS_CFUNC_generic && p->u.cfunc.magic == 0 && arg_count == 1 &&
        !(flags & JS_CALL_FLAG_CONSTRUCTOR) && argc >= 1 &&
        JS_VALUE_GET_TAG(this_obj) == JS_TAG_STRING &&
        JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        JSString *str = JS_VALUE_GET_STRING(this_obj);
        int idx = JS_VALUE_GET_INT(argv[0]);
#ifdef QJS_STRING_INTRINSIC_DIAGNOSTICS
        rt->string_intrinsic_hits++;
#endif
        if (idx < 0 || idx >= str->len)
            return JS_NAN;
        return js_int32(string_get(str, idx));
    }

'''
    body = once(body, '    prev_sf = rt->current_stack_frame;\n',
                fast + '    prev_sf = rt->current_stack_frame;\n')
    # Guard placement is part of the proof: original overflow check precedes it.
    assert body.index('if (js_check_stack_overflow(') < body.index('func.generic == js_string_charCodeAt')
    assert body.index('func.generic == js_string_charCodeAt') < body.index('rt->current_stack_frame = sf;')
    source = source[:start] + body + source[end:]
    source = once(source, 'void JS_RunGC(JSRuntime *rt)\n{\n',
        'void JS_RunGC(JSRuntime *rt)\n{\n'
        '#ifdef QJS_STRING_INTRINSIC_DIAGNOSTICS\n'
        '    fprintf(stderr, "STRING_INTRINSIC_STATS charCodeAt=%" PRIu64 "\\n",\n'
        '            rt->string_intrinsic_hits);\n'
        '#endif\n')
    return source
