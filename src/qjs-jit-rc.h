/* Direct RC is available for QuickJS's struct representation of JSValue.
   Alternate representations keep using the existing semantic helpers. */
#if !defined(JS_CHECK_JSVALUE) && (!defined(JS_NAN_BOXING) || !JS_NAN_BOXING)
#define QJS_BOXED_NATIVE_RC 1

_Static_assert(sizeof(JSValue) == 16 && offsetof(JSValue, u) == 0 &&
               offsetof(JSValue, tag) == 8, "boxed RC value layout");
_Static_assert(sizeof(((JSMallocBlockHeader *)0)->ref_count) == 4 &&
               offsetof(JSMallocBlockHeader, ref_count) < offsetof(JSMallocBlockHeader, user_data) &&
               offsetof(JSMallocBlockHeader, user_data) - offsetof(JSMallocBlockHeader, ref_count) <= 256,
               "boxed RC arena layout");
_Static_assert(JS_TAG_FIRST < 0 && JS_TAG_FIRST >= -4095, "boxed RC tag range");
_Static_assert(sizeof(QJSBoxedFrame) <= 16384 && sizeof(JSStackFrame) <= 32768,
               "boxed RC frame offsets");

enum {
    QJS_RC_HEADER_IMM = ((int)offsetof(JSMallocBlockHeader, ref_count) -
                         (int)offsetof(JSMallocBlockHeader, user_data)) & 0x1ff,
    QJS_RC_LO = 3,
};

static void qjs_boxed_rc_note(QJSArm64Emitter *e, size_t offset)
{
    unsigned imm = (offset / sizeof(uint32_t)) << 10;
    qjs_arm64_emit(e, 0xb940026d | imm); /* ldr w13, [x19, #counter] */
    qjs_arm64_emit(e, 0x110005ad); /* add w13, w13, #1 */
    qjs_arm64_emit(e, 0xb900026d | imm); /* str w13, [x19, #counter] */
}

static void qjs_boxed_rc_bind(QJSArm64Emitter *e, size_t branch)
{
    if (!e->failed)
        e->code[branch] |= (uint32_t)(e->count - branch) << 5;
}

static bool qjs_boxed_emit_rc_slot(QJSArm64Emitter *e, uint32_t operand, bool stats)
{
    if (operand & ~(UINT16_MAX | QJS_BOXED_SLOT_ARG))
        return false;

    unsigned frame_offset = (operand & QJS_BOXED_SLOT_ARG) ?
                            offsetof(JSStackFrame, arg_buf) : offsetof(JSStackFrame, var_buf);
    unsigned slot_offset = (operand & UINT16_MAX) * sizeof(JSValue);
    unsigned sp_imm = (offsetof(QJSBoxedFrame, sp) / 8) << 10;
    qjs_arm64_emit(e, 0xf9400269 | ((offsetof(QJSBoxedFrame, sf) / 8) << 10)); /* ldr x9, [x19, #sf] */
    qjs_arm64_emit(e, 0xf9400129 | ((frame_offset / 8) << 10)); /* ldr x9, [x9, #buf] */
    if (slot_offset <= 4095) {
        if (slot_offset)
            qjs_arm64_emit(e, 0x91000129 | (slot_offset << 10)); /* add x9, x9, #offset */
    } else {
        qjs_arm64_immediate(e, 16, slot_offset);
        qjs_arm64_emit(e, 0x8b100129); /* add x9, x9, x16 */
    }
    qjs_arm64_emit(e, 0xa9402d2a); /* ldp x10, x11, [x9] */
    /* The carry test matches unsigned JS_VALUE_HAS_REF_COUNT. */
    qjs_arm64_emit(e, 0x3100017f | ((-JS_TAG_FIRST) << 10)); /* cmn w11, #-JS_TAG_FIRST */
    size_t non_ref = e->count;
    qjs_arm64_emit(e, 0x54000000 | QJS_RC_LO);
    qjs_arm64_emit(e, 0xb840014c | (QJS_RC_HEADER_IMM << 12)); /* ldur w12, [x10, #rc] */
    qjs_arm64_emit(e, 0x1100058c); /* add w12, w12, #1 */
    qjs_arm64_emit(e, 0xb800014c | (QJS_RC_HEADER_IMM << 12)); /* stur w12, [x10, #rc] */
    if (stats)
        qjs_boxed_rc_note(e, offsetof(QJSBoxedFrame, native_increfs));
    qjs_boxed_rc_bind(e, non_ref);
    qjs_arm64_emit(e, 0xf940026c | sp_imm); /* ldr x12, [x19, #sp] */
    qjs_arm64_emit(e, 0xa8812d8a); /* stp x10, x11, [x12], #16 */
    qjs_arm64_emit(e, 0xf900026c | sp_imm); /* str x12, [x19, #sp] */
    return true;
}

/* Last references retain the original operand on the frame stack and fall
   through to the ordinary C helper, which runs finalizers and cycle teardown. */
static void qjs_boxed_emit_rc_drop(QJSArm64Emitter *e, unsigned next, bool stats)
{
    unsigned sp_imm = (offsetof(QJSBoxedFrame, sp) / 8) << 10;
    qjs_arm64_emit(e, 0xf9400269 | sp_imm); /* ldr x9, [x19, #sp] */
    qjs_arm64_emit(e, 0xa97f2d2a); /* ldp x10, x11, [x9, #-16] */
    qjs_arm64_emit(e, 0x3100017f | ((-JS_TAG_FIRST) << 10)); /* cmn w11, #-JS_TAG_FIRST */
    size_t non_ref = e->count;
    qjs_arm64_emit(e, 0x54000000 | QJS_RC_LO);
    qjs_arm64_emit(e, 0xb840014c | (QJS_RC_HEADER_IMM << 12)); /* ldur w12, [x10, #rc] */
    qjs_arm64_emit(e, 0x7100059f); /* cmp w12, #1 */
    size_t final_ref = e->count;
    qjs_arm64_emit(e, 0x54000000 | QJS_ARM64_LE);
    qjs_arm64_emit(e, 0x5100058c); /* sub w12, w12, #1 */
    qjs_arm64_emit(e, 0xb800014c | (QJS_RC_HEADER_IMM << 12)); /* stur w12, [x10, #rc] */
    if (stats)
        qjs_boxed_rc_note(e, offsetof(QJSBoxedFrame, native_decrefs));
    qjs_boxed_rc_bind(e, non_ref);
    qjs_arm64_emit(e, 0xd1004129); /* sub x9, x9, #16 */
    qjs_arm64_emit(e, 0xf9000269 | sp_imm); /* str x9, [x19, #sp] */
    qjs_arm64_jump(e, 0x14000000, next);
    qjs_boxed_rc_bind(e, final_ref);
    if (stats)
        qjs_boxed_rc_note(e, offsetof(QJSBoxedFrame, native_drop_fallbacks));
}
#endif
