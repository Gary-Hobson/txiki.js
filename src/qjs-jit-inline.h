/* Included by qjs-jit-boxed.h after the frame and helper declarations. */

enum {
    QJS_JIT_INLINE_MAX_SITES = 4,
    QJS_JIT_INLINE_MIN_SAMPLES = 8,
};

#if !defined(ENABLE_DUMPS) && !defined(JS_CHECK_JSVALUE) && \
    (!defined(JS_NAN_BOXING) || !JS_NAN_BOXING)
_Static_assert(sizeof(JSValue) == 16 && offsetof(JSValue, u) == 0 &&
               offsetof(JSValue, tag) == 8 && JS_TAG_OBJECT == -1 && JS_TAG_INT == 0,
               "The boxed inliner requires the AArch64 QuickJS value layout");
_Static_assert(offsetof(QJSBoxedFrame, result) % 8 == 0 &&
               offsetof(QJSBoxedFrame, result) / 8 <= 63,
               "The boxed result must fit an AArch64 paired store");

typedef struct {
    size_t words[12];
    unsigned count;
} QJSInlineGuards;

static void qjs_inline_guard(QJSArm64Emitter *e, QJSInlineGuards *guards, uint32_t branch)
{
    if (guards->count == countof(guards->words)) {
        e->failed = true;
        return;
    }
    guards->words[guards->count++] = e->count;
    qjs_arm64_emit(e, branch);
}

static void qjs_inline_load(QJSArm64Emitter *e, int dst, int base, size_t offset, unsigned shift)
{
    static const uint32_t load[] = { 0, 0x79400000, 0xb9400000, 0xf9400000 };
    if ((offset & ((1u << shift) - 1)) || (offset >> shift) > 4095) {
        e->failed = true;
        return;
    }
    qjs_arm64_emit(e, load[shift] | ((offset >> shift) << 10) | (base << 5) | dst);
}

static void qjs_inline_store_w(QJSArm64Emitter *e, int src, int base, size_t offset)
{
    if ((offset & 3) || (offset >> 2) > 4095) {
        e->failed = true;
        return;
    }
    qjs_arm64_emit(e, 0xb9000000 | ((offset >> 2) << 10) | (base << 5) | src);
}

static bool qjs_boxed_emit_inline_tail(QJSArm64Emitter *e, const JSFunctionBytecode *caller,
                                       uint32_t pc, uint32_t next, uint32_t argc)
{
    int op = caller->byte_code_buf[pc];
    if (argc != 1 || (op != OP_tail_call &&
        ((op != OP_call && op != OP_call1) || next >= (uint32_t)caller->byte_code_len ||
         caller->byte_code_buf[next] != OP_return)))
        return false;

    const QJSJitFeedbackSite *site = qjs_jit_feedback_lookup(caller, pc, QJS_JIT_FEEDBACK_CALL);
    if (!site || site->u.call.count != 1 || site->u.call.other || !site->u.call.inline_valid ||
        site->u.call.targets[0].count < QJS_JIT_INLINE_MIN_SAMPLES)
        return false;

    QJSInlineGuards guards = { 0 };
    qjs_inline_load(e, 4, 19, offsetof(QJSBoxedFrame, ctx), 3);
    qjs_inline_load(e, 5, 4, offsetof(JSContext, rt), 3);
    qjs_inline_load(e, 6, 5, offsetof(JSRuntime, interrupt_handler), 3);
    qjs_inline_guard(e, &guards, 0xb5000006); /* cbnz x6 */
#ifdef BUILD_WITH_INSPECTOR
    qjs_inline_load(e, 6, 4, offsetof(JSContext, debug_trace), 3);
    qjs_inline_guard(e, &guards, 0xb5000006);
#endif
    qjs_inline_load(e, 6, 5, offsetof(JSRuntime, stack_limit), 3);
    qjs_arm64_emit(e, 0xd12003e7); /* sub x7, sp, #2048 */
    qjs_arm64_emit(e, qjs_arm64_rrr(0xeb000000, 31, 7, 6)); /* cmp x7, x6 */
    qjs_inline_guard(e, &guards, 0x54000000 | (QJS_ARM64_HS ^ 1));
    qjs_inline_load(e, 7, 4, offsetof(JSContext, interrupt_counter), 2);
    qjs_arm64_emit(e, 0x710004e7); /* subs w7, w7, #1 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_LE);

    qjs_inline_load(e, 8, 19, offsetof(QJSBoxedFrame, sp), 3);
    qjs_arm64_emit(e, 0xd1008108); /* sub x8, x8, #2 * sizeof(JSValue) */
    qjs_inline_load(e, 9, 8, offsetof(JSValue, tag), 3);
    qjs_arm64_emit(e, 0xb100053f); /* cmn x9, #1 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);
    qjs_inline_load(e, 9, 8, offsetof(JSValue, u), 3);
    qjs_inline_load(e, 10, 9, offsetof(JSObject, class_id), 1);
    qjs_arm64_emit(e, 0x7100015f | (JS_CLASS_BYTECODE_FUNCTION << 10)); /* cmp w10, #class */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);
    qjs_inline_load(e, 9, 9, offsetof(JSObject, u.func.function_bytecode), 3);
    qjs_inline_load(e, 10, 9, offsetof(JSFunctionBytecode, jit_feedback_target_id), 2);
    qjs_arm64_immediate(e, 11, site->u.call.targets[0].id);
    qjs_arm64_emit(e, qjs_arm64_rrr(0x6b000000, 31, 10, 11)); /* cmp w10, w11 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);
    qjs_inline_load(e, 10, 9, offsetof(JSFunctionBytecode, realm), 3);
    qjs_arm64_emit(e, qjs_arm64_rrr(0xeb000000, 31, 10, 4)); /* cmp x10, x4 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);

    qjs_inline_load(e, 9, 8, sizeof(JSValue) + offsetof(JSValue, tag), 3);
    qjs_inline_guard(e, &guards, 0xb5000009); /* cbnz x9 */
    qjs_inline_load(e, 9, 8, sizeof(JSValue) + offsetof(JSValue, u), 2);
    int delta = site->u.call.inline_delta;
    qjs_arm64_emit(e, (delta < 0 ? 0x71000120 : 0x31000120) |
                      ((uint32_t)abs(delta) << 10)); /* subs/adds w0, w9, #abs(delta) */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_VS);

    /* A tail return leaves the rooted callee and argument on the stack.
       The normal interpreter epilogue releases both exactly once. */
    qjs_inline_store_w(e, 7, 4, offsetof(JSContext, interrupt_counter));
    qjs_arm64_immediate(e, 9, pc + 1);
    qjs_inline_store_w(e, 9, 19, offsetof(QJSBoxedFrame, inline_pc_plus_one));
    qjs_arm64_emit(e, 0xa9007e60 | ((offsetof(QJSBoxedFrame, result) / 8) << 15));
    qjs_arm64_immediate(e, 0, QJS_BOXED_RETURN);
    qjs_arm64_jump(e, 0x14000000, QJS_BOXED_EPILOGUE);

    if (!e->failed) {
        for (unsigned i = 0; i < guards.count; i++) {
            size_t word = guards.words[i];
            e->code[word] |= (e->count - word) << 5;
        }
    }
    return true;
}
#else
static bool qjs_boxed_emit_inline_tail(QJSArm64Emitter *e, const JSFunctionBytecode *caller,
                                       uint32_t pc, uint32_t next, uint32_t argc)
{
    return false;
}
#endif
