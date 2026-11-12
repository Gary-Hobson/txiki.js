/* Included after qjs-jit.h. Boxed values live in the interpreter's own frame.
   Native code carries only control state across calls to semantic helpers. */
enum {
    QJS_BOXED_MAX_BYTECODE = QJS_ARM64_MAX_BYTECODE,
    QJS_BOXED_EPILOGUE = QJS_BOXED_MAX_BYTECODE,
};

typedef struct {
    JSContext *ctx;
    JSContext *caller_ctx;
    JSFunctionBytecode *b;
    JSStackFrame *sf;
    JSVarRef **var_refs;
    JSValue this_obj;
    JSValue *sp;
    JSValue result;
    uint32_t pc;
    uint32_t fuel;
    uint32_t inline_pc_plus_one;
    uint32_t native_shuffles;
    uint32_t native_increfs, native_decrefs, native_drop_fallbacks;
    uint32_t native_short_bigints;
    uint32_t native_borrowed_fields, native_retained_fields;
    JSValue new_target;
    int argc;
    JSValueConst *argv;
} QJSBoxedFrame;

typedef int QJSBoxedHelper(QJSBoxedFrame *, uint32_t, uint32_t);
typedef int QJSBoxedEntry(QJSBoxedFrame *, QJSBoxedHelper *const *);

static void qjs_boxed_safepoint(QJSBoxedFrame *f)
{
    f->sf->cur_pc = f->b->byte_code_buf + f->pc;
}

static int qjs_boxed_push_int(QJSBoxedFrame *f, uint32_t value, uint32_t pc)
{
    *f->sp++ = js_int32((int32_t)value);
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_push_const(QJSBoxedFrame *f, uint32_t index, uint32_t pc)
{
    *f->sp++ = js_dup(f->b->cpool[index]);
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_push_short_bigint(QJSBoxedFrame *f, uint32_t value, uint32_t pc)
{
    *f->sp++ = __JS_NewShortBigInt(f->ctx, (int32_t)value);
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_push_primitive(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    switch (opcode) {
    case OP_undefined: *f->sp++ = JS_UNDEFINED; break;
    case OP_null: *f->sp++ = JS_NULL; break;
    case OP_push_true: *f->sp++ = JS_TRUE; break;
    case OP_push_false: *f->sp++ = JS_FALSE; break;
    default: abort();
    }
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_apply_eval(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSRuntime *rt = ctx->rt;

    qjs_boxed_safepoint(f);
    if (rt->jit_stats) {
        if (opcode == OP_apply)
            rt->jit_boxed_applies++;
        else if (opcode == OP_eval)
            rt->jit_boxed_evals++;
        else
            rt->jit_boxed_apply_evals++;
    }
    return js_op_apply_or_eval(ctx, &f->sp, opcode, f->b->byte_code_buf + pc + 1) < 0 ?
           QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_regexp_import(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue *sp = f->sp;
    JSValue result;

    qjs_boxed_safepoint(f);
    if (opcode == OP_regexp) {
        ctx->rt->jit_boxed_regexps++;
        /* The constructor takes both operands, including on failure. */
        result = js_regexp_constructor_internal(ctx, JS_UNDEFINED, sp[-2], sp[-1]);
    } else {
        assert(opcode == OP_import);
        ctx->rt->jit_boxed_dynamic_imports++;
        /* Import borrows the operands and can reenter JS during coercion. */
        result = js_dynamic_import(ctx, sp[-2], sp[-1]);
        if (unlikely(JS_IsException(result)))
            return QJS_BOXED_EXCEPTION;
        JS_FreeValue(ctx, sp[-2]);
        JS_FreeValue(ctx, sp[-1]);
    }
    sp[-2] = result;
    f->sp = sp - 1;
    return unlikely(JS_IsException(result)) ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_create(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue val;

    qjs_boxed_safepoint(f);
    switch (f->b->byte_code_buf[pc]) {
    case OP_fclosure:
    case OP_fclosure8:
        val = js_closure(ctx, js_dup(f->b->cpool[operand]), f->var_refs, f->sf);
        break;
    case OP_push_atom_value:
        val = JS_AtomToValue(ctx, operand);
        break;
    case OP_push_empty_string:
        val = js_empty_string(ctx->rt);
        break;
    case OP_push_this:
        if (f->b->is_strict_mode || JS_IsObject(f->this_obj))
            val = js_dup(f->this_obj);
        else if (JS_IsNull(f->this_obj) || JS_IsUndefined(f->this_obj))
            val = js_dup(ctx->global_obj);
        else
            val = JS_ToObject(ctx, f->this_obj);
        break;
    default:
        abort();
    }
    if (unlikely(JS_IsException(val)))
        return QJS_BOXED_EXCEPTION;
    *f->sp++ = val;
    return QJS_BOXED_CONTINUE;
}

enum {
    QJS_BOXED_SLOT_ARG = 1u << 16,
    QJS_BOXED_SLOT_REF = 1u << 17,
    QJS_BOXED_SLOT_PUT = 1u << 18,
    QJS_BOXED_SLOT_SET = 1u << 19,
    QJS_BOXED_SLOT_TDZ = 1u << 20,
    QJS_BOXED_SLOT_INIT = 1u << 21,
    QJS_BOXED_SLOT_RESET = 1u << 22,
};

static JSValue *qjs_boxed_slot_address(QJSBoxedFrame *f, uint32_t operand)
{
    uint16_t index = operand;
    return operand & QJS_BOXED_SLOT_REF ? f->var_refs[index]->pvalue :
           operand & QJS_BOXED_SLOT_ARG ? f->sf->arg_buf + index : f->sf->var_buf + index;
}

static int qjs_boxed_slot(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    uint16_t index = operand;
    JSValue *slot = qjs_boxed_slot_address(f, operand);
    if (operand & QJS_BOXED_SLOT_RESET) {
        set_value(f->ctx, slot, JS_UNINITIALIZED);
    } else {
        if ((operand & QJS_BOXED_SLOT_TDZ) && unlikely(JS_IsUninitialized(*slot))) {
            qjs_boxed_safepoint(f);
            bool ref = operand & QJS_BOXED_SLOT_REF;
            JS_ThrowReferenceErrorUninitialized2(ref ? f->ctx : f->caller_ctx,
                                                  f->b, index, ref);
            return QJS_BOXED_EXCEPTION;
        }
        if ((operand & QJS_BOXED_SLOT_INIT) && unlikely(!JS_IsUninitialized(*slot))) {
            qjs_boxed_safepoint(f);
            if (operand & QJS_BOXED_SLOT_REF)
                JS_ThrowReferenceErrorUninitialized2(f->ctx, f->b, index, true);
            else
                JS_ThrowReferenceError(f->caller_ctx, "'this' can be initialized only once");
            return QJS_BOXED_EXCEPTION;
        }
        if (operand & QJS_BOXED_SLOT_PUT)
            set_value(f->ctx, slot, *--f->sp);
        else if (operand & QJS_BOXED_SLOT_SET)
            set_value(f->ctx, slot, js_dup(f->sp[-1]));
        else
            *f->sp++ = js_dup(*slot);
    }
    return QJS_BOXED_CONTINUE;
}

typedef struct {
    uint8_t count;
    uint8_t source[5];
} QJSBoxedShuffle;

static const QJSBoxedShuffle qjs_boxed_shuffles[] = {
    [OP_perm3 - OP_perm3] = { 3, { 1, 0, 2 } },
    [OP_perm4 - OP_perm3] = { 4, { 2, 0, 1, 3 } },
    [OP_perm5 - OP_perm3] = { 5, { 3, 0, 1, 2, 4 } },
    [OP_swap - OP_perm3] = { 2, { 1, 0 } },
    [OP_swap2 - OP_perm3] = { 4, { 2, 3, 0, 1 } },
    [OP_rot3l - OP_perm3] = { 3, { 1, 2, 0 } },
    [OP_rot3r - OP_perm3] = { 3, { 2, 0, 1 } },
    [OP_rot4l - OP_perm3] = { 4, { 1, 2, 3, 0 } },
    [OP_rot5l - OP_perm3] = { 5, { 1, 2, 3, 4, 0 } },
};

static const QJSBoxedShuffle *qjs_boxed_shuffle(unsigned opcode)
{
    unsigned index = opcode - OP_perm3;
    return index < countof(qjs_boxed_shuffles) && qjs_boxed_shuffles[index].count ?
           &qjs_boxed_shuffles[index] : NULL;
}

static int qjs_boxed_stack(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    JSValue *sp = f->sp;

    switch (opcode) {
    case OP_drop:
        JS_FreeValue(f->ctx, *--sp);
        break;
    case OP_dup:
        sp[0] = js_dup(sp[-1]);
        sp++;
        break;
    case OP_dup2:
        sp[0] = js_dup(sp[-2]);
        sp[1] = js_dup(sp[-1]);
        sp += 2;
        break;
    case OP_dup3:
        sp[0] = js_dup(sp[-3]);
        sp[1] = js_dup(sp[-2]);
        sp[2] = js_dup(sp[-1]);
        sp += 3;
        break;
    case OP_nip:
        JS_FreeValue(f->ctx, sp[-2]);
        sp[-2] = sp[-1];
        sp--;
        break;
    case OP_nip1:
        JS_FreeValue(f->ctx, sp[-3]);
        sp[-3] = sp[-2];
        sp[-2] = sp[-1];
        sp--;
        break;
    case OP_dup1:
        sp[0] = sp[-1];
        sp[-1] = js_dup(sp[-2]);
        sp++;
        break;
    case OP_insert2: case OP_insert3: case OP_insert4:
        sp = js_stack_insert(sp, 2 + opcode - OP_insert2);
        f->ctx->rt->jit_boxed_stack_inserts++;
        break;
    case OP_get_loc0_loc1:
        *sp++ = js_dup(f->sf->var_buf[0]);
        *sp++ = js_dup(f->sf->var_buf[1]);
        break;
    default: {
        const QJSBoxedShuffle *shuffle = qjs_boxed_shuffle(opcode);
        assert(shuffle);
        JSValue *base = sp - shuffle->count;
        JSValue slots[5];
        for (unsigned i = 0; i < shuffle->count; i++)
            slots[i] = base[shuffle->source[i]];
        memcpy(base, slots, shuffle->count * sizeof(*base));
        break;
    }
    }
    f->sp = sp;
    return QJS_BOXED_CONTINUE;
}

static inline JSValue qjs_boxed_type_result(JSContext *ctx, JSValue value,
                                          unsigned opcode, bool owned)
{
    bool matches;

    switch (opcode) {
    case OP_is_undefined_or_null:
        matches = JS_IsUndefined(value) || JS_IsNull(value);
        break;
    case OP_is_undefined:
        matches = JS_IsUndefined(value);
        break;
    case OP_is_null:
        matches = JS_IsNull(value);
        break;
    case OP_typeof_is_undefined:
    case OP_typeof_is_function:
        matches = js_operator_typeof(ctx, value) ==
                  (opcode == OP_typeof_is_undefined ? JS_ATOM_undefined : JS_ATOM_function);
        break;
    case OP_typeof: {
        JSAtom atom = js_operator_typeof(ctx, value);
        if (owned)
            JS_FreeValue(ctx, value);
        return JS_AtomToString(ctx, atom);
    }
    default:
        abort();
    }
    if (owned)
        JS_FreeValue(ctx, value);
    return js_bool(matches);
}

static int qjs_boxed_type(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue *top = f->sp - 1;
    unsigned opcode = f->b->byte_code_buf[pc];

    ctx->rt->jit_boxed_type_checks++;
    if (opcode == OP_check_object) {
        if (unlikely(!JS_IsObject(*top))) {
            qjs_boxed_safepoint(f);
            JS_ThrowTypeErrorNotAnObject(ctx);
            return QJS_BOXED_EXCEPTION;
        }
    } else {
        *top = qjs_boxed_type_result(ctx, *top, opcode, true);
    }
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_borrow_type(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSRuntime *rt = f->ctx->rt;
    JSValue value = *qjs_boxed_slot_address(f, operand);

    /* Type predicates neither call user code nor modify the source slot. */
    *f->sp++ = qjs_boxed_type_result(f->ctx, value, f->b->byte_code_buf[pc], false);
    rt->jit_boxed_type_checks++;
    if (rt->jit_stats && JS_VALUE_HAS_REF_COUNT(value))
        rt->jit_boxed_rc_pairs_elided++;
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_membership(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue *sp = f->sp;
    int status;

    qjs_boxed_safepoint(f);
    switch (f->b->byte_code_buf[pc]) {
    case OP_in:
        ctx->rt->jit_boxed_membership++;
        status = js_operator_in(ctx, sp);
        break;
    case OP_private_in:
        ctx->rt->jit_boxed_membership++;
        status = js_operator_private_in(ctx, sp);
        break;
    case OP_instanceof:
        ctx->rt->jit_boxed_membership++;
        status = js_operator_instanceof(ctx, sp);
        break;
    case OP_delete:
        ctx->rt->jit_boxed_deletes++;
        status = js_operator_delete(ctx, sp);
        break;
    case OP_delete_var:
        ctx->rt->jit_boxed_deletes++;
        status = JS_DeleteGlobalVar(ctx, operand);
        if (unlikely(status < 0))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = js_bool(status);
        return QJS_BOXED_CONTINUE;
    default:
        abort();
    }
    if (unlikely(status < 0))
        return QJS_BOXED_EXCEPTION;
    f->sp--;
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_convert(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    f->ctx->rt->jit_boxed_conversions++;
    return js_op_convert(f->ctx, f->sf, f->b->byte_code_buf + f->pc, f->sp, opcode) < 0 ?
           QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_ctor(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    ctx->rt->jit_boxed_ctor_checks++;
    qjs_boxed_safepoint(f);
    switch (opcode) {
    case OP_check_ctor:
        if (js_op_check_ctor(ctx, f->new_target))
            return QJS_BOXED_EXCEPTION;
        break;
    case OP_check_ctor_return:
        if (js_op_check_ctor_return(f->caller_ctx, f->sp))
            return QJS_BOXED_EXCEPTION;
        f->sp++;
        break;
    case OP_init_ctor: {
        JSValue value = js_op_init_ctor(ctx, f->sf->cur_func, f->new_target, f->argc, f->argv);
        if (JS_IsException(value))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = value;
        break;
    }
    default:
        abort();
    }
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_special(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    f->ctx->rt->jit_boxed_special_objects++;
    qjs_boxed_safepoint(f);
    JSValue value = f->b->byte_code_buf[pc] == OP_rest ?
                    js_op_rest(f->ctx, f->argc, f->argv, operand) :
                    js_op_special_object(f->ctx, f->sf, f->b, operand,
                                         f->new_target, f->argc, f->argv);
    *f->sp++ = value;
    return JS_IsException(value) ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_reference(QJSBoxedFrame *f, uint32_t atom, uint32_t pc)
{
    f->ctx->rt->jit_boxed_references++;
    qjs_boxed_safepoint(f);
    unsigned opcode = f->b->byte_code_buf[pc];
    switch (opcode) {
    case OP_make_loc_ref: case OP_make_arg_ref: case OP_make_var_ref_ref:
        if (js_op_make_local_ref(f->ctx, f->sf, f->var_refs, &f->sp, atom,
                                 get_u16(f->b->byte_code_buf + pc + 5), opcode))
            return QJS_BOXED_EXCEPTION;
        break;
    case OP_make_var_ref:
        if (JS_GetGlobalVarRef(f->ctx, atom, f->sp))
            return QJS_BOXED_EXCEPTION;
        f->sp += 2;
        break;
    case OP_get_ref_value: {
        JSValue value = js_op_get_ref_value(f->ctx, f->sp);
        if (JS_IsException(value))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = value;
        break;
    }
    case OP_put_ref_value:
        if (js_op_put_ref_value(f->ctx, &f->sp))
            return QJS_BOXED_EXCEPTION;
        break;
    default:
        abort();
    }
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_class(QJSBoxedFrame *f, uint32_t atom, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSRuntime *rt = ctx->rt;
    unsigned opcode = f->b->byte_code_buf[pc];
    qjs_boxed_safepoint(f);
    switch (opcode) {
    case OP_get_super:
    case OP_get_super_value:
    case OP_put_super_value:
        if (opcode == OP_get_super_value)
            rt->jit_boxed_super_reads++;
        if (opcode == OP_put_super_value)
            rt->jit_boxed_super_writes++;
        return js_op_super(ctx, &f->sp, opcode) ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
    case OP_private_symbol: {
        JSValue symbol = JS_NewSymbolFromAtom(ctx, atom, JS_ATOM_TYPE_PRIVATE);
        if (JS_IsException(symbol))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = symbol;
        return QJS_BOXED_CONTINUE;
    }
    case OP_get_private_field:
    case OP_put_private_field:
    case OP_define_private_field:
    case OP_check_brand:
    case OP_add_brand:
        if (opcode == OP_get_private_field)
            rt->jit_boxed_private_reads++;
        if (opcode == OP_put_private_field || opcode == OP_define_private_field)
            rt->jit_boxed_private_writes++;
        if (opcode == OP_check_brand)
            rt->jit_boxed_brand_checks++;
        return js_op_private(ctx, &f->sp, opcode) ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
    default:
        abort();
    }
}

static int qjs_boxed_read_field(QJSBoxedFrame *f, JSValue obj, uint32_t atom, uint32_t pc,
                                bool borrowed)
{
    JSRuntime *rt = f->ctx->rt;
    JSValue val;
    JSProperty *pr = NULL;
    JSShapeProperty *prs = NULL;
    bool retain_receiver = f->b->byte_code_buf[pc] == OP_get_field2;

    rt->jit_boxed_fields++;
    if (JS_IsObject(obj)) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        prs = rt->jit_feedback_enabled ?
            qjs_jit_feedback_property(rt, f->b, f->b->byte_code_buf + pc, p, atom, &pr) :
            find_own_property(&pr, p, atom);
    }
    if (prs && !(prs->flags & JS_PROP_TMASK)) {
        val = js_dup(pr->u.value);
        if (borrowed) {
            *f->sp++ = val;
            rt->jit_boxed_rc_pairs_elided++;
            return QJS_BOXED_CONTINUE;
        }
    } else {
        /* An accessor can replace the local or closure reference. Hold the
           receiver in the VM stack before crossing into user code. */
        if (borrowed)
            *f->sp++ = js_dup(obj);
        qjs_boxed_safepoint(f);
        val = JS_GetPropertyInternal(f->ctx, obj, atom, obj, false);
        if (unlikely(JS_IsException(val)))
            return QJS_BOXED_EXCEPTION;
    }

    if (retain_receiver) {
        *f->sp++ = val;
    } else {
        JS_FreeValue(f->ctx, obj);
        f->sp[-1] = val;
    }
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_get_field(QJSBoxedFrame *f, uint32_t atom, uint32_t pc)
{
    return qjs_boxed_read_field(f, f->sp[-1], atom, pc, false);
}

static int qjs_boxed_borrow_field(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSValue receiver = *qjs_boxed_slot_address(f, operand);
    return qjs_boxed_read_field(f, receiver, get_u32(f->b->byte_code_buf + pc + 1),
                                pc, true);
}

static int qjs_boxed_put_field(QJSBoxedFrame *f, uint32_t atom, uint32_t pc)
{
    JSValue obj = f->sp[-2];
    qjs_boxed_safepoint(f);
    f->ctx->rt->jit_boxed_fields++;
    int ret = QJS_JIT_PROPERTY_WRITE_MISS;
    if (f->ctx->rt->jit_feedback_enabled) {
        ret = qjs_jit_feedback_put_property(f->ctx, f->b, f->b->byte_code_buf + pc,
                                            obj, atom, f->sp[-1], true);
    }
    if (ret == QJS_JIT_PROPERTY_WRITE_MISS)
        ret = JS_SetPropertyInternal2(f->ctx, obj, atom, f->sp[-1], obj, JS_PROP_THROW_STRICT);
    JS_FreeValue(f->ctx, obj);
    f->sp -= 2;
    return ret < 0 ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_get_element(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    JSValue *sp = f->sp;
    JSValue val;
    bool retain_receiver = opcode == OP_get_array_el2;

    f->ctx->rt->jit_boxed_elements++;
    qjs_boxed_safepoint(f);
    if (unlikely(f->ctx->rt->jit_feedback_enabled)) {
        val = qjs_jit_feedback_get_element(f->ctx, f->b, f->b->byte_code_buf + pc, sp[-2], sp[-1], true);
        goto finish;
    }
    if (JS_IsObject(sp[-2]) && JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_INT) {
        JSObject *obj = JS_VALUE_GET_OBJ(sp[-2]);
        uint32_t index = JS_VALUE_GET_INT(sp[-1]);
        if (obj->class_id == JS_CLASS_ARRAY && index < obj->u.array.count) {
            val = js_dup(obj->u.array.u.values[index]);
            goto finish;
        }
        if (js_get_fast_array_element(f->ctx, obj, index, &val))
            goto finish;
    }
    val = JS_GetPropertyValue(f->ctx, sp[-2], sp[-1]);

finish:
    if (retain_receiver) {
        sp[-1] = val;
    } else {
        JS_FreeValue(f->ctx, sp[-2]);
        sp[-2] = val;
        f->sp--;
    }
    return JS_IsException(val) ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_put_element(QJSBoxedFrame *f, uint32_t unused, uint32_t pc)
{
    JSValue *sp = f->sp;
    qjs_boxed_safepoint(f);
    f->ctx->rt->jit_boxed_elements++;
    if (unlikely(f->ctx->rt->jit_feedback_enabled)) {
        QJSJitElementFeedback *feedback = qjs_jit_feedback_element(f->ctx->rt, f->b,
            f->b->byte_code_buf + pc, sp[-3], sp[-2], true, true);
        qjs_jit_feedback_element_value(feedback, sp[-1]);
    }
    int ret = JS_SetPropertyValue(f->ctx, sp[-3], sp[-2], sp[-1], JS_PROP_THROW_STRICT);
    JS_FreeValue(f->ctx, sp[-3]);
    f->sp -= 3;
    return ret < 0 ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_global(QJSBoxedFrame *f, uint32_t atom, uint32_t pc)
{
    int opcode = f->b->byte_code_buf[pc];

    qjs_boxed_safepoint(f);
    if (opcode == OP_put_var || opcode == OP_put_var_init) {
        int ret = JS_SetGlobalVar(f->ctx, atom, f->sp[-1], opcode - OP_put_var);
        f->sp--;
        return ret < 0 ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
    }
    JSValue val = JS_GetGlobalVar(f->ctx, atom, opcode == OP_get_var);
    if (unlikely(JS_IsException(val)))
        return QJS_BOXED_EXCEPTION;
    *f->sp++ = val;
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_declare(QJSBoxedFrame *f, uint32_t atom, uint32_t pc)
{
    JSRuntime *rt = f->ctx->rt;
    const uint8_t *insn = f->b->byte_code_buf + pc;
    if (*insn == OP_check_define_var)
        rt->jit_boxed_global_checks++;
    else if (*insn == OP_define_var)
        rt->jit_boxed_global_vars++;
    else
        rt->jit_boxed_global_funcs++;
    qjs_boxed_safepoint(f);
    return js_op_define_global(f->ctx, &f->sp, atom, insn[5], *insn) ?
           QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_close_loc(QJSBoxedFrame *f, uint32_t slot, uint32_t pc)
{
    f->ctx->rt->jit_boxed_loc_closes++;
    close_lexical_var(f->ctx, f->b, f->sf, slot);
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_call(QJSBoxedFrame *f, uint32_t argc, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue *argv = f->sp - argc;
    int opcode = f->b->byte_code_buf[pc];
    bool method = opcode == OP_call_method || opcode == OP_tail_call_method;
    bool constructor = opcode == OP_call_constructor;
    int receivers = method || constructor ? 2 : 1;
    JSValue callee = argv[constructor ? -2 : -1];
    JSValue val;

    ctx->rt->jit_boxed_calls++;
    qjs_boxed_safepoint(f);
    if (constructor) {
        if (ctx->rt->jit_feedback_enabled)
            qjs_jit_feedback_call(ctx->rt, f->b, f->b->byte_code_buf + pc, callee);
        val = JS_CallConstructorInternal(ctx, callee, argv[-1], argc, vc(argv), 0);
    } else if (!ctx->rt->jit_feedback_enabled ||
               !qjs_jit_profiled_call(ctx, f->b, f->b->byte_code_buf + pc, callee,
                                     argc, argv, &val, true)) {
        val = JS_CallInternal(ctx, callee, method ? argv[-2] : JS_UNDEFINED,
                              JS_UNDEFINED, argc, vc(argv), 0);
    }
    if (unlikely(JS_IsException(val)))
        return QJS_BOXED_EXCEPTION;
    if (opcode == OP_tail_call || opcode == OP_tail_call_method) {
        f->result = val;
        return QJS_BOXED_RETURN;
    }
    for (int i = -receivers; i < (int)argc; i++)
        JS_FreeValue(ctx, argv[i]);
    f->sp = argv - receivers;
    *f->sp++ = val;
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_binary(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue *sp = f->sp;
    int ret = 0;

    if (ctx->rt->jit_feedback_enabled)
        qjs_jit_feedback_binary(ctx->rt, f->b, f->b->byte_code_buf + pc, sp[-2], sp[-1]);
    qjs_boxed_safepoint(f);
    switch (opcode) {
    case OP_add:
        ret = js_add_slow(ctx, sp);
        break;
    case OP_sub: case OP_mul: case OP_div: case OP_mod: case OP_pow:
        ret = js_binary_arith_slow(ctx, sp, opcode);
        break;
    case OP_and: case OP_or: case OP_xor: case OP_shl: case OP_sar:
        ret = js_binary_logic_slow(ctx, sp, opcode);
        break;
    case OP_shr:
        ret = js_shr_slow(ctx, sp);
        break;
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        ret = js_relational_slow(ctx, sp, opcode);
        break;
    case OP_eq: case OP_neq:
        ret = js_eq_slow(ctx, sp, opcode == OP_neq);
        break;
    case OP_strict_eq: case OP_strict_neq: {
        bool equal = js_strict_eq(ctx, sp[-2], sp[-1]);
        JS_FreeValue(ctx, sp[-2]);
        JS_FreeValue(ctx, sp[-1]);
        sp[-2] = js_bool(equal != (opcode == OP_strict_neq));
        break;
    }
    default:
        abort();
    }
    if (ret < 0)
        return QJS_BOXED_EXCEPTION;
    f->sp--;
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_unary(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    qjs_boxed_safepoint(f);
    if (opcode == OP_lnot) {
        f->sp[-1] = js_bool(!JS_ToBoolFree(f->ctx, f->sp[-1]));
        return QJS_BOXED_CONTINUE;
    }
    if (opcode == OP_post_inc || opcode == OP_post_dec) {
        if (JS_VALUE_GET_TAG(f->sp[-1]) == JS_TAG_INT) {
            int64_t value = JS_VALUE_GET_INT(f->sp[-1]);
            f->sp[0] = JS_NewInt64(f->ctx, value + (opcode == OP_post_inc ? 1 : -1));
        } else if (js_post_inc_slow(f->ctx, f->sp, opcode)) {
            return QJS_BOXED_EXCEPTION;
        }
        f->sp++;
        return QJS_BOXED_CONTINUE;
    }
    int ret = opcode == OP_not ? js_not_slow(f->ctx, f->sp) :
              js_unary_arith_slow(f->ctx, f->sp, opcode);
    return ret < 0 ? QJS_BOXED_EXCEPTION : QJS_BOXED_CONTINUE;
}

static int qjs_boxed_loc_arith(QJSBoxedFrame *f, uint32_t slot, uint32_t pc)
{
    JSValue *local = f->sf->var_buf + slot;
    int opcode = f->b->byte_code_buf[pc];
    JSValue value = js_dup(*local);

    qjs_boxed_safepoint(f);
    if (opcode == OP_add_loc) {
        JSValue ops[] = { value, *--f->sp };
        if (f->ctx->rt->jit_feedback_enabled)
            qjs_jit_feedback_binary(f->ctx->rt, f->b, f->b->byte_code_buf + pc, ops[0], ops[1]);
        if (js_add_slow(f->ctx, ops + 2))
            return QJS_BOXED_EXCEPTION;
        value = ops[0];
    } else if (js_unary_arith_slow(f->ctx, &value + 1, opcode == OP_inc_loc ? OP_inc : OP_dec)) {
        return QJS_BOXED_EXCEPTION;
    }
    set_value(f->ctx, local, value);
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_catch(QJSBoxedFrame *f, uint32_t target, uint32_t pc)
{
    *f->sp++ = JS_NewCatchOffset(f->ctx, target);
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_iterator(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSValue *sp = f->sp;

    qjs_boxed_safepoint(f);
    switch (f->b->byte_code_buf[pc]) {
    case OP_for_in_start:
        if (js_for_in_start(ctx, sp))
            return QJS_BOXED_EXCEPTION;
        break;
    case OP_for_in_next:
        ctx->rt->jit_boxed_for_in_next++;
        if (js_for_in_next(ctx, sp))
            return QJS_BOXED_EXCEPTION;
        f->sp += 2;
        break;
    case OP_for_of_start:
    case OP_for_await_of_start:
        if (js_for_of_start(ctx, sp, f->b->byte_code_buf[pc] == OP_for_await_of_start))
            return QJS_BOXED_EXCEPTION;
        sp[1] = JS_NewCatchOffset(ctx, 0);
        f->sp += 2;
        break;
    case OP_for_of_next:
        ctx->rt->jit_boxed_for_of_next++;
        if (js_for_of_next(ctx, sp, -3 - (int)operand))
            return QJS_BOXED_EXCEPTION;
        f->sp += 2;
        break;
    case OP_iterator_get_value_done:
        ctx->rt->jit_boxed_iterator_await_results++;
        if (js_iterator_get_value_done(ctx, sp))
            return QJS_BOXED_EXCEPTION;
        f->sp++;
        break;
    case OP_iterator_next:
        ctx->rt->jit_boxed_delegate_next++;
        if (js_op_iterator_next(ctx, sp))
            return QJS_BOXED_EXCEPTION;
        break;
    case OP_iterator_call:
        ctx->rt->jit_boxed_delegate_calls++;
        if (js_op_iterator_call(ctx, sp, operand))
            return QJS_BOXED_EXCEPTION;
        f->sp++;
        break;
    case OP_iterator_close:
        if (!JS_IsUndefined(sp[-3]))
            ctx->rt->jit_boxed_iterator_closes++;
        if (js_iterator_close_stack(ctx, &f->sp))
            return QJS_BOXED_EXCEPTION;
        break;
    default:
        abort();
    }
    return QJS_BOXED_CONTINUE;
}

static int qjs_boxed_exit(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    if (opcode == OP_throw) {
        qjs_boxed_safepoint(f);
        JS_Throw(f->ctx, *--f->sp);
        return QJS_BOXED_EXCEPTION;
    }
    f->result = opcode == OP_return ? *--f->sp : JS_UNDEFINED;
    return QJS_BOXED_RETURN;
}

static int qjs_boxed_suspend(QJSBoxedFrame *f, uint32_t opcode, uint32_t pc)
{
    f->result = js_coro_terminal_value(opcode);
    return QJS_BOXED_SUSPEND;
}

static int qjs_boxed_branch(QJSBoxedFrame *f, uint32_t target, uint32_t pc)
{
    int opcode = f->b->byte_code_buf[pc];
    bool taken = true;

    if (opcode == OP_if_true || opcode == OP_if_false || opcode == OP_if_true8 || opcode == OP_if_false8) {
        bool truthy = JS_ToBoolFree(f->ctx, *--f->sp);
        taken = truthy == (opcode == OP_if_true || opcode == OP_if_true8);
        if (f->ctx->rt->jit_feedback_enabled)
            qjs_jit_feedback_branch(f->ctx->rt, f->b, f->b->byte_code_buf + pc, taken);
    }
    if (taken)
        f->pc = target;
    if (unlikely(f->ctx->rt->interrupt_handler))
        qjs_boxed_safepoint(f);
    if (unlikely(js_poll_interrupts(f->ctx)))
        return QJS_BOXED_EXCEPTION;
    if (taken && target <= pc && !f->fuel--)
        return QJS_BOXED_FUEL;
    return taken ? QJS_BOXED_TAKEN : QJS_BOXED_CONTINUE;
}

#include "qjs-jit-literal.h"

#define QJS_BOXED_HELPER_LIST(M) \
    M(PUSH_INT, push_int) M(PUSH_CONST, push_const) M(PUSH_PRIMITIVE, push_primitive) \
    M(REGEXP_IMPORT, regexp_import) M(APPLY_EVAL, apply_eval) \
    M(CREATE, create) M(SLOT, slot) M(STACK, stack) M(GET_FIELD, get_field) \
    M(BORROW_FIELD, borrow_field) M(BORROW_TYPE, borrow_type) M(PUT_FIELD, put_field) \
    M(GET_ELEMENT, get_element) \
    M(PUT_ELEMENT, put_element) \
    M(GLOBAL, global) M(CALL, call) M(BINARY, binary) M(UNARY, unary) \
    M(LOC_ARITH, loc_arith) M(CATCH, catch) M(ITERATOR, iterator) M(EXIT, exit) \
    M(BRANCH, branch) M(LITERAL, literal) M(TYPE, type) M(MEMBERSHIP, membership) \
    M(CORO_TERMINAL, suspend) M(CONVERT, convert) M(CTOR, ctor) M(SPECIAL, special) \
    M(REFERENCE, reference) M(CLASS, class) M(DECLARE, declare) M(CLOSE_LOC, close_loc) \
    M(PUSH_SHORT_BIGINT, push_short_bigint)

typedef enum {
    QJS_BOXED_UNHANDLED,
    QJS_BOXED_NOP,
#define QJS_BOXED_HELPER_ENUM(id, fn) QJS_BOXED_##id,
    QJS_BOXED_HELPER_LIST(QJS_BOXED_HELPER_ENUM)
#undef QJS_BOXED_HELPER_ENUM
} QJSBoxedHelperID;

static QJSBoxedHelper *const qjs_boxed_helpers[] = {
#define QJS_BOXED_HELPER_ENTRY(id, fn) [QJS_BOXED_##id] = qjs_boxed_##fn,
    QJS_BOXED_HELPER_LIST(QJS_BOXED_HELPER_ENTRY)
#undef QJS_BOXED_HELPER_ENTRY
};
#undef QJS_BOXED_HELPER_LIST

typedef struct {
    uint32_t operand;
    uint16_t next;
    int16_t depth;
    uint16_t pop;
    uint16_t push;
    uint8_t helper;
    bool falls_through;
    bool branch_target;
    bool escape_elided;
} QJSBoxedInsn;

typedef struct {
    QJSArm64Emitter emitter;
    QJSBoxedInsn insns[QJS_BOXED_MAX_BYTECODE];
    uint16_t queue[QJS_BOXED_MAX_BYTECODE];
    uint32_t code[QJS_BOXED_MAX_BYTECODE * 12];
} QJSBoxedWork;

static bool qjs_boxed_decode_slot(const JSFunctionBytecode *b, QJSBoxedInsn *insn,
                                  unsigned opcode, const uint8_t *imm, unsigned fmt)
{
    uint32_t index, flags = 0;
    unsigned limit = b->var_count;

    switch (fmt) {
    case OP_FMT_none_loc:
        if (opcode == OP_get_loc0_loc1)
            return false;
        if (opcode >= OP_set_loc0) {
            index = opcode - OP_set_loc0;
            flags = QJS_BOXED_SLOT_SET;
        } else if (opcode >= OP_put_loc0) {
            index = opcode - OP_put_loc0;
            flags = QJS_BOXED_SLOT_PUT;
        } else {
            index = opcode - OP_get_loc0;
        }
        break;
    case OP_FMT_none_arg:
        limit = b->arg_count;
        flags = QJS_BOXED_SLOT_ARG;
        if (opcode >= OP_set_arg0) {
            index = opcode - OP_set_arg0;
            flags |= QJS_BOXED_SLOT_SET;
        } else if (opcode >= OP_put_arg0) {
            index = opcode - OP_put_arg0;
            flags |= QJS_BOXED_SLOT_PUT;
        } else {
            index = opcode - OP_get_arg0;
        }
        break;
    case OP_FMT_none_var_ref:
        limit = b->closure_var_count;
        flags = QJS_BOXED_SLOT_REF;
        if (opcode >= OP_set_var_ref0) {
            index = opcode - OP_set_var_ref0;
            flags |= QJS_BOXED_SLOT_SET;
        } else if (opcode >= OP_put_var_ref0) {
            index = opcode - OP_put_var_ref0;
            flags |= QJS_BOXED_SLOT_PUT;
        } else {
            index = opcode - OP_get_var_ref0;
        }
        break;
    default:
        index = fmt == OP_FMT_loc8 ? *imm : get_u16(imm);
        switch (opcode) {
        case OP_get_arg: case OP_put_arg: case OP_set_arg:
            limit = b->arg_count;
            flags = QJS_BOXED_SLOT_ARG;
            break;
        case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
        case OP_get_var_ref_check: case OP_put_var_ref_check: case OP_put_var_ref_check_init:
            limit = b->closure_var_count;
            flags = QJS_BOXED_SLOT_REF;
            break;
        default:
            break;
        }
        switch (opcode) {
        case OP_put_loc: case OP_put_loc8: case OP_put_arg: case OP_put_var_ref:
            flags |= QJS_BOXED_SLOT_PUT;
            break;
        case OP_set_loc: case OP_set_loc8: case OP_set_arg: case OP_set_var_ref:
            flags |= QJS_BOXED_SLOT_SET;
            break;
        case OP_get_loc_check: case OP_get_var_ref_check:
            flags |= QJS_BOXED_SLOT_TDZ;
            break;
        case OP_put_loc_check: case OP_put_var_ref_check:
            flags |= QJS_BOXED_SLOT_TDZ | QJS_BOXED_SLOT_PUT;
            break;
        case OP_put_loc_check_init:
        case OP_put_var_ref_check_init:
            flags |= QJS_BOXED_SLOT_INIT | QJS_BOXED_SLOT_PUT;
            break;
        case OP_set_loc_uninitialized:
            flags |= QJS_BOXED_SLOT_RESET;
            break;
        case OP_get_loc: case OP_get_loc8: case OP_get_arg: case OP_get_var_ref:
            break;
        default:
            return false;
        }
        break;
    }
    if (index >= limit)
        return false;
    insn->helper = QJS_BOXED_SLOT;
    insn->operand = flags | index;
    return true;
}

static bool qjs_boxed_decode(const JSFunctionBytecode *b, QJSBoxedWork *work)
{
    for (unsigned pc = 0; pc < (unsigned)b->byte_code_len;) {
        unsigned opcode = b->byte_code_buf[pc];
        unsigned canonical = opcode == OP_object_layout ? OP_object :
                             opcode == OP_init_object_field ? OP_define_field : opcode;
        if (canonical == OP_invalid || canonical >= OP_COUNT)
            return false;
        const JSOpCode *info = &short_opcode_info(canonical);
        unsigned next = pc + info->size;
        if (!info->size || next > (unsigned)b->byte_code_len)
            return false;
        const uint8_t *imm = b->byte_code_buf + pc + 1;
        QJSBoxedInsn *insn = &work->insns[pc];
        *insn = (QJSBoxedInsn) { .next = next, .depth = -1, .pop = info->n_pop, .push = info->n_push,
                                 .operand = opcode, .falls_through = true };

        switch (opcode) {
        case OP_nop:
            insn->helper = QJS_BOXED_NOP;
            break;
        case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
        case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
            insn->helper = QJS_BOXED_PUSH_INT;
            insn->operand = (int)opcode - OP_push_0;
            break;
        case OP_push_i32: case OP_push_i16: case OP_push_i8:
            insn->helper = QJS_BOXED_PUSH_INT;
            insn->operand = opcode == OP_push_i32 ? get_u32(imm) :
                            opcode == OP_push_i16 ? (int32_t)(int16_t)get_u16(imm) : (int32_t)(int8_t)*imm;
            break;
        case OP_push_bigint_i32:
            insn->helper = QJS_BOXED_PUSH_SHORT_BIGINT;
            insn->operand = get_u32(imm);
            break;
        case OP_push_const: case OP_push_const8: case OP_fclosure: case OP_fclosure8:
            insn->operand = info->size == 2 ? *imm : get_u32(imm);
            if (insn->operand >= (unsigned)b->cpool_count)
                return false;
            insn->helper = opcode == OP_fclosure || opcode == OP_fclosure8 ?
                           QJS_BOXED_CREATE : QJS_BOXED_PUSH_CONST;
            break;
        case OP_null: case OP_undefined: case OP_push_true: case OP_push_false:
            insn->helper = QJS_BOXED_PUSH_PRIMITIVE;
            break;
        case OP_regexp: case OP_import:
            insn->helper = QJS_BOXED_REGEXP_IMPORT;
            break;
        case OP_eval: {
            unsigned argc = get_u16(imm);
            if (argc > UINT16_MAX - insn->pop)
                return false;
            insn->pop += argc;
            insn->helper = QJS_BOXED_APPLY_EVAL;
            break;
        }
        case OP_apply:
            if (get_u16(imm) > 1)
                return false;
            /* fall through */
        case OP_apply_eval:
            insn->helper = QJS_BOXED_APPLY_EVAL;
            break;
        case OP_push_this: case OP_push_empty_string: case OP_push_atom_value:
            insn->helper = QJS_BOXED_CREATE;
            if (opcode == OP_push_atom_value)
                insn->operand = get_u32(imm);
            break;
        case OP_object: case OP_object_layout: case OP_set_proto: case OP_set_home_object:
        case OP_define_array_el: case OP_set_name_computed: case OP_append:
            insn->helper = QJS_BOXED_LITERAL;
            break;
        case OP_define_field: case OP_init_object_field: case OP_set_name:
            insn->helper = QJS_BOXED_LITERAL;
            insn->operand = get_u32(imm);
            break;
        case OP_array_from:
            insn->helper = QJS_BOXED_LITERAL;
            insn->operand = get_u16(imm);
            insn->pop = insn->operand;
            break;
        case OP_copy_data_properties:
            insn->helper = QJS_BOXED_LITERAL;
            insn->operand = *imm;
            insn->pop = insn->push = 1 + max_int(*imm & 3, max_int((*imm >> 2) & 7, *imm >> 5));
            break;
        case OP_drop: case OP_dup: case OP_dup1: case OP_dup2: case OP_dup3:
        case OP_nip: case OP_nip1:
        case OP_insert2: case OP_insert3: case OP_insert4:
        case OP_perm3: case OP_perm4: case OP_perm5: case OP_swap: case OP_swap2:
        case OP_rot3l: case OP_rot3r: case OP_rot4l: case OP_rot5l:
        case OP_get_loc0_loc1:
            if (opcode == OP_get_loc0_loc1 && b->var_count < 2)
                return false;
            insn->helper = QJS_BOXED_STACK;
            break;
        case OP_is_undefined_or_null: case OP_is_undefined: case OP_is_null:
        case OP_typeof_is_undefined: case OP_typeof_is_function: case OP_typeof: case OP_check_object:
            insn->helper = QJS_BOXED_TYPE;
            break;
        case OP_to_object: case OP_to_propkey: case OP_to_propkey2:
            insn->helper = QJS_BOXED_CONVERT;
            break;
        case OP_check_ctor: case OP_check_ctor_return: case OP_init_ctor:
            insn->helper = QJS_BOXED_CTOR;
            break;
        case OP_special_object:
            if (*imm > OP_SPECIAL_OBJECT_NULL_PROTO)
                return false;
            insn->helper = QJS_BOXED_SPECIAL;
            insn->operand = *imm;
            break;
        case OP_rest:
            insn->helper = QJS_BOXED_SPECIAL;
            insn->operand = get_u16(imm);
            break;
        case OP_make_loc_ref: case OP_make_arg_ref: case OP_make_var_ref_ref:
            if (get_u16(imm + 4) >= (opcode == OP_make_loc_ref ? b->var_count :
                                      opcode == OP_make_arg_ref ? b->arg_count : b->closure_var_count))
                return false;
            /* fall through */
        case OP_make_var_ref:
            insn->operand = get_u32(imm);
            /* fall through */
        case OP_get_ref_value: case OP_put_ref_value:
            insn->helper = QJS_BOXED_REFERENCE;
            break;
        case OP_private_symbol:
            insn->operand = get_u32(imm);
            /* fall through */
        case OP_get_super: case OP_get_super_value: case OP_put_super_value:
        case OP_get_private_field: case OP_put_private_field: case OP_define_private_field:
        case OP_check_brand: case OP_add_brand:
            insn->helper = QJS_BOXED_CLASS;
            break;
        case OP_delete_var:
            insn->operand = get_u32(imm);
            /* fall through */
        case OP_in: case OP_private_in: case OP_instanceof: case OP_delete:
            insn->helper = QJS_BOXED_MEMBERSHIP;
            break;
        case OP_get_field: case OP_get_field2: case OP_get_length:
            insn->helper = QJS_BOXED_GET_FIELD;
            insn->operand = opcode == OP_get_length ? JS_ATOM_length : get_u32(imm);
            break;
        case OP_put_field:
            insn->helper = QJS_BOXED_PUT_FIELD;
            insn->operand = get_u32(imm);
            break;
        case OP_get_array_el: case OP_get_array_el2:
            insn->helper = QJS_BOXED_GET_ELEMENT;
            break;
        case OP_put_array_el:
            insn->helper = QJS_BOXED_PUT_ELEMENT;
            break;
        case OP_get_var: case OP_get_var_undef: case OP_put_var: case OP_put_var_init:
            insn->helper = QJS_BOXED_GLOBAL;
            insn->operand = get_u32(imm);
            break;
        case OP_check_define_var: case OP_define_var: case OP_define_func:
            insn->helper = QJS_BOXED_DECLARE;
            insn->operand = get_u32(imm);
            break;
        case OP_close_loc: {
            unsigned slot = get_u16(imm);
            if (!b->vardefs || slot >= b->var_count ||
                b->vardefs[b->arg_count + slot].var_ref_idx >= b->var_ref_count)
                return false;
            insn->helper = QJS_BOXED_CLOSE_LOC;
            insn->operand = slot;
            break;
        }
        case OP_call0: case OP_call1: case OP_call2: case OP_call3:
        case OP_call: case OP_call_method: case OP_call_constructor: case OP_tail_call: case OP_tail_call_method:
            if (b->func_kind != JS_FUNC_NORMAL &&
                (opcode == OP_tail_call || opcode == OP_tail_call_method))
                break;
            insn->helper = QJS_BOXED_CALL;
            insn->operand = info->size == 1 ? opcode - OP_call0 : get_u16(imm);
            if (insn->operand > UINT16_MAX - insn->pop)
                return false;
            insn->pop += insn->operand;
            insn->falls_through = opcode != OP_tail_call && opcode != OP_tail_call_method;
            break;
        case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod: case OP_pow:
        case OP_and: case OP_or: case OP_xor: case OP_shl: case OP_sar: case OP_shr:
        case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
            insn->helper = QJS_BOXED_BINARY;
            break;
        case OP_plus: case OP_neg: case OP_not: case OP_lnot: case OP_inc: case OP_dec:
        case OP_post_inc: case OP_post_dec:
            insn->helper = QJS_BOXED_UNARY;
            break;
        case OP_add_loc: case OP_inc_loc: case OP_dec_loc:
            if (*imm >= b->var_count)
                return false;
            insn->helper = QJS_BOXED_LOC_ARITH;
            insn->operand = *imm;
            break;
        case OP_return: case OP_return_undef: case OP_throw:
            if (b->func_kind != JS_FUNC_NORMAL && opcode != OP_throw)
                break;
            insn->helper = QJS_BOXED_EXIT;
            insn->falls_through = false;
            break;
        case OP_await: case OP_yield: case OP_yield_star: case OP_async_yield_star: case OP_return_async:
            if (b->func_kind == JS_FUNC_NORMAL)
                break;
            insn->helper = QJS_BOXED_CORO_TERMINAL;
            insn->falls_through = false;
            /* The coroutine caller transforms the stack after suspension. */
            insn->push = insn->pop;
            break;
        case OP_for_in_start: case OP_for_in_next: case OP_for_of_start: case OP_iterator_close:
        case OP_for_await_of_start: case OP_iterator_next:
            insn->helper = QJS_BOXED_ITERATOR;
            break;
        case OP_iterator_call:
            if (*imm > 3)
                return false;
            insn->helper = QJS_BOXED_ITERATOR;
            insn->operand = *imm;
            break;
        case OP_iterator_get_value_done:
            insn->helper = QJS_BOXED_ITERATOR;
            /* The iterator record is below the awaited result. */
            insn->pop += 3;
            insn->push += 3;
            break;
        case OP_for_of_next:
            insn->helper = QJS_BOXED_ITERATOR;
            insn->operand = *imm;
            /* The u8 counts values between the iterator record and stack top. */
            insn->pop += *imm;
            insn->push += *imm;
            break;
        case OP_if_false: case OP_if_true: case OP_if_false8: case OP_if_true8:
        case OP_goto: case OP_goto8: case OP_goto16: case OP_catch: {
            int32_t delta = info->size == 2 ? (int8_t)*imm :
                            info->size == 3 ? (int16_t)get_u16(imm) : (int32_t)get_u32(imm);
            int64_t target = (int64_t)pc + 1 + delta;
            if (target < 0 || target >= b->byte_code_len)
                return false;
            insn->helper = opcode == OP_catch ? QJS_BOXED_CATCH : QJS_BOXED_BRANCH;
            insn->operand = target;
            insn->falls_through = opcode != OP_goto && opcode != OP_goto8 && opcode != OP_goto16;
            break;
        }
        default:
            switch (info->fmt) {
            case OP_FMT_loc: case OP_FMT_loc8: case OP_FMT_none_loc:
            case OP_FMT_arg: case OP_FMT_none_arg:
            case OP_FMT_var_ref: case OP_FMT_none_var_ref:
                qjs_boxed_decode_slot(b, insn, opcode, imm, info->fmt);
                break;
            }
            break;
        }
        pc = next;
    }
    return true;
}

#include "qjs-jit-escape.h"

static bool qjs_boxed_verify(const JSFunctionBytecode *b, QJSBoxedWork *work,
                             unsigned entry_pc, unsigned entry_depth)
{
    QJSBoxedInsn *insns = work->insns;
    unsigned head = 0, tail = 0, supported = 0;
    bool useful = false, escape_elided = false, literal = false;
    if (entry_pc >= (unsigned)b->byte_code_len || !insns[entry_pc].next ||
        entry_depth > b->stack_size || entry_depth > INT16_MAX)
        return false;
    insns[entry_pc].depth = entry_depth;
    /* A resume PC cannot be removed by a fusion starting at its predecessor. */
    insns[entry_pc].branch_target = true;
    work->queue[tail++] = entry_pc;

    while (head < tail) {
        unsigned pc = work->queue[head++];
        QJSBoxedInsn *insn = &insns[pc];
        if (!insn->helper)
            continue;
        if (insn->pop > insn->depth)
            return false;
        int depth = insn->depth - insn->pop + insn->push;
        if (depth > b->stack_size || depth > INT16_MAX)
            return false;
        if (insn->helper == QJS_BOXED_BRANCH || insn->helper == QJS_BOXED_CATCH) {
            if (!insns[insn->operand].next)
                return false;
            insns[insn->operand].branch_target = true;
        }

        supported++;
        useful |= insn->helper == QJS_BOXED_REGEXP_IMPORT || insn->helper == QJS_BOXED_APPLY_EVAL ||
                  (insn->helper >= QJS_BOXED_GET_FIELD && insn->helper <= QJS_BOXED_EXIT) ||
                  insn->helper == QJS_BOXED_TYPE || insn->helper == QJS_BOXED_MEMBERSHIP ||
                  (insn->helper >= QJS_BOXED_CONVERT && insn->helper <= QJS_BOXED_PUSH_SHORT_BIGINT);
        escape_elided |= insn->escape_elided;
        literal |= insn->helper == QJS_BOXED_LITERAL;
        unsigned targets[2], count = 0;
        if (insn->falls_through)
            targets[count++] = insn->next;
        if (insn->helper == QJS_BOXED_BRANCH)
            targets[count++] = insn->operand;
        for (unsigned i = 0; i < count; i++) {
            if (targets[i] >= (unsigned)b->byte_code_len)
                return false;
            QJSBoxedInsn *target = &insns[targets[i]];
            if (target->depth < 0) {
                target->depth = depth;
                work->queue[tail++] = targets[i];
            } else if (target->depth != depth) {
                return false;
            }
        }
    }
    return (useful && supported >= 3) || escape_elided || (literal && supported >= 2);
}

#include "qjs-jit-inline.h"

static bool qjs_boxed_emit_shuffle(QJSArm64Emitter *e, unsigned opcode, bool stats)
{
    const QJSBoxedShuffle *shuffle = qjs_boxed_shuffle(opcode);
    if (!shuffle || sizeof(JSValue) != 16)
        return false;

    qjs_arm64_emit(e, 0xf9400268 | ((offsetof(QJSBoxedFrame, sp) / 8) << 10)); /* ldr x8 */
    for (unsigned i = 0; i < shuffle->count; i++) {
        if (shuffle->source[i] != i) {
            uint32_t offset = ((i - shuffle->count) * sizeof(JSValue)) & 0x1ff;
            qjs_arm64_emit(e, 0x3cc00100 | (offset << 12) | i); /* ldur qi, [x8, #offset] */
        }
    }
    for (unsigned i = 0; i < shuffle->count; i++) {
        if (shuffle->source[i] != i) {
            uint32_t offset = ((i - shuffle->count) * sizeof(JSValue)) & 0x1ff;
            qjs_arm64_emit(e, 0x3c800100 | (offset << 12) | shuffle->source[i]); /* stur qsrc */
        }
    }
    if (stats) {
        uint32_t offset = (offsetof(QJSBoxedFrame, native_shuffles) / 4) << 10;
        qjs_arm64_emit(e, 0xb9400269 | offset); /* ldr w9, [x19, #native_shuffles] */
        qjs_arm64_emit(e, 0x11000529); /* add w9, w9, #1 */
        qjs_arm64_emit(e, 0xb9000269 | offset); /* str w9, [x19, #native_shuffles] */
    }
    return true;
}

#include "qjs-jit-rc.h"
#include "qjs-jit-field.h"

#ifdef QJS_BOXED_NATIVE_RC
static void qjs_boxed_emit_short_bigint(QJSArm64Emitter *e, uint32_t payload, bool stats)
{
    unsigned sp_imm = (offsetof(QJSBoxedFrame, sp) / 8) << 10;
    qjs_arm64_immediate(e, 10, payload);
    qjs_arm64_immediate(e, 11, JS_TAG_SHORT_BIG_INT);
    qjs_arm64_emit(e, 0xf940026c | sp_imm); /* ldr x12, [x19, #sp] */
    qjs_arm64_emit(e, 0xa8812d8a); /* stp x10, x11, [x12], #16 */
    qjs_arm64_emit(e, 0xf900026c | sp_imm); /* str x12, [x19, #sp] */
    if (stats)
        qjs_boxed_rc_note(e, offsetof(QJSBoxedFrame, native_short_bigints));
}
#endif

static size_t qjs_boxed_compile(const JSFunctionBytecode *b, QJSBoxedWork *work,
                               unsigned entry_pc, unsigned entry_depth, bool stats)
{
    if (!qjs_boxed_decode(b, work))
        return 0;
    if (b->func_kind == JS_FUNC_NORMAL)
        qjs_boxed_escape_analyze(b, work);
    if (!qjs_boxed_verify(b, work, entry_pc, entry_depth))
        return 0;

    QJSArm64Emitter *e = &work->emitter;
    e->code = work->code;
    e->capacity = countof(work->code);
    qjs_arm64_emit(e, 0xa9be7bfd); /* stp x29, x30, [sp, #-32]! */
    qjs_arm64_emit(e, 0xa90153f3); /* stp x19, x20, [sp, #16] */
    qjs_arm64_emit(e, 0x910003fd); /* mov x29, sp */
    qjs_arm64_emit(e, 0xaa0003f3); /* mov x19, x0 */
    qjs_arm64_emit(e, 0xaa0103f4); /* mov x20, x1 */
    if (entry_pc)
        qjs_arm64_jump(e, 0x14000000, entry_pc);

    unsigned inline_sites = 0;
    unsigned field_sites = 0;
    for (unsigned pc = 0; pc < (unsigned)b->byte_code_len; pc = work->insns[pc].next) {
        const QJSBoxedInsn *insn = &work->insns[pc];
        if (insn->depth < 0)
            continue;
        e->offsets[pc] = e->count;
        if (insn->helper == QJS_BOXED_NOP)
            continue;
        if (insn->escape_elided)
            qjs_boxed_emit_escape_count(e, b->byte_code_buf[pc] == OP_object_layout);

        QJSBoxedHelperID helper = insn->helper;
#ifdef QJS_BOXED_NATIVE_RC
        if (helper == QJS_BOXED_PUSH_SHORT_BIGINT) {
            qjs_boxed_emit_short_bigint(e, insn->operand, stats);
            continue;
        }
#endif
        if (helper == QJS_BOXED_STACK && qjs_boxed_emit_shuffle(e, insn->operand, stats))
            continue;
        unsigned next = helper == QJS_BOXED_UNHANDLED ? pc : insn->next;
        unsigned op_pc = pc;
        if (helper == QJS_BOXED_SLOT &&
            !(insn->operand & ~(UINT16_MAX | QJS_BOXED_SLOT_ARG | QJS_BOXED_SLOT_REF)) &&
            next < (unsigned)b->byte_code_len && !work->insns[next].branch_target) {
            const QJSBoxedInsn *consumer = &work->insns[next];
            unsigned opcode = b->byte_code_buf[next];
            if (consumer->helper == QJS_BOXED_GET_FIELD && opcode == OP_get_field)
                helper = QJS_BOXED_BORROW_FIELD;
            else if (consumer->helper == QJS_BOXED_TYPE && opcode != OP_check_object)
                helper = QJS_BOXED_BORROW_TYPE;
            if (helper != QJS_BOXED_SLOT) {
                op_pc = next;
                next = consumer->next;
            }
        }
#ifdef QJS_BOXED_NATIVE_RC
        if (helper == QJS_BOXED_SLOT && qjs_boxed_emit_rc_slot(e, insn->operand, stats))
            continue;
        if (helper == QJS_BOXED_STACK && insn->operand == OP_drop)
            qjs_boxed_emit_rc_drop(e, next, stats);
#endif
        qjs_arm64_immediate(e, 3, next);
        qjs_arm64_emit(e, 0xb9000263 | ((offsetof(QJSBoxedFrame, pc) / sizeof(uint32_t)) << 10));
        if (insn->helper == QJS_BOXED_UNHANDLED) {
            qjs_arm64_immediate(e, 0, QJS_BOXED_RESUME);
            qjs_arm64_jump(e, 0x14000000, QJS_BOXED_EPILOGUE);
            continue;
        }

        if (helper == QJS_BOXED_CALL && inline_sites < QJS_JIT_INLINE_MAX_SITES &&
            qjs_boxed_emit_inline_tail(e, b, pc, next, insn->operand))
            inline_sites++;
        if (field_sites < QJS_JIT_NATIVE_FIELD_MAX_SITES &&
            (helper == QJS_BOXED_BORROW_FIELD ||
             (helper == QJS_BOXED_GET_FIELD && b->byte_code_buf[pc] == OP_get_field2)) &&
            qjs_boxed_emit_field(e, b, op_pc, next, insn->operand,
                                 helper == QJS_BOXED_BORROW_FIELD, stats))
            field_sites++;

        qjs_arm64_emit(e, 0xaa1303e0); /* mov x0, x19 */
        qjs_arm64_immediate(e, 1, insn->operand);
        qjs_arm64_immediate(e, 2, op_pc);
        qjs_arm64_emit(e, 0xf9400290 | ((uint32_t)helper << 10)); /* ldr x16, [x20, #helper] */
        qjs_arm64_emit(e, 0xd63f0200); /* blr x16 */
        if (insn->helper == QJS_BOXED_BRANCH) {
            qjs_arm64_emit(e, 0x7100001f | (QJS_BOXED_TAKEN << 10)); /* cmp w0, #QJS_BOXED_TAKEN */
            qjs_arm64_bcond(e, QJS_ARM64_EQ, insn->operand);
        }
        if (insn->falls_through)
            qjs_arm64_jump(e, 0x35000000, QJS_BOXED_EPILOGUE); /* cbnz w0 */
        else
            qjs_arm64_jump(e, 0x14000000, QJS_BOXED_EPILOGUE);
        pc = op_pc;
    }
    e->offsets[QJS_BOXED_EPILOGUE] = e->count;
    qjs_arm64_emit(e, 0xa94153f3); /* ldp x19, x20, [sp, #16] */
    qjs_arm64_emit(e, 0xa8c27bfd); /* ldp x29, x30, [sp], #32 */
    qjs_arm64_emit(e, 0xd65f03c0); /* ret */
    return qjs_arm64_finish(e);
}

typedef struct {
    QJSBoxedSite jit;
    uint16_t pc, depth;
} QJSBoxedCoroSite;

typedef struct QJSBoxedCoroSites {
    unsigned count;
    QJSBoxedCoroSite sites[8];
} QJSBoxedCoroSites;

typedef struct QJSBoxedCode {
    struct QJSBoxedCode *prev, *next;
    QJSBoxedSite *site;
    void *entry;
    size_t size;
    uint64_t used;
    unsigned active;
    bool coroutine;
} QJSBoxedCode;

static void qjs_boxed_cache_unlink(JSRuntime *rt, QJSBoxedCode *code)
{
    if (code->prev)
        code->prev->next = code->next;
    else
        rt->jit_boxed_cache_oldest = code->next;
    if (code->next)
        code->next->prev = code->prev;
    else
        rt->jit_boxed_cache_newest = code->prev;
}

static void qjs_boxed_cache_link(JSRuntime *rt, QJSBoxedCode *code)
{
    code->used = rt->jit_cache_tick;
    code->prev = rt->jit_boxed_cache_newest;
    code->next = NULL;
    if (code->prev)
        code->prev->next = code;
    else
        rt->jit_boxed_cache_oldest = code;
    rt->jit_boxed_cache_newest = code;
}

static void qjs_boxed_cache_remove(JSRuntime *rt, QJSBoxedSite *site)
{
    QJSBoxedCode *code = site->code;
    if (!code)
        return;
    assert(code->site == site && !code->active);
    qjs_boxed_cache_unlink(rt, code);
    munmap(code->entry, code->size);
    rt->jit_boxed_cache_bytes -= code->size;
    rt->jit_code_bytes -= code->size;
    rt->malloc_state.malloc_size -= code->size;
    rt->malloc_state.malloc_count--;
    site->code = NULL;
    js_free_rt(rt, code);
}

static bool qjs_boxed_cache_has_space(const JSRuntime *rt, size_t size, bool boxed_request)
{
    assert(rt->jit_boxed_cache_bytes <= QJS_JIT_CACHE_BOXED_LIMIT);
    return boxed_request ? size <= QJS_JIT_CACHE_BOXED_LIMIT - rt->jit_boxed_cache_bytes
                         : qjs_jit_cache_has_space(rt, size);
}

static bool qjs_boxed_cache_reclaim(JSRuntime *rt, size_t size, bool boxed_request)
{
    if (qjs_boxed_cache_has_space(rt, size, boxed_request))
        return true;
    QJSBoxedCode *code = rt->jit_boxed_cache_oldest;
    for (unsigned i = 0; i < QJS_JIT_CACHE_SCAN_LIMIT && code; i++) {
        if (rt->jit_cache_tick - code->used < QJS_JIT_CACHE_COLD_AGE)
            break;
        QJSBoxedCode *next = code->next;
        if (code->active) {
            rt->jit_boxed_cache_pinned++;
        } else {
            code->site->was_evicted = true;
            rt->jit_boxed_cache_evictions[code->coroutine]++;
            qjs_boxed_cache_remove(rt, code->site);
            if (qjs_boxed_cache_has_space(rt, size, boxed_request))
                return true;
        }
        code = next;
    }
    return false;
}

static void qjs_jit_boxed_free(JSRuntime *rt, JSFunctionBytecode *b)
{
    qjs_boxed_cache_remove(rt, &b->jit_boxed_site);
    QJSBoxedCoroSites *coro = b->jit_boxed_coro_sites;
    if (coro) {
        for (unsigned i = 0; i < coro->count; i++)
            qjs_boxed_cache_remove(rt, &coro->sites[i].jit);
        js_free_rt(rt, coro);
        b->jit_boxed_coro_sites = NULL;
    }
}

static bool qjs_boxed_install(JSRuntime *rt, JSFunctionBytecode *b, unsigned pc, unsigned depth,
                              QJSBoxedSite *site)
{
    assert(!site->code);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || b->byte_code_len <= 0 || b->byte_code_len > QJS_BOXED_MAX_BYTECODE) {
        site->rejected = true;
        return false;
    }
    QJSBoxedWork *work = js_mallocz_rt(rt, sizeof(*work));
    if (!work)
        return false;
    size_t words = qjs_boxed_compile(b, work, pc, depth, rt->jit_stats);
    if (!words) {
        site->rejected = true;
        js_free_rt(rt, work);
        return false;
    }

    size_t bytes = words * sizeof(*work->code);
    size_t size = (bytes + page_size - 1) / page_size * page_size;
    QJSBoxedCode *code = js_mallocz_rt(rt, sizeof(*code));
    if (!code || size > QJS_JIT_CACHE_BOXED_LIMIT ||
        !qjs_boxed_cache_reclaim(rt, size, true) || !qjs_jit_cache_reserve(rt, b, size)) {
        js_free_rt(rt, code);
        js_free_rt(rt, work);
        return false;
    }

    void *entry = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (entry == MAP_FAILED) {
        js_free_rt(rt, code);
        js_free_rt(rt, work);
        return false;
    }
    memcpy(entry, work->code, bytes);
    js_free_rt(rt, work);
    __builtin___clear_cache(entry, (char *)entry + bytes);
    if (mprotect(entry, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(entry, size);
        js_free_rt(rt, code);
        return false;
    }

    code->site = site;
    code->entry = entry;
    code->size = size;
    code->coroutine = b->func_kind != JS_FUNC_NORMAL;
    site->code = code;
    qjs_boxed_cache_link(rt, code);
    if (site->was_evicted) {
        site->was_evicted = false;
        rt->jit_boxed_cache_reinstalls[code->coroutine]++;
    }
    rt->jit_boxed_cache_bytes += size;
    rt->jit_code_bytes += size;
    rt->malloc_state.malloc_size += size;
    rt->malloc_state.malloc_count++;
    rt->jit_boxed_compiles++;
    return true;
}

static bool qjs_boxed_warm(JSRuntime *rt, JSFunctionBytecode *b, unsigned pc, unsigned depth,
                           QJSBoxedSite *site)
{
    if (++site->hot_calls < QJS_JIT_BASELINE_THRESHOLD)
        return false;
    site->hot_calls = 0;
    uint64_t started;
    if (!qjs_jit_cache_begin_compile(rt, b, &started))
        return false;
    bool installed = qjs_boxed_install(rt, b, pc, depth, site);
    qjs_jit_cache_end_compile(rt, started);
    return installed;
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int qjs_boxed_run(QJSBoxedCode *code, JSContext *ctx, JSContext *caller_ctx,
                         JSFunctionBytecode *b, JSStackFrame *sf, JSVarRef **var_refs,
                         JSValue this_obj, JSValue new_target, int argc, JSValueConst *argv,
                         uint8_t **pc, JSValue **sp, JSValue *result)
{
    JSRuntime *rt = ctx->rt;
    QJSBoxedFrame frame = {
        .ctx = ctx, .caller_ctx = caller_ctx, .b = b, .sf = sf, .var_refs = var_refs,
        .this_obj = this_obj, .sp = *sp, .fuel = QJS_JIT_FUEL,
        .new_target = new_target, .argc = argc, .argv = argv,
    };
    if (rt->jit_cache_tick - code->used >= QJS_JIT_CACHE_EPOCH) {
        qjs_boxed_cache_unlink(rt, code);
        qjs_boxed_cache_link(rt, code);
    }
    /* Native helpers can execute guest code and trigger another installation. */
    code->active++;
    int status = ((QJSBoxedEntry *)code->entry)(&frame, qjs_boxed_helpers);
    code->active--;
    if (unlikely(frame.inline_pc_plus_one)) {
        QJSJitFeedbackSite *site = qjs_jit_feedback_find(b->jit_feedback,
            frame.inline_pc_plus_one - 1, QJS_JIT_FEEDBACK_CALL, false);
        assert(site);
        qjs_jit_feedback_increment(&site->u.call.inline_hits);
    }
    *pc = b->byte_code_buf + frame.pc;
    *sp = frame.sp;
    rt->jit_boxed_entries++;
    rt->jit_boxed_native_shuffles += frame.native_shuffles;
    rt->jit_boxed_native_increfs += frame.native_increfs;
    rt->jit_boxed_native_decrefs += frame.native_decrefs;
    rt->jit_boxed_native_drop_fallbacks += frame.native_drop_fallbacks;
    rt->jit_boxed_short_bigints += frame.native_short_bigints;
    rt->jit_boxed_native_borrowed_fields += frame.native_borrowed_fields;
    rt->jit_boxed_native_retained_fields += frame.native_retained_fields;
    rt->jit_boxed_fields += frame.native_borrowed_fields + frame.native_retained_fields;
    rt->jit_boxed_rc_pairs_elided += frame.native_borrowed_fields;
    if (status == QJS_BOXED_RETURN) {
        *result = frame.result;
        rt->jit_boxed_returns++;
    } else if (status == QJS_BOXED_SUSPEND) {
        *result = frame.result;
        rt->jit_boxed_coro_terminals++;
    } else if (status == QJS_BOXED_EXCEPTION) {
        rt->jit_boxed_exceptions++;
    } else {
        assert(status == QJS_BOXED_RESUME || status == QJS_BOXED_FUEL);
        rt->jit_boxed_resumes++;
        if (status == QJS_BOXED_FUEL)
            rt->jit_boxed_fuel_exits++;
    }
    return status;
}

static int qjs_jit_boxed_enter(JSContext *ctx, JSContext *caller_ctx, JSFunctionBytecode *b,
                               JSStackFrame *sf, JSVarRef **var_refs, JSValue this_obj,
                               JSValue new_target, int argc, JSValueConst *argv,
                               uint8_t **pc, JSValue **sp, JSValue *result)
{
    JSRuntime *rt = ctx->rt;
    QJSBoxedSite *site = &b->jit_boxed_site;
    if (site->rejected || js_check_stack_overflow(rt, QJS_JIT_NATIVE_STACK))
        return QJS_BOXED_CONTINUE;
    qjs_jit_cache_tick(rt);
    if (!site->code && !qjs_boxed_warm(rt, b, 0, 0, site))
        return QJS_BOXED_CONTINUE;

    return qjs_boxed_run(site->code, ctx, caller_ctx, b, sf, var_refs,
                         this_obj, new_target, argc, argv, pc, sp, result);
}

static QJSBoxedCoroSite *qjs_boxed_coro_site(JSRuntime *rt, JSFunctionBytecode *b,
                                          unsigned pc, unsigned depth)
{
    QJSBoxedCoroSites *coro = b->jit_boxed_coro_sites;
    if (!coro) {
        coro = js_mallocz_rt(rt, sizeof(*coro));
        if (!coro)
            return NULL;
        b->jit_boxed_coro_sites = coro;
    }
    for (unsigned i = 0; i < coro->count; i++) {
        QJSBoxedCoroSite *site = &coro->sites[i];
        if (site->pc == pc && site->depth == depth)
            return site;
    }
    if (coro->count == countof(coro->sites))
        return NULL;
    QJSBoxedCoroSite *site = &coro->sites[coro->count++];
    site->pc = pc;
    site->depth = depth;
    return site;
}

static int qjs_jit_boxed_resume(JSContext *ctx, JSContext *caller_ctx, JSFunctionBytecode *b,
                                JSStackFrame *sf, JSVarRef **var_refs, JSValue this_obj,
                                JSValue new_target, int argc, JSValueConst *argv,
                                JSValue *stack_buf, uint8_t **pc, JSValue **sp, JSValue *result)
{
    ptrdiff_t offset = *pc - b->byte_code_buf;
    ptrdiff_t depth = *sp - stack_buf;
    JSRuntime *rt = ctx->rt;
    if (offset < 0 || offset >= b->byte_code_len || b->byte_code_len > QJS_BOXED_MAX_BYTECODE ||
        depth < 0 || depth > b->stack_size || depth > INT16_MAX ||
        js_check_stack_overflow(rt, QJS_JIT_NATIVE_STACK))
        return QJS_BOXED_CONTINUE;

    /* Keep the VM's running-frame invariant. The coroutine caller keeps its
       owner alive, and its active slots hold counted references throughout. */
    assert(sf->cur_gc_obj && !sf->cur_sp && b->func_kind != JS_FUNC_NORMAL);
    qjs_jit_cache_tick(rt);
    QJSBoxedCoroSite *site = qjs_boxed_coro_site(rt, b, offset, depth);
    if (!site || site->jit.rejected ||
        (!site->jit.code && !qjs_boxed_warm(rt, b, offset, depth, &site->jit)))
        return QJS_BOXED_CONTINUE;

    int status = qjs_boxed_run(site->jit.code, ctx, caller_ctx, b, sf, var_refs,
                               this_obj, new_target, argc, argv, pc, sp, result);
    /* The VM suspension epilogue writes cur_sp and transfers the result. */
    assert(status != QJS_BOXED_RETURN);
    if (offset)
        rt->jit_boxed_coro_resumes[b->func_kind]++;
    return status;
}
