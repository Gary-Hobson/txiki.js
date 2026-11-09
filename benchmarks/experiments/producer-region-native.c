/* Compile by including the candidate quickjs.c immediately before this file.
 * Internal access makes checkpoint/retirement assertions deterministic; this
 * fixture is not linked into the measured binary. Requires diagnostics. */
static int pr_test_polls, pr_test_abort, pr_test_mode, pr_test_nested;
static JSValue pr_test_function, pr_test_code;
static uint8_t *pr_test_canonical;
static size_t pr_test_canonical_len;
static int pr_test_finalized;
static JSClassID pr_test_class;

static void pr_check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL producer region: %s\n", what);
        abort();
    }
}

static JSValue pr_eval(JSContext *ctx, const char *src) {
    JSValue result = JS_Eval(ctx, src, strlen(src), "producer-native.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, ex);
        fprintf(stderr, "JS failure: %s\n", message ? message : "unknown");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, ex);
        abort();
    }
    return result;
}

static JSFunctionBytecode *pr_bytecode(JSValue fn) {
    return JS_VALUE_GET_OBJ(fn)->u.func.function_bytecode;
}

static void pr_serialized_equal(JSContext *ctx) {
    size_t len;
    uint8_t *bytes = JS_WriteObject(ctx, &len, pr_test_code, JS_WRITE_OBJ_BYTECODE);
    pr_check(bytes && len == pr_test_canonical_len && !memcmp(bytes, pr_test_canonical, len), "active canonical bytes");
    js_free(ctx, bytes);
}

static int pr_interrupt(JSContext *ctx, void *opaque) {
    JSRuntime *rt = ctx->rt;
    pr_test_polls++;
    if (pr_test_mode && !pr_test_nested) {
        JSStackFrame *sf = rt->current_stack_frame;
        pr_test_nested = 1;
        pr_check(sf && JS_VALUE_GET_OBJ(sf->cur_func)->class_id == JS_CLASS_BYTECODE_FUNCTION,
                 "call poll still exposes bytecode caller");
        pr_check(pr_bytecode(sf->cur_func) == pr_bytecode(pr_test_function), "call poll caller identity");
        pr_serialized_equal(ctx);
        if (pr_test_mode == 1) {
            JSProducerRegionPlan *plan = pr_bytecode(pr_test_function)->producer_regions;
            pr_check(plan && plan->count == 1, "one selected native site");
            JSValue old = sf->var_buf[plan->sites[0].local];
            sf->var_buf[plan->sites[0].local] = JS_NewObject(ctx);
            JS_FreeValue(ctx, old);
        } else if (pr_test_mode == 2) {
            /* Recursive same-site call sees a non-target and retires the
               entry while this outer handler retains its original recipe. */
            JSValue object = pr_eval(ctx, "({charCodeAt:function(){return 80}})");
            JSValue args[2] = { object, js_int32(0) };
            JSValue result = JS_Call(ctx, pr_test_function, JS_UNDEFINED, 2, args);
            pr_check(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 80,
                     "recursive non-target result");
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, object);
            pr_check(pr_bytecode(pr_test_function)->producer_regions->sites[0].retired,
                     "recursive retirement performed");
            pr_serialized_equal(ctx);
        }
    }
    return pr_test_abort && pr_test_polls >= pr_test_abort;
}

static void pr_finalizer(JSRuntime *rt, JSValue value) {
    pr_test_finalized++;
}

static JSValue pr_call(JSContext *ctx, JSValue receiver, JSValue index) {
    JSValue args[2] = { receiver, index };
    return JS_Call(ctx, pr_test_function, JS_UNDEFINED, 2, args);
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    pr_check(rt && ctx, "runtime");
    const char *source = "function producer(s,i){var c;if((c=s.charCodeAt(i))>64)return c;return -1;}";
    pr_test_code =
        JS_Eval(ctx, source, strlen(source), "producer-checkpoint.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    pr_check(!JS_IsException(pr_test_code), "compile-only");
    pr_test_canonical = JS_WriteObject(ctx, &pr_test_canonical_len, pr_test_code, JS_WRITE_OBJ_BYTECODE);
    pr_check(pr_test_canonical != NULL, "canonical snapshot");
    JSValue result = JS_EvalFunction(ctx, js_dup(pr_test_code));
    pr_check(!JS_IsException(result), "instantiate");
    JS_FreeValue(ctx, result);
    JSValue global = JS_GetGlobalObject(ctx);
    pr_test_function = JS_GetPropertyStr(ctx, global, "producer");
    JSFunctionBytecode *b = pr_bytecode(pr_test_function);
    pr_check(b->producer_regions && b->producer_regions->count == 1, "actual selected site");
    JSValue flat = JS_NewString(ctx, "AZ");
    JS_SetInterruptHandler(rt, pr_interrupt, NULL);

    /* JS_Call entry decrements 2 -> 1; producer's call poll triggers callback.
       Force a refcounted old local only after the callee arguments are owned. */
    ctx->interrupt_counter = 2;
    pr_test_mode = 1;
    pr_test_nested = 0;
    uint64_t fails = rt->producer_region_stats.local_guard_fail;
    result = pr_call(ctx, flat, js_int32(0));
    pr_check(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 65,
             "post-poll local fallback value");
    JS_FreeValue(ctx, result);
    pr_check(rt->producer_region_stats.local_guard_fail == fails + 1, "post-poll guard actually failed");

    /* Native primitive/rope views and domain misses must not retire. */
    pr_test_mode = 0;
    JSValue rope = js_new_string_rope(ctx, JS_NewString(ctx, "AZ"), JS_NewString(ctx, "BC"));
    JSValue rope_keeper = js_dup(rope); /* memoization needs more than one rope owner */
    JSValue zero = js_int32(0);
    result = js_string_charCodeAt(ctx, rope, 1, &zero);
    JS_FreeValue(ctx, result);
    uint64_t cached = rt->producer_region_stats.fast_cached_rope;
    result = pr_call(ctx, rope, zero);
    pr_check(JS_VALUE_GET_INT(result) == 65, "cached rope result");
    JS_FreeValue(ctx, result);
    pr_check(rt->producer_region_stats.fast_cached_rope == cached + 1, "cached rope actual hit");
    result = pr_call(ctx, flat, JS_NewFloat64(ctx, 0.5));
    pr_check(JS_VALUE_GET_INT(result) == 65, "non-int domain fallback");
    JS_FreeValue(ctx, result);
    pr_check(!b->producer_regions->sites[0].retired, "domain miss does not retire");

    /* Equal call/branch checkpoint counts against the same function with its
       entry canonically restored. Every 50k invocation has exactly 150k polls. */
    int normal_polls = 0;
    for (int variant = 0; variant < 2; variant++) {
        if (variant) {
            js_producer_region_restore(b->producer_regions, b->byte_code_buf);
        }
        ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT;
        pr_test_polls = 0;
        pr_test_abort = 0;
        for (int i = 0; i < 50000; i++) {
            result = pr_call(ctx, flat, zero);
            pr_check(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 65, "poll result");
            JS_FreeValue(ctx, result);
        }
        if (!variant) {
            normal_polls = pr_test_polls;
        } else {
            pr_check(pr_test_polls == normal_polls, "normal poll parity");
        }
        ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT;
        pr_test_polls = 0;
        pr_test_abort = 2;
        for (int i = 0; i < 50000; i++) {
            result = pr_call(ctx, flat, zero);
            if (JS_IsException(result)) {
                break;
            }
            JS_FreeValue(ctx, result);
        }
        pr_check(JS_IsException(result) && pr_test_polls == 2, "abort checkpoint parity");
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    pr_check(normal_polls == 15, "expected checkpoint count");
    pr_test_abort = 0;
    /* Reinstall only while no invocation exists; production never re-quickens. */
    b->byte_code_buf[b->producer_regions->sites[0].pc] = OP_producer_region;
    put_u16(b->byte_code_buf + b->producer_regions->sites[0].pc + 1, 0);
    ctx->interrupt_counter = 2;
    pr_test_mode = 2;
    pr_test_nested = 0;
    result = pr_call(ctx, flat, zero);
    pr_check(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 65,
             "outer handler survives recursive retirement");
    JS_FreeValue(ctx, result);
    pr_test_mode = 0;
    JS_SetInterruptHandler(rt, NULL, NULL);
    pr_serialized_equal(ctx);

    /* Read the canonical bytes while the original live plan remains rooted. */
    JSValue restored =
        JS_ReadObject(ctx, pr_test_canonical, pr_test_canonical_len, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA);
    pr_check(!JS_IsException(restored), "ROM read canonical bytes");
    result = JS_EvalFunction(ctx, restored);
    pr_check(!JS_IsException(result), "restored execution");
    JS_FreeValue(ctx, result);
    result = pr_eval(ctx, "producer('AZ',1)");
    pr_check(JS_VALUE_GET_INT(result) == 90, "restored producer result");
    JS_FreeValue(ctx, result);
    pr_serialized_equal(ctx);
    JS_FreeValue(ctx, rope_keeper);
    JS_FreeValue(ctx, rope);
    JS_FreeValue(ctx, flat);
    JS_FreeValue(ctx, pr_test_function);
    JS_FreeValue(ctx, pr_test_code);
    JS_FreeValue(ctx, global);
    js_free(ctx, pr_test_canonical);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("PASS producer native checkpoints normal_polls=%d abort=2 active-canonical recursive-retirement local-guard "
           "cached-rope ROM\n",
           normal_polls);
    return 0;
}
