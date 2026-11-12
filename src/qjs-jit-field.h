/* Included after the boxed frame and AArch64 RC helpers. */
enum {
    QJS_JIT_NATIVE_FIELD_MAX_SITES = 4,
    QJS_JIT_NATIVE_FIELD_MIN_HITS = 8,
};

#if defined(QJS_BOXED_NATIVE_RC) && !defined(ENABLE_DUMPS)
_Static_assert(sizeof(JSProperty) == 16 && offsetof(JSProperty, u.value) == 0 &&
               sizeof(JSShapeProperty) == 8 && offsetof(JSShapeProperty, atom) == 4,
               "native field slot layout");
_Static_assert(JS_TAG_OBJECT < 0 && JS_TAG_OBJECT >= -4095 && JS_CLASS_OBJECT <= 4095,
               "native field guards must fit AArch64 immediates");

static void qjs_boxed_field_shape_id(QJSArm64Emitter *e, unsigned reg, uint64_t shape)
{
    qjs_arm64_emit(e, 0xd2800000 | ((shape & UINT16_MAX) << 5) | reg); /* movz xreg */
    for (unsigned half = 1; half < 4; ++half) {
        unsigned bits = (shape >> (half * 16)) & UINT16_MAX;
        if (bits)
            qjs_arm64_emit(e, 0xf2800000 | (half << 21) | (bits << 5) | reg); /* movk xreg */
    }
}

static bool qjs_boxed_emit_field(QJSArm64Emitter *e, const JSFunctionBytecode *b,
                                 unsigned pc, unsigned next, unsigned operand,
                                 bool borrowed, bool stats)
{
    const QJSJitFeedbackSite *site = qjs_jit_feedback_lookup(b, pc, QJS_JIT_FEEDBACK_PROPERTY);
    if (!site || site->u.property.count != 1 || site->u.property.megamorphic ||
        site->u.property.hits < QJS_JIT_NATIVE_FIELD_MIN_HITS)
        return false;

    unsigned slot = site->u.property.entries[0].slot;
    /* An LDP with two 64-bit values uses a signed, scaled 7-bit offset. */
    if (slot * (uint64_t)sizeof(JSProperty) > 63 * sizeof(uint64_t))
        return false;
    if (borrowed && (operand & ~(UINT16_MAX | QJS_BOXED_SLOT_ARG)))
        return false;

    JSShapeProperty special = { .flags = JS_PROP_TMASK };
    uint32_t type_mask;
    memcpy(&type_mask, &special, sizeof(type_mask));

    qjs_inline_load(e, 8, 19, offsetof(QJSBoxedFrame, sp), 3);
    if (borrowed) {
        size_t buffer = (operand & QJS_BOXED_SLOT_ARG) ? offsetof(JSStackFrame, arg_buf) :
                                                        offsetof(JSStackFrame, var_buf);
        unsigned offset = (operand & UINT16_MAX) * sizeof(JSValue);
        qjs_inline_load(e, 9, 19, offsetof(QJSBoxedFrame, sf), 3);
        qjs_inline_load(e, 9, 9, buffer, 3);
        if (offset <= 4095) {
            if (offset)
                qjs_arm64_emit(e, 0x91000129 | (offset << 10)); /* add x9, x9, #offset */
        } else {
            qjs_arm64_immediate(e, 16, offset);
            qjs_arm64_emit(e, 0x8b100129); /* add x9, x9, x16 */
        }
    } else {
        qjs_arm64_emit(e, 0xd1004109); /* sub x9, x8, #16 */
    }

    QJSInlineGuards guards = { 0 };
    qjs_inline_load(e, 10, 9, offsetof(JSValue, tag), 3);
    qjs_arm64_emit(e, 0xb100015f | ((-JS_TAG_OBJECT) << 10)); /* cmn x10, #-JS_TAG_OBJECT */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);
    qjs_inline_load(e, 9, 9, offsetof(JSValue, u), 3);
    qjs_inline_load(e, 10, 9, offsetof(JSObject, class_id), 1);
    /* JS_CLASS_OBJECT always uses ordinary own-property lookup. */
    qjs_arm64_emit(e, 0x7100015f | (JS_CLASS_OBJECT << 10)); /* cmp w10, #class */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);
    qjs_inline_load(e, 10, 9, offsetof(JSObject, shape), 3);
    qjs_boxed_field_shape_id(e, 11, site->u.property.entries[0].shape);
    qjs_arm64_emit(e, qjs_arm64_rrr(0xeb000000, 31, 10, 11)); /* cmp x10, x11 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);

    /* A recycled shape address is safe only after checking the live slot. */
    qjs_inline_load(e, 11, 10, offsetof(JSShape, prop_count), 2);
    qjs_arm64_emit(e, 0x7100017f | ((slot + 1) << 10)); /* cmp w11, #slot + 1 */
    qjs_inline_guard(e, &guards, 0x54000000 | (QJS_ARM64_HS ^ 1));
    qjs_inline_load(e, 11, 10, offsetof(JSShape, prop_hash_mask), 2);
    qjs_arm64_emit(e, qjs_arm64_rrr(0x8b000800, 10, 10, 11)); /* add x10, x10, x11, lsl #2 */
    size_t descriptor = offsetof(JSShape, hash_table) + sizeof(uint32_t) +
                        slot * sizeof(JSShapeProperty);
    qjs_inline_load(e, 11, 10, descriptor + offsetof(JSShapeProperty, atom), 2);
    qjs_arm64_immediate(e, 12, get_u32(b->byte_code_buf + pc + 1));
    qjs_arm64_emit(e, qjs_arm64_rrr(0x6b000000, 31, 11, 12)); /* cmp w11, w12 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);
    qjs_inline_load(e, 11, 10, descriptor, 2);
    qjs_arm64_immediate(e, 12, type_mask);
    qjs_arm64_emit(e, qjs_arm64_rrr(0x6a000000, 31, 11, 12)); /* tst w11, w12 */
    qjs_inline_guard(e, &guards, 0x54000000 | QJS_ARM64_NE);

    qjs_inline_load(e, 9, 9, offsetof(JSObject, prop), 3);
    qjs_arm64_emit(e, 0xa9402d2a | ((slot * sizeof(JSProperty) / 8) << 15)); /* ldp x10, x11 */
    /* Duplicate the property value before exposing it on the VM stack. */
    qjs_arm64_emit(e, 0x3100017f | ((-JS_TAG_FIRST) << 10)); /* cmn w11, #-JS_TAG_FIRST */
    size_t non_ref = e->count;
    qjs_arm64_emit(e, 0x54000000 | QJS_RC_LO);
    qjs_arm64_emit(e, 0xb840014c | (QJS_RC_HEADER_IMM << 12)); /* ldur w12, [x10, #rc] */
    qjs_arm64_emit(e, 0x1100058c); /* add w12, w12, #1 */
    qjs_arm64_emit(e, 0xb800014c | (QJS_RC_HEADER_IMM << 12)); /* stur w12, [x10, #rc] */
    if (stats)
        qjs_boxed_rc_note(e, offsetof(QJSBoxedFrame, native_increfs));
    qjs_boxed_rc_bind(e, non_ref);
    qjs_arm64_emit(e, 0xa8812d0a); /* stp x10, x11, [x8], #16 */
    qjs_arm64_emit(e, 0xf9000268 | ((offsetof(QJSBoxedFrame, sp) / 8) << 10)); /* str x8 */
    if (stats)
        qjs_boxed_rc_note(e, borrowed ? offsetof(QJSBoxedFrame, native_borrowed_fields) :
                                        offsetof(QJSBoxedFrame, native_retained_fields));
    qjs_arm64_jump(e, 0x14000000, next);
    for (unsigned i = 0; i < guards.count; ++i)
        qjs_boxed_rc_bind(e, guards.words[i]);
    return true;
}
#else
static bool qjs_boxed_emit_field(QJSArm64Emitter *e, const JSFunctionBytecode *b,
                                 unsigned pc, unsigned next, unsigned operand,
                                 bool borrowed, bool stats)
{
    return false;
}
#endif
