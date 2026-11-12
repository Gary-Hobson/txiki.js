static bool qjs_jit_scalar_call_allowed(JSContext *ctx, const JSFunctionBytecode *target,
                                        int argc, bool copy_argv)
{
    JSRuntime *rt = ctx->rt;
    if (rt->jit_shadow || target->realm != ctx || rt->interrupt_handler || argc < target->arg_count)
        return false;
#ifdef BUILD_WITH_INSPECTOR
    if (ctx->debug_trace)
        return false;
#endif
#ifdef ENABLE_DUMPS
    if (check_dump_flag(rt, JS_DUMP_BYTECODE_STEP))
        return false;
#endif
    size_t slots = target->var_count + target->stack_size + (copy_argv ? target->arg_count : 0);
    return !js_check_stack_overflow(rt, sizeof(JSValue) * slots + QJS_JIT_NATIVE_STACK);
}

/* A call-site may bypass JS_CallInternal for a ready, side-effect-free scalar
   callee. The interpreter and boxed callers keep the callee and argv rooted
   until this helper returns. No call site retains a pointer into native code. */
static bool qjs_jit_ready_scalar_call(JSContext *ctx, JSFunctionBytecode *target,
                                      int argc, const JSValue *argv, JSValue *result)
{
    JSRuntime *rt = ctx->rt;
    if (!target->jit_code || target->jit_retry_delay ||
        !qjs_jit_scalar_call_allowed(ctx, target, argc, false))
        return false;

    QJSJitArgs args;
    if (target->jit_f64 ? !qjs_jit_unbox_float_args(target, argv, args.f64)
                        : !qjs_jit_unbox_int_args(target, argv, args.i32))
        return false;

    QJSJitExit exit;
    exit.pc = UINT32_MAX;
    qjs_jit_cache_tick(rt);
    if (!qjs_jit_enter(rt, target, &args, 0, NULL, result, &exit)) {
        qjs_jit_discard_exit(rt, &exit);
        return false;
    }

    /* Match the interpreter's entry polling cadence. The handler is absent
       and the native scalar callee cannot install one or run user code. */
    js_poll_interrupts(ctx);
    return true;
}

static no_inline bool qjs_jit_constructor_body(JSContext *ctx, JSFunctionBytecode *target,
                                                int argc, const JSValue *argv, JSValue *result,
                                                bool copy_argv)
{
    JSRuntime *rt = ctx->rt;
    if (target->jit_rejected || !qjs_jit_scalar_call_allowed(ctx, target, argc, copy_argv))
        return false;

    QJSJitExit exit;
    exit.pc = UINT32_MAX;
    if (!qjs_jit_call(rt, target, argv, result, &exit)) {
        /* The caller has already allocated the receiver. Only the scalar
           body may be replayed, since it cannot use or mutate the receiver. */
        qjs_jit_discard_exit(rt, &exit);
        return false;
    }
    js_poll_interrupts(ctx);
    rt->jit_constructor_hits++;
    if (target->jit_optimized)
        rt->jit_constructor_optimized_hits++;
    return true;
}

static no_inline bool qjs_jit_profiled_call(JSContext *ctx, JSFunctionBytecode *caller,
                                            const uint8_t *pc, JSValueConst callee,
                                            int argc, const JSValue *argv, JSValue *result,
                                            bool from_boxed)
{
    JSRuntime *rt = ctx->rt;
    QJSJitFeedbackSite *site = NULL;
    if (rt->jit_enabled && caller->jit_feedback)
        site = qjs_jit_feedback_find(caller->jit_feedback, pc - caller->byte_code_buf,
                                     QJS_JIT_FEEDBACK_CALL, false);
    if (!site || site->u.call.count != 1 || site->u.call.other ||
        site->u.call.targets[0].count < QJS_JIT_BASELINE_THRESHOLD) {
        qjs_jit_feedback_call(rt, caller, pc, callee);
        return false;
    }

    JSFunctionBytecode *target = NULL;
    if (JS_VALUE_GET_TAG(callee) == JS_TAG_OBJECT) {
        JSObject *object = JS_VALUE_GET_OBJ(callee);
        if (object->class_id == JS_CLASS_BYTECODE_FUNCTION)
            target = object->u.func.function_bytecode;
    }
    if (!target || target->jit_feedback_target_id != site->u.call.targets[0].id) {
        /* Observe a changed target immediately, even between sample windows. */
        qjs_jit_feedback_record_call(rt, site, callee);
        return false;
    }

    if (rt->jit_optimize && !target->jit_optimization_attempted &&
        target->jit_baseline_hits >= QJS_JIT_OPTIMIZED_THRESHOLD)
        return false;
    if (!qjs_jit_ready_scalar_call(ctx, target, argc, argv, result))
        return false;

    qjs_jit_feedback_increment(&site->u.call.native_hits);
    if (from_boxed)
        qjs_jit_feedback_increment(&site->u.call.boxed_hits);
    return true;
}
