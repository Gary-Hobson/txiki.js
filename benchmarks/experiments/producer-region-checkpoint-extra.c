/* Include after candidate quickjs.c; diagnostic-only internal adversarial
 * callbacks verify release/local-commit order, stack checking and realms. */
static JSContext *px_ctx;
static JSValue px_fn, px_proto, px_object;
static JSAtom px_method;
static JSClassID px_class;
static int px_mode, px_callbacks, px_finalizers, px_old_undefined;

static void px_check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL producer extra: %s\n", what);
        abort();
    }
}

static JSValue px_eval(JSContext *ctx, const char *src) {
    JSValue value = JS_Eval(ctx, src, strlen(src), "producer-extra.js", JS_EVAL_TYPE_GLOBAL);
    px_check(!JS_IsException(value), "evaluation");
    return value;
}

static JSProducerRegionSite *px_site(void) {
    JSFunctionBytecode *b = JS_VALUE_GET_OBJ(px_fn)->u.func.function_bytecode;
    px_check(b->producer_regions && b->producer_regions->count == 1, "one planned site");
    return b->producer_regions->sites;
}

static void px_finalizer(JSRuntime *rt, JSValue value) {
    JSStackFrame *sf = rt->current_stack_frame;
    JSProducerRegionSite *site = px_site();
    px_check(sf && JS_VALUE_GET_PTR(sf->cur_func) == JS_VALUE_GET_PTR(px_fn), "cleanup caller frame");
    px_finalizers++;
    JSValue old = sf->var_buf[site->local];
    px_old_undefined = JS_IsUndefined(old);
    /* An existing strong root supplies the replacement: no allocation or JS
       execution inside a class finalizer. The suffix must recheck after this. */
    sf->var_buf[site->local] = js_dup(px_object);
    JS_FreeValueRT(rt, old);
}

static int px_interrupt(JSContext *ctx, void *opaque) {
    px_callbacks++;
    if (px_mode == 1) {
        px_check(JS_DeleteProperty(ctx, px_proto, px_method, 0) == 1, "remove last external callee root");
    } else if (px_mode == 2) {
        /* Outer JS frame already exists; only the native call check remains. */
        JS_SetMaxStackSize(ctx->rt, 1);
    }
    return 0;
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    px_ctx = ctx;
    px_check(rt && ctx, "runtime");
    px_fn = px_eval(ctx, "(function producer(s,i){var c;if((c=s.charCodeAt(i))>64)return c;return -1;})");
    px_site();
    px_proto = px_eval(ctx, "String.prototype");
    JSValue original = JS_GetPropertyStr(ctx, px_proto, "charCodeAt");
    px_method = JS_NewAtom(ctx, "charCodeAt");
    px_object = JS_NewObject(ctx);
    JS_NewClassID(rt, &px_class);
    JSClassDef definition = { .class_name = "ProducerCleanupProbe", .finalizer = px_finalizer };
    px_check(JS_NewClass(rt, px_class, &definition) == 0, "probe class");
    JSValue clone = JS_NewCFunction(ctx, js_string_charCodeAt, "charCodeAt", 1);
    JSValue probe = JS_NewObjectClass(ctx, px_class);
    px_check(JS_SetPropertyStr(ctx, clone, "probe", probe) == 1, "probe ownership");
    px_check(JS_SetProperty(ctx, px_proto, px_method, clone) == 1, "clone callee ownership");
    JSValue args[2] = { JS_NewString(ctx, "AZ"), js_int32(0) };
    JS_SetInterruptHandler(rt, px_interrupt, NULL);
    ctx->interrupt_counter = 2;
    px_mode = 1;
    uint64_t failures = rt->producer_region_stats.local_guard_fail;
    JSValue result = JS_Call(ctx, px_fn, JS_UNDEFINED, 2, args);
    px_check(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 65, "cleanup fallback result");
    JS_FreeValue(ctx, result);
    px_check(px_callbacks == 1 && px_finalizers == 1 && px_old_undefined, "cleanup precedes local commit");
    px_check(rt->producer_region_stats.local_guard_fail == failures + 1, "post-cleanup guard observed finalizer write");
    px_check(JS_SetProperty(ctx, px_proto, px_method, js_dup(original)) == 1, "restore builtin");

    size_t stack_size = rt->stack_size;
    px_mode = 2;
    ctx->interrupt_counter = 2;
    result = JS_Call(ctx, px_fn, JS_UNDEFINED, 2, args);
    JS_SetMaxStackSize(rt, stack_size);
    px_check(JS_IsException(result), "native stack check after poll");
    JSValue error = JS_GetException(ctx);
    JSValue message = JS_GetPropertyStr(ctx, error, "message");
    const char *text = JS_ToCString(ctx, message);
    px_check(text && strstr(text, "stack"), "native stack overflow error");
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, error);
    px_mode = 0;
    JS_SetInterruptHandler(rt, NULL, NULL);

    /* Foreign builtin realm matters on the domain fallback, which must keep
       the full ordinary native frame and its realm-specific error prototype. */
    JSContext *other = JS_NewContext(rt);
    JSValue foreign_proto = px_eval(other, "String.prototype");
    JSValue foreign_builtin = JS_GetPropertyStr(other, foreign_proto, "charCodeAt");
    JSValue foreign_type_error = px_eval(other, "TypeError.prototype");
    JS_SetProperty(ctx, px_proto, px_method, foreign_builtin);
    JSValue symbol = px_eval(ctx, "Symbol('index')");
    args[1] = symbol;
    result = JS_Call(ctx, px_fn, JS_UNDEFINED, 2, args);
    px_check(JS_IsException(result), "foreign builtin domain exception");
    error = JS_GetException(ctx);
    JSValue prototype = JS_GetPrototype(ctx, error);
    px_check(JS_VALUE_GET_PTR(prototype) == JS_VALUE_GET_PTR(foreign_type_error), "fallback native realm");
    JS_FreeValue(ctx, prototype);
    JS_FreeValue(ctx, error);
    JS_FreeValue(ctx, symbol);
    JS_SetProperty(ctx, px_proto, px_method, original);
    JS_FreeValue(other, foreign_type_error);
    JS_FreeValue(other, foreign_proto);
    JS_FreeContext(other);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, px_object);
    JS_FreeValue(ctx, px_proto);
    JS_FreeValue(ctx, px_fn);
    JS_FreeAtom(ctx, px_method);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts("PASS producer extra cleanup-finalizer checkpoint native-stack-check fallback-realm");
    return 0;
}
