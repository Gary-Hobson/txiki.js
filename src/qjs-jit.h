/* Included by quickjs.c, after the interpreter types and allocation helpers. */
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    QJS_JIT_MAX_ARGS = 16,
    QJS_JIT_CODE_WORDS = 16384,
    QJS_JIT_CODE_LIMIT = 8 * 1024 * 1024,
    QJS_JIT_BASELINE_THRESHOLD = 32,
    QJS_JIT_OPTIMIZED_THRESHOLD = 128,
    QJS_JIT_OSR_THRESHOLD = 128,
    QJS_JIT_FUEL = 32768,
    QJS_JIT_RETRY_DELAY = 32,
    QJS_JIT_MAX_FAILURE_STREAK = 4,
    QJS_JIT_NATIVE_STACK = 1024,
};

typedef struct {
    uint32_t pc, word_offset, locals_mask;
} QJSJitOSR;

#include "qjs-jit-cache.h"
#include "qjs-jit-arm64.h"
#include "qjs-jit-f64.h"

typedef int QJSJitEntry(const int32_t *args, QJSJitExit *exit, uint32_t fuel,
                        const int32_t *locals);
typedef int QJSJitF64Entry(const double *args, QJSJitF64Exit *exit, uint32_t fuel,
                           const double *locals);

typedef union {
    int32_t i32[QJS_JIT_MAX_ARGS];
    double f64[QJS_JIT_MAX_ARGS];
} QJSJitArgs;

typedef enum {
    QJS_JIT_COMPILE_READY,
    QJS_JIT_COMPILE_RETRY,
    QJS_JIT_COMPILE_UNSUPPORTED,
} QJSJitCompileResult;

static void qjs_jit_free(JSRuntime *rt, JSFunctionBytecode *b)
{
    qjs_jit_cache_remove(rt, b);
}

static QJSJitCompileResult qjs_jit_compile(JSRuntime *rt, JSFunctionBytecode *b,
                                           bool optimized, bool f64)
{
    if (b->byte_code_len <= 0 || b->byte_code_len > QJS_ARM64_MAX_BYTECODE ||
        b->var_count > QJS_JIT_MAX_LOCALS || b->stack_size > QJS_ARM64_MAX_STACK ||
        b->arg_count > QJS_JIT_MAX_ARGS || b->var_ref_count || b->closure_var_count)
        return QJS_JIT_COMPILE_UNSUPPORTED;
    if (rt->jit_cache_tick < b->jit_cache_retry_at)
        return QJS_JIT_COMPILE_RETRY;

    uint64_t started;
    if (!qjs_jit_cache_begin_compile(rt, b, &started)) {
        b->jit_cache_retry_at = rt->jit_cache_tick + QJS_JIT_CACHE_COMPILE_RETRY;
        rt->jit_cache_deferrals++;
        return QJS_JIT_COMPILE_RETRY;
    }

    QJSJitCompileResult result = QJS_JIT_COMPILE_RETRY;
    QJSArm64Workspace *workspace = js_mallocz_rt(rt, sizeof(*workspace));
    uint32_t *scratch = js_malloc_rt(rt, QJS_JIT_CODE_WORDS * sizeof(*scratch));
    if (workspace && scratch) {
        QJSJitOSR osr = { 0 };
        size_t words = f64 ?
                       qjs_f64_compile(rt, workspace, b, optimized, scratch, QJS_JIT_CODE_WORDS, &osr) :
                       qjs_arm64_compile(rt, workspace, b, optimized, scratch, QJS_JIT_CODE_WORDS, &osr);
        if (!words)
            result = QJS_JIT_COMPILE_UNSUPPORTED;
        else if (qjs_jit_cache_install(rt, b, scratch, words * sizeof(*scratch), osr))
            result = QJS_JIT_COMPILE_READY;
    }
    js_free_rt(rt, scratch);
    js_free_rt(rt, workspace);
    qjs_jit_cache_end_compile(rt, started);

    if (result == QJS_JIT_COMPILE_RETRY) {
        b->jit_cache_retry_at = rt->jit_cache_tick + QJS_JIT_CACHE_COMPILE_RETRY;
        rt->jit_cache_deferrals++;
    }
    if (result != QJS_JIT_COMPILE_READY)
        return result;

    b->jit_optimized = optimized;
    b->jit_f64 = f64;
    if (optimized)
        rt->jit_optimized_compiles++;
    else
        rt->jit_baseline_compiles++;
    if (f64)
        rt->jit_f64_compiles++;
    return QJS_JIT_COMPILE_READY;
}

static bool qjs_jit_unbox_int_args(const JSFunctionBytecode *b, const JSValue *args,
                                  int32_t *unboxed)
{
    for (unsigned i = 0; i < b->arg_count; i++) {
        if (JS_VALUE_GET_TAG(args[i]) != JS_TAG_INT)
            return false;
        unboxed[i] = JS_VALUE_GET_INT(args[i]);
    }
    return true;
}

static bool qjs_jit_unbox_float_args(const JSFunctionBytecode *b, const JSValue *args,
                                    double *unboxed)
{
    for (unsigned i = 0; i < b->arg_count; i++) {
        if (!js_arith_to_float64(args[i], &unboxed[i]))
            return false;
    }
    return true;
}

static bool qjs_jit_try_f64(JSRuntime *rt, JSFunctionBytecode *b, bool optimized)
{
    if (!b->jit_f64_rejected) {
        QJSJitCompileResult result = qjs_jit_compile(rt, b, optimized, true);
        if (result == QJS_JIT_COMPILE_READY)
            return true;
        if (result == QJS_JIT_COMPILE_UNSUPPORTED)
            b->jit_f64_rejected = true;
    }
    return false;
}

static bool qjs_jit_promote(JSRuntime *rt, JSFunctionBytecode *b)
{
    if (!qjs_jit_try_f64(rt, b, b->jit_optimized))
        return false;
    b->jit_retry_delay = 0;
    b->jit_failure_streak = 0;
    b->jit_optimization_attempted = b->jit_optimized;
    rt->jit_f64_promotions++;
    return true;
}

#if defined(__clang__)
/* Generated entry points have no Clang function-sanitizer metadata. */
__attribute__((no_sanitize("function")))
#endif
static no_inline int qjs_jit_enter_f64(JSRuntime *rt, QJSJitF64Entry *entry,
                                       const double *args, const double *locals,
                                       JSValue *result, QJSJitExit *exit)
{
    QJSJitF64Exit native;
    int status = entry(args, &native, QJS_JIT_FUEL, locals);
    if (status == 1) {
        *result = js_number(native.result);
    } else if (status == QJS_JIT_EXIT_PRECISE_FUEL) {
        /* Only an actual exit needs to outlive this native activation. */
        QJSJitF64Exit *snapshot = js_malloc_rt(rt, sizeof(*snapshot));
        if (!snapshot)
            return 2;
        memcpy(snapshot, &native, sizeof(*snapshot));
        exit->f64_exit = snapshot;
        exit->pc = native.pc;
    }
    return status;
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static bool qjs_jit_enter(JSRuntime *rt, JSFunctionBytecode *b, const QJSJitArgs *args,
                          uint32_t word_offset, const void *locals, JSValue *result,
                          QJSJitExit *exit)
{
    const uint32_t *entry = (const uint32_t *)b->jit_code + word_offset;
    bool f64 = b->jit_f64;
    if (f64 && js_check_stack_overflow(rt, QJS_JIT_NATIVE_STACK + sizeof(QJSJitF64Exit)))
        return false;
    int status;
    qjs_jit_cache_enter(rt, b);
    if (f64) {
        status = qjs_jit_enter_f64(rt, (QJSJitF64Entry *)entry, args->f64, locals, result, exit);
    } else {
        status = ((QJSJitEntry *)entry)(args->i32, exit, QJS_JIT_FUEL, locals);
    }
    qjs_jit_cache_leave(b);
    if (status != 1) {
        bool fuel = status == 2 || status == QJS_JIT_EXIT_PRECISE_FUEL;
        if (fuel)
            rt->jit_fuel_exits++;
        else
            rt->jit_guard_exits++;
        if (status == QJS_JIT_EXIT_PRECISE_GUARD || status == QJS_JIT_EXIT_PRECISE_FUEL) {
            if (!f64)
                exit->f64_exit = NULL;
            rt->jit_precise_exits++;
        }
        if (status == QJS_JIT_EXIT_PRECISE_FUEL)
            rt->jit_precise_fuel_exits++;
        if (word_offset)
            rt->jit_osr_exits++;
        /* Complete this activation through its original interpreter exit.
           A successful promotion replaces the code for subsequent calls. */
        if (!fuel && !f64 && qjs_jit_promote(rt, b))
            return false;
        b->jit_retry_delay = QJS_JIT_RETRY_DELAY << b->jit_failure_streak;
        if (b->jit_failure_streak < QJS_JIT_MAX_FAILURE_STREAK - 1)
            b->jit_failure_streak++;
        return false;
    }
    if (f64)
        rt->jit_f64_hits++;
    b->jit_failure_streak = 0;
    if (word_offset)
        rt->jit_osr_hits++;
    if (b->jit_optimized) {
        rt->jit_optimized_hits++;
    } else {
        rt->jit_baseline_hits++;
        if (b->jit_baseline_hits < QJS_JIT_OPTIMIZED_THRESHOLD)
            b->jit_baseline_hits++;
    }
    if (!f64)
        *result = js_int32(exit->result);
    return true;
}

static void qjs_jit_discard_exit(JSRuntime *rt, QJSJitExit *exit)
{
    if (exit->pc != UINT32_MAX && exit->f64_exit) {
        js_free_rt(rt, exit->f64_exit);
        exit->f64_exit = NULL;
    }
    exit->pc = UINT32_MAX;
}

static void qjs_jit_resume(JSContext *ctx, const JSFunctionBytecode *b, QJSJitExit *exit,
                           JSValue *vars, JSValue *stack, uint8_t **pc, JSValue **sp)
{
    const QJSJitF64Exit *f64 = exit->f64_exit;
    uint32_t depth = f64 ? f64->depth : exit->depth;
    uint32_t initialized = f64 ? f64->initialized : exit->initialized;
    uint32_t uninitialized = f64 ? f64->uninitialized : exit->uninitialized;
    assert(exit->pc < (uint32_t)b->byte_code_len && depth <= b->stack_size);
    assert(*sp == stack);
    for (unsigned i = 0; i < b->var_count; i++) {
        uint32_t bit = UINT32_C(1) << i;
        JS_FreeValue(ctx, vars[i]);
        if (initialized & bit)
            vars[i] = f64 ? js_number(f64->locals[i]) : js_int32(exit->locals[i]);
        else
            vars[i] = uninitialized & bit ? JS_UNINITIALIZED : JS_UNDEFINED;
    }
    for (unsigned i = 0; i < depth; i++)
        stack[i] = f64 ? js_number(f64->stack[i]) : js_int32(exit->stack[i]);
    *sp = stack + depth;
    *pc = b->byte_code_buf + exit->pc;
    qjs_jit_discard_exit(ctx->rt, exit);
}

static bool qjs_jit_compile_baseline(JSRuntime *rt, JSFunctionBytecode *b, bool f64)
{
    if (!f64 && !b->jit_int_rejected) {
        QJSJitCompileResult result = qjs_jit_compile(rt, b, false, false);
        if (result == QJS_JIT_COMPILE_READY)
            return true;
        if (result == QJS_JIT_COMPILE_RETRY)
            return false;
        b->jit_int_rejected = true;
    }
    if (qjs_jit_try_f64(rt, b, false))
        return true;
    b->jit_rejected = b->jit_int_rejected && b->jit_f64_rejected;
    return false;
}

static bool qjs_jit_prepare(JSRuntime *rt, JSFunctionBytecode *b, const JSValue *args,
                             bool from_call, QJSJitArgs *unboxed)
{
    bool f64 = b->jit_f64 || !qjs_jit_unbox_int_args(b, args, unboxed->i32);
    if (f64 && !qjs_jit_unbox_float_args(b, args, unboxed->f64))
        return false;
    if (!b->jit_code) {
        if (from_call && b->jit_hot_calls < QJS_JIT_BASELINE_THRESHOLD &&
            ++b->jit_hot_calls < QJS_JIT_BASELINE_THRESHOLD)
            return false;
        if (!qjs_jit_compile_baseline(rt, b, f64))
            return false;
    } else if (f64 && !b->jit_f64) {
        if (!qjs_jit_promote(rt, b))
            return false;
    } else if (from_call && rt->jit_optimize && !b->jit_optimization_attempted &&
               b->jit_baseline_hits >= QJS_JIT_OPTIMIZED_THRESHOLD) {
        b->jit_optimization_attempted =
            qjs_jit_compile(rt, b, true, b->jit_f64) != QJS_JIT_COMPILE_RETRY;
    }
    if (b->jit_f64 && !f64)
        return qjs_jit_unbox_float_args(b, args, unboxed->f64);
    return true;
}

static bool qjs_jit_call(JSRuntime *rt, JSFunctionBytecode *b, const JSValue *args,
                         JSValue *result, QJSJitExit *exit)
{
    qjs_jit_cache_tick(rt);
    if (b->jit_rejected)
        return false;
    if (b->arg_count > QJS_JIT_MAX_ARGS || b->var_count > QJS_JIT_MAX_LOCALS ||
        b->var_ref_count || b->closure_var_count || b->func_kind != JS_FUNC_NORMAL) {
        b->jit_rejected = true;
        return false;
    }
    if (b->jit_retry_delay) {
        b->jit_retry_delay--;
        return false;
    }
    if (js_check_stack_overflow(rt, QJS_JIT_NATIVE_STACK))
        return false;

    QJSJitArgs unboxed;
    if (!qjs_jit_prepare(rt, b, args, true, &unboxed))
        return false;
    return qjs_jit_enter(rt, b, &unboxed, 0, NULL, result, exit);
}

static bool qjs_jit_osr(JSRuntime *rt, JSFunctionBytecode *b, const JSValue *args,
                        const JSValue *vars, uint32_t pc, bool *attempted, JSValue *result,
                        QJSJitExit *exit)
{
    if (b->jit_rejected || b->jit_retry_delay)
        return false;
    if (!b->jit_code && b->jit_hot_backedges < QJS_JIT_OSR_THRESHOLD &&
        ++b->jit_hot_backedges < QJS_JIT_OSR_THRESHOLD)
        return false;
    if (b->jit_code && (!b->jit_osr_word || b->jit_osr_pc != pc))
        return false;

    qjs_jit_cache_tick(rt);
    *attempted = true;
    if (js_check_stack_overflow(rt, QJS_JIT_NATIVE_STACK))
        return false;
    QJSJitArgs unboxed;
    if (!qjs_jit_prepare(rt, b, args, false, &unboxed))
        return false;
    if (!b->jit_osr_word || b->jit_osr_pc != pc) {
        /* The next back edge may reach the selected loop header. */
        *attempted = !b->jit_osr_word;
        return false;
    }

    if (!b->jit_f64) {
        int32_t locals[QJS_JIT_MAX_LOCALS];
        uint32_t mask = b->jit_osr_locals;
        for (; mask; mask &= mask - 1) {
            unsigned i = __builtin_ctz(mask);
            if (JS_VALUE_GET_TAG(vars[i]) != JS_TAG_INT) {
                if (!JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(vars[i])))
                    return false;
                break;
            }
            locals[i] = JS_VALUE_GET_INT(vars[i]);
        }
        if (!mask)
            return qjs_jit_enter(rt, b, &unboxed, b->jit_osr_word, locals, result, exit);
        if (!qjs_jit_promote(rt, b) || !b->jit_osr_word || b->jit_osr_pc != pc ||
            !qjs_jit_unbox_float_args(b, args, unboxed.f64))
            return false;
    }
    double locals[QJS_JIT_MAX_LOCALS];
    for (uint32_t mask = b->jit_osr_locals; mask; mask &= mask - 1) {
        unsigned i = __builtin_ctz(mask);
        if (!js_arith_to_float64(vars[i], &locals[i]))
            return false;
    }
    return qjs_jit_enter(rt, b, &unboxed, b->jit_osr_word, locals, result, exit);
}
