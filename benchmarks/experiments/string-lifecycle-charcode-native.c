/* Included after quickjs.c and an unchanged reference charCodeAt body. */
static unsigned string_lifecycle_cases;
static int string_lifecycle_polls, string_lifecycle_abort;
static void lifecycle_check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        abort();
    }
}
static int lifecycle_interrupt(JSContext *ctx, void *opaque) {
    string_lifecycle_polls++;
    return string_lifecycle_abort && string_lifecycle_polls >= string_lifecycle_abort;
}
static void lifecycle_compare(JSContext *ctx, JSValue value, JSValue arg) {
    int32_t refs = JS_REF_COUNT(JS_VALUE_GET_PTR(value));
    JSValue expected = js_string_charCodeAt_reference(ctx, value, 1, &arg);
    JSValue actual = js_string_charCodeAt(ctx, value, 1, &arg);
    lifecycle_check(!JS_IsException(expected) && !JS_IsException(actual), "unexpected exception");
    lifecycle_check(js_same_value(ctx, expected, actual), "native result differential");
    lifecycle_check(JS_REF_COUNT(JS_VALUE_GET_PTR(value)) == refs, "receiver reference count");
    JS_FreeValue(ctx, expected);
    JS_FreeValue(ctx, actual);
    string_lifecycle_cases++;
}
static void lifecycle_view(JSContext *ctx, JSValue value, int len) {
    for (int i = -3; i < len + 3; i++) {
        lifecycle_compare(ctx, value, js_int32(i));
    }
    lifecycle_compare(ctx, value, js_int32(INT32_MIN));
    lifecycle_compare(ctx, value, js_int32(INT32_MAX));
    lifecycle_compare(ctx, value, JS_NewFloat64(ctx, 1.5));
    lifecycle_compare(ctx, value, JS_NewFloat64(ctx, -0.5));
    lifecycle_compare(ctx, value, JS_NewFloat64(ctx, INFINITY));
    lifecycle_compare(ctx, value, JS_NewFloat64(ctx, NAN));
}
int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    lifecycle_check(rt && ctx, "runtime");
    const int len = 17000;
    char *bytes = malloc(len);
    uint16_t *wide = malloc(len * sizeof(*wide));
    lifecycle_check(bytes && wide, "buffers");
    for (int i = 0; i < len; i++) {
        bytes[i] = (char) (i & 255);
        wide[i] = (uint16_t) (i * 17);
    }
    JSValue flat = JS_NewStringLen(ctx, bytes, len);
    JSValue utf16 = JS_NewStringUTF16(ctx, wide, len);
    lifecycle_check(!JS_IsException(flat) && !JS_IsException(utf16), "strings");
    lifecycle_view(ctx, flat, JS_VALUE_GET_STRING(flat)->len);
    lifecycle_view(ctx, utf16, len);
    JSValue slice = js_sub_string(ctx, JS_VALUE_GET_STRING(utf16), 17, len - 17);
    lifecycle_check(JS_VALUE_GET_STRING(slice)->kind == JS_STRING_KIND_SLICE, "slice fixture");
    lifecycle_view(ctx, slice, len - 34);
    JSString *indirect = js_alloc_string(ctx, sizeof(void *), 0);
    lifecycle_check(indirect != NULL, "indirect allocation");
    uint8_t *data = js_malloc(ctx, len);
    lifecycle_check(data != NULL, "indirect data");
    memcpy(data, bytes, len);
    *(void **) strv(indirect) = data;
    indirect->kind = JS_STRING_KIND_INDIRECT;
    indirect->len = len;
    JSValue iv = JS_MKPTR(JS_TAG_STRING, indirect);
    lifecycle_view(ctx, iv, len);
    JSAtom atom = JS_NewAtomLen(ctx, "lifecycle_atom", 14);
    JSValue av = JS_AtomToString(ctx, atom);
    lifecycle_view(ctx, av, 14);
    JSValue rope = js_new_string_rope(ctx, js_dup(flat), js_dup(utf16));
    JSValue keeper = js_dup(rope);
    lifecycle_check(JS_VALUE_GET_TAG(rope) == JS_TAG_STRING_ROPE, "rope fixture");
    JSValue zero = js_int32(0);
    JSValue first = js_string_charCodeAt(ctx, rope, 1, &zero);
    lifecycle_check(!JS_IsException(first), "uncached rope fallback");
    JS_FreeValue(ctx, first);
    JSStringRope *rp = JS_VALUE_GET_STRING_ROPE(rope);
    lifecycle_check(JS_VALUE_GET_TAG(rp->left) == JS_TAG_STRING && JS_VALUE_GET_STRING(rp->right)->len == 0,
                    "cached rope fixture");
    int32_t left_refs = JS_REF_COUNT(JS_VALUE_GET_STRING(rp->left));
    lifecycle_view(ctx, rope, rp->len);
    lifecycle_check(JS_REF_COUNT(JS_VALUE_GET_STRING(rp->left)) == left_refs, "flat child reference count");

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue string_ctor = JS_GetPropertyStr(ctx, global, "String");
    JSValue proto = JS_GetPropertyStr(ctx, string_ctor, "prototype");
    JSValue builtin = JS_GetPropertyStr(ctx, proto, "charCodeAt");
    const char *program = "(function(){var s='abcdefghijklmnopqrstuvwxyz';var n=0;for(var "
                          "i=0;i<50000;i++)n+=s.charCodeAt(i%26);return n;})()";
    JSValue code =
        JS_Eval(ctx, program, strlen(program), "lifecycle-polls", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    lifecycle_check(!JS_IsException(code), "poll compile");
    int expected_polls = 0;
    for (int variant = 0; variant < 2; variant++) {
        JSValue fn = variant ? js_dup(builtin) : JS_NewCFunction(ctx, js_string_charCodeAt_reference, "charCodeAt", 1);
        JS_SetPropertyStr(ctx, proto, "charCodeAt", fn);
        JS_SetInterruptHandler(rt, lifecycle_interrupt, NULL);
        ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT;
        string_lifecycle_polls = 0;
        string_lifecycle_abort = 0;
        JSValue result = JS_EvalFunction(ctx, js_dup(code));
        lifecycle_check(!JS_IsException(result), "poll execution");
        JS_FreeValue(ctx, result);
        if (variant) {
            lifecycle_check(string_lifecycle_polls == expected_polls, "normal poll parity");
        } else {
            expected_polls = string_lifecycle_polls;
        }
        lifecycle_check(expected_polls > 1, "normal polls present");
        ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT;
        string_lifecycle_polls = 0;
        string_lifecycle_abort = 2;
        result = JS_EvalFunction(ctx, js_dup(code));
        lifecycle_check(JS_IsException(result) && string_lifecycle_polls == 2, "interrupt abort parity");
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_SetInterruptHandler(rt, NULL, NULL);
    }
    JS_SetPropertyStr(ctx, proto, "charCodeAt", builtin);
    JS_FreeValue(ctx, code);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, string_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, keeper);
    JS_FreeValue(ctx, rope);
    JS_FreeValue(ctx, av);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, iv);
    JS_FreeValue(ctx, slice);
    JS_FreeValue(ctx, utf16);
    JS_FreeValue(ctx, flat);
    free(bytes);
    free(wide);
    JS_RunGC(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf(
        "PASS %u string view cases; normal_polls=%d abort_polls=2; flat/slice/indirect/atom/cached-rope roots valid\n",
        string_lifecycle_cases,
        expected_polls);
}
