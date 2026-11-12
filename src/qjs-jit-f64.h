/* AAPCS64 entry: x0 double arguments, x1 result, w2 fuel, x3 OSR locals.
 * Every live native slot is an unboxed Number. Admission guards in the caller
 * exclude other tags, so arithmetic never invokes coercion or user code. */
enum {
    QJS_F64_LOCAL_REGS = 16,
    QJS_F64_STACK_REGS = 4,
    QJS_F64_INVARIANT_REG = 3,
    QJS_F64_LS = 9,
};

static int qjs_f64_slot_reg(const QJSArm64Emitter *e, int slot, int temp)
{
    if (slot < e->local_regs)
        return 16 + slot;
    if (slot >= e->var_count && slot - e->var_count < e->stack_regs)
        return 4 + slot - e->var_count;
    return temp;
}

static void qjs_f64_move(QJSArm64Emitter *e, int dst, int src)
{
    if (dst != src)
        qjs_arm64_emit(e, 0x1e604000 | (src << 5) | dst); /* fmov dDst, dSrc */
}

static int qjs_f64_read(QJSArm64Emitter *e, int slot, int temp)
{
    int reg = qjs_f64_slot_reg(e, slot, -1);
    if (reg < 0) {
        reg = temp;
        qjs_arm64_emit(e, 0xfd4003e0 | (qjs_arm64_slot_offset(e, slot) << 10) | reg);
    }
    return reg;
}

static void qjs_f64_write(QJSArm64Emitter *e, int slot, int src)
{
    int reg = qjs_f64_slot_reg(e, slot, -1);
    if (reg < 0)
        qjs_arm64_emit(e, 0xfd0003e0 | (qjs_arm64_slot_offset(e, slot) << 10) | src);
    else
        qjs_f64_move(e, reg, src);
}

static void qjs_f64_wide_immediate(QJSArm64Emitter *e, int dst, uint64_t bits)
{
    unsigned part = __builtin_ctzll(bits) / 16;
    unsigned imm = (bits >> (16 * part)) & 0xffff;
    qjs_arm64_emit(e, 0xd2800000 | (part << 21) | (imm << 5) | dst); /* movz */
    for (part++; part < 4; part++) {
        imm = (bits >> (16 * part)) & 0xffff;
        if (imm)
            qjs_arm64_emit(e, 0xf2800000 | (part << 21) | (imm << 5) | dst); /* movk */
    }
}

static void qjs_f64_immediate(QJSArm64Emitter *e, int dst, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));

    unsigned exponent = (bits >> 52) & 0x7ff;
    if (exponent >= 1020 && exponent <= 1027 && !(bits & UINT64_C(0xffffffffffff))) {
        /* FMOV immediate encodes a sign, exponent -3..4 and four fraction bits. */
        unsigned imm = ((bits >> 56) & 0x80) | ((exponent < 1024) << 6) |
                       ((exponent & 3) << 4) | ((bits >> 48) & 15);
        qjs_arm64_emit(e, 0x1e601000 | (imm << 13) | dst);
    } else {
        int src = bits ? 16 : 31;
        if (bits)
            qjs_f64_wide_immediate(e, src, bits);
        qjs_arm64_emit(e, 0x9e670000 | (src << 5) | dst); /* fmov dDst, xSrc */
    }
}

static double qjs_f64_literal(const JSFunctionBytecode *b, const QJSArm64Insn *insn)
{
    return insn->op == OP_push_i32 ? (double)insn->operand :
           JS_VALUE_GET_FLOAT64(b->cpool[insn->operand]);
}

static bool qjs_f64_reciprocal(const JSFunctionBytecode *b, const QJSArm64Insn *literal,
                               double *reciprocal)
{
    if (literal->op != OP_push_i32 && literal->op != OP_push_const)
        return false;
    double divisor = qjs_f64_literal(b, literal);
    uint64_t bits;
    memcpy(&bits, &divisor, sizeof(bits));
    unsigned exponent = (bits >> 52) & 0x7ff;
    if ((bits & UINT64_C(0x000fffffffffffff)) || !exponent || exponent >= 2046)
        return false;

    /* Keep the reciprocal exact and normal. Division and multiplication then
       round the same mathematical result, including signed underflow. */
    bits = (bits & (UINT64_C(1) << 63)) | ((uint64_t)(2046 - exponent) << 52);
    memcpy(reciprocal, &bits, sizeof(bits));
    return true;
}

/* The two numeric helpers use the same IEEE/ECMAScript semantics as the
 * interpreter. Neither receives a runtime or owns a JSValue. */
static void qjs_f64_math_call(QJSArm64Emitter *e, int op, int lhs_slot, int rhs_slot)
{
    int lhs = qjs_f64_read(e, lhs_slot, 0);
    int rhs = qjs_f64_read(e, rhs_slot, 1);
    int stack_regs = min_int(e->stack_regs, lhs_slot - e->var_count);
    unsigned saved = e->local_regs + stack_regs;
    unsigned frame_size = (32 + saved * sizeof(double) + 15) & ~15u;
    uintptr_t helper = op == OP_mod ? (uintptr_t)fmod : (uintptr_t)js_math_pow;

    qjs_arm64_emit(e, 0xd10003ff | (frame_size << 10)); /* sub sp, sp, #frame_size */
    qjs_arm64_emit(e, 0xa90007e0); /* stp x0, x1, [sp] */
    qjs_arm64_emit(e, 0xa9017be2); /* stp x2, x30, [sp, #16] */
    for (unsigned i = 0; i < saved; i++) {
        unsigned reg = i < (unsigned)e->local_regs ? 16 + i : 4 + i - e->local_regs;
        qjs_arm64_emit(e, 0xfd0003e0 | ((4 + i) << 10) | reg); /* str dReg */
    }
    qjs_f64_move(e, 0, lhs);
    qjs_f64_move(e, 1, rhs);
    qjs_f64_wide_immediate(e, 16, helper);
    qjs_arm64_emit(e, 0xd63f0200); /* blr x16 */
    for (unsigned i = 0; i < saved; i++) {
        unsigned reg = i < (unsigned)e->local_regs ? 16 + i : 4 + i - e->local_regs;
        qjs_arm64_emit(e, 0xfd4003e0 | ((4 + i) << 10) | reg); /* ldr dReg */
    }
    qjs_arm64_emit(e, 0xa94007e0); /* ldp x0, x1, [sp] */
    qjs_arm64_emit(e, 0xa9417be2); /* ldp x2, x30, [sp, #16] */
    qjs_arm64_emit(e, 0x910003ff | (frame_size << 10)); /* add sp, sp, #frame_size */
    qjs_f64_write(e, lhs_slot, 0);
}

static uint32_t qjs_f64_binary_opcode(int op)
{
    switch (op) {
    case OP_add: case OP_add_loc: return 0x1e602800; /* fadd */
    case OP_sub: return 0x1e603800; /* fsub */
    case OP_mul: return 0x1e600800; /* fmul */
    case OP_div: return 0x1e601800; /* fdiv */
    default: return 0;
    }
}

static void qjs_f64_binary(QJSArm64Emitter *e, int op, int dst_slot, int lhs_slot, int rhs_slot)
{
    int lhs = qjs_f64_read(e, lhs_slot, 0);
    int rhs = qjs_f64_read(e, rhs_slot, 1);
    int dst = qjs_f64_slot_reg(e, dst_slot, 2);
    uint32_t opcode = qjs_f64_binary_opcode(op);
    assert(opcode);
    qjs_arm64_emit(e, qjs_arm64_rrr(opcode, dst, lhs, rhs));
    qjs_f64_write(e, dst_slot, dst);
}

#if defined(__ARM_FEATURE_JCVT)
static void qjs_f64_bitwise(QJSArm64Emitter *e, int op, int lhs_slot, int rhs_slot)
{
    int lhs = qjs_f64_read(e, lhs_slot, 0);
    int dst = qjs_f64_slot_reg(e, lhs_slot, 2);
    qjs_arm64_emit(e, 0x1e7e0010 | (lhs << 5)); /* fjcvtzs w16, dLhs */
    if (op == OP_not) {
        qjs_arm64_emit(e, 0x2a3003f0); /* mvn w16, w16 */
    } else {
        int rhs = qjs_f64_read(e, rhs_slot, 1);
        uint32_t opcode;
        switch (op) {
        case OP_and: opcode = 0x0a000000; break;
        case OP_xor: opcode = 0x4a000000; break;
        case OP_or:  opcode = 0x2a000000; break;
        case OP_shl: opcode = 0x1ac02000; break; /* lslv */
        case OP_sar: opcode = 0x1ac02800; break; /* asrv */
        case OP_shr: opcode = 0x1ac02400; break; /* lsrv */
        default: abort();
        }
        qjs_arm64_emit(e, 0x1e7e0011 | (rhs << 5)); /* fjcvtzs w17, dRhs */
        qjs_arm64_emit(e, qjs_arm64_rrr(opcode, 16, 16, 17));
    }
    /* The unsigned right shift returns a JavaScript Number in [0, 2^32). */
    uint32_t convert = op == OP_shr ? 0x1e630200 : 0x1e620200; /* ucvtf/scvtf dDst, w16 */
    qjs_arm64_emit(e, convert | dst);
    qjs_f64_write(e, lhs_slot, dst);
}
#endif

static void qjs_f64_step(QJSArm64Emitter *e, int op, int dst_slot, int src_slot)
{
    int src = qjs_f64_read(e, src_slot, 0);
    int dst = qjs_f64_slot_reg(e, dst_slot, 2);
    uint32_t opcode = op == OP_dec || op == OP_post_dec || op == OP_dec_loc ?
                      0x1e603800 : 0x1e602800;
    qjs_f64_immediate(e, 1, 1.0);
    qjs_arm64_emit(e, qjs_arm64_rrr(opcode, dst, src, 1));
    qjs_f64_write(e, dst_slot, dst);
}

static bool qjs_f64_lower(QJSArm64Emitter *e, const JSFunctionBytecode *b,
                          const QJSArm64Insn *insn, int index, unsigned frame_size)
{
    int top = e->var_count + insn->depth;
    int local = insn->operand;
    int src, dst;

    if (qjs_arm64_is_comparison(insn->op)) {
        int lhs = qjs_f64_read(e, top - 2, 0);
        int rhs = qjs_f64_read(e, top - 1, 1);
        /* FCMP sets C and V for unordered. MI and LS exclude unordered,
           unlike the signed-integer LT and LE condition codes. */
        int cond = insn->op == OP_lt ? QJS_ARM64_MI :
                   insn->op == OP_lte ? QJS_F64_LS : qjs_arm64_comparison(insn->op);
        qjs_arm64_emit(e, qjs_arm64_rrr(0x1e602000, 0, lhs, rhs)); /* fcmp */
        qjs_arm64_branch(e, index, insn->target, cond ^ !insn->branch_if_true);
        return true;
    }

    switch (insn->op) {
    case OP_push_i32: case OP_push_const:
        dst = qjs_f64_slot_reg(e, top, 0);
        qjs_f64_immediate(e, dst, qjs_f64_literal(b, insn));
        qjs_f64_write(e, top, dst);
        break;
    case OP_get_arg:
        src = e->arg_regs[local];
        if (!src) {
            src = qjs_f64_slot_reg(e, top, 0);
            qjs_arm64_emit(e, 0xfd400000 | (local << 10) | src); /* ldr dSrc, [x0, #arg] */
        }
        qjs_f64_write(e, top, src);
        break;
    case OP_get_loc0_loc1:
        qjs_f64_write(e, top, qjs_f64_read(e, 0, 0));
        qjs_f64_write(e, top + 1, qjs_f64_read(e, 1, 0));
        break;
    case OP_get_loc:
        qjs_f64_write(e, top, qjs_f64_read(e, local, 0));
        break;
    case OP_put_loc: case OP_set_loc: case OP_put_loc_check:
        qjs_f64_write(e, local, qjs_f64_read(e, top - 1, 0));
        break;
    case OP_dup:
        qjs_f64_write(e, top, qjs_f64_read(e, top - 1, 0));
        break;
    case OP_swap:
        qjs_f64_move(e, 2, qjs_f64_read(e, top - 1, 0));
        qjs_f64_write(e, top - 1, qjs_f64_read(e, top - 2, 1));
        qjs_f64_write(e, top - 2, 2);
        break;
    case OP_inc_loc: case OP_dec_loc:
        qjs_f64_step(e, insn->op, local, local);
        break;
    case OP_inc: case OP_dec:
        qjs_f64_step(e, insn->op, top - 1, top - 1);
        break;
    case OP_post_inc: case OP_post_dec:
        qjs_f64_step(e, insn->op, top, top - 1);
        break;
    case OP_add_loc:
        qjs_f64_binary(e, insn->op, local, local, top - 1);
        break;
    case OP_add: case OP_sub: case OP_mul: case OP_div:
        qjs_f64_binary(e, insn->op, top - 2, top - 2, top - 1);
        break;
    case OP_mod: case OP_pow:
        qjs_f64_math_call(e, insn->op, top - 2, top - 1);
        break;
#if defined(__ARM_FEATURE_JCVT)
    case OP_and: case OP_xor: case OP_or: case OP_shl: case OP_sar: case OP_shr:
        qjs_f64_bitwise(e, insn->op, top - 2, top - 1);
        break;
    case OP_not:
        qjs_f64_bitwise(e, insn->op, top - 1, 0);
        break;
#endif
    case OP_neg:
        src = qjs_f64_read(e, top - 1, 0);
        dst = qjs_f64_slot_reg(e, top - 1, 2);
        qjs_arm64_emit(e, 0x1e614000 | (src << 5) | dst); /* fneg */
        qjs_f64_write(e, top - 1, dst);
        break;
    case OP_if_true: case OP_if_false:
        src = qjs_f64_read(e, top - 1, 0);
        qjs_arm64_emit(e, 0x1e602008 | (src << 5)); /* fcmp dSrc, #0.0 */
        /* Preserve the ordered comparison flags. Set Z for unordered
           because JavaScript treats NaN as false just like signed zeros. */
        qjs_arm64_emit(e, 0x54000047); /* b.vc +8 */
        qjs_arm64_emit(e, 0x6b1f03ff); /* cmp wzr, wzr */
        qjs_arm64_branch(e, index, insn->target,
                         insn->op == OP_if_true ? QJS_ARM64_NE : QJS_ARM64_EQ);
        break;
    case OP_goto:
        qjs_arm64_branch(e, index, insn->target, -1);
        break;
    case OP_return:
        src = qjs_f64_read(e, top - 1, 0);
        _Static_assert(offsetof(QJSJitF64Exit, result) == 0, "F64 result must be first");
        qjs_arm64_emit(e, 0xfd000020 | src); /* str dSrc, [x1] */
        qjs_arm64_epilogue(e, 1, frame_size);
        break;
    case OP_plus: case OP_drop: case OP_nop: case OP_set_loc_uninitialized:
        break;
    default:
        return false;
    }
    return true;
}

static void qjs_f64_prologue(QJSArm64Emitter *e, unsigned frame_size)
{
    if (frame_size)
        qjs_arm64_emit(e, 0xd10003ff | (frame_size << 10)); /* sub sp, sp, #frame_size */
    for (int arg = 0; arg < QJS_JIT_MAX_ARGS; arg++) {
        if (e->arg_regs[arg])
            qjs_arm64_emit(e, 0xfd400000 | (arg << 10) | e->arg_regs[arg]); /* ldr dArg, [x0, #arg] */
    }
}

static bool qjs_f64_invariant_operand(int op)
{
    return op == OP_get_arg || op == OP_push_i32 || op == OP_push_const;
}

static int qjs_f64_invariant(const QJSArm64Insn *insns, int count)
{
    QJSJitCFGLoop loop = qjs_cfg_find_simple_loop(insns, count, NULL);
    if (!loop.length)
        return -1;

    for (int i = loop.header; i + 2 < loop.latch; i++) {
        int op = insns[i + 2].op;
        if (op != OP_add_loc && qjs_f64_binary_opcode(op) &&
            qjs_f64_invariant_operand(insns[i].op) && qjs_f64_invariant_operand(insns[i + 1].op) &&
            !insns[i + 1].branch_target && !insns[i + 2].branch_target)
            return i;
    }
    return -1;
}

static bool qjs_f64_is_leaf(const QJSArm64Insn *insns, int count)
{
    for (int i = 0; i < count; i++) {
        if (insns[i].depth >= 0 && (insns[i].op == OP_mod || insns[i].op == OP_pow))
            return false;
    }
    return true;
}

static void qjs_f64_assign_arg_regs(QJSArm64Emitter *e, const QJSArm64Insn *insns, int count,
                                    int16_t *loop_boundaries, int invariant)
{
    uint16_t uses[QJS_JIT_MAX_ARGS] = {0};
    qjs_arm64_count_loop_args(insns, count, loop_boundaries, invariant, uses);
    for (int reg = 16 + e->local_regs; reg < 32; reg++) {
        int arg = qjs_arm64_take_hot_arg(uses);
        if (arg < 0)
            break;
        e->arg_regs[arg] = reg;
    }
}

static void qjs_f64_emit_invariant(QJSArm64Emitter *e, const JSFunctionBytecode *b,
                                  const QJSArm64Insn *insn)
{
    if (!insn)
        return;

    for (int reg = 0; reg < 2; reg++) {
        int operand = insn[reg].operand;
        if (insn[reg].op == OP_get_arg)
            qjs_arm64_emit(e, 0xfd400000 | (operand << 10) | reg);
        else
            qjs_f64_immediate(e, reg, qjs_f64_literal(b, &insn[reg]));
    }
    qjs_arm64_emit(e, qjs_arm64_rrr(qjs_f64_binary_opcode(insn[2].op), QJS_F64_INVARIANT_REG, 0, 1));
}

static void qjs_f64_emit_osr(QJSArm64Emitter *e, const QJSArm64Insn *insns, int target,
                             unsigned frame_size, QJSJitOSR *osr, const JSFunctionBytecode *b,
                             const QJSArm64Insn *invariant)
{
    if (target < 0)
        return;

    uint32_t initialized = insns[target].initialized;
    *osr = (QJSJitOSR) { .pc = insns[target].pc, .word_offset = e->count,
                        .locals_mask = initialized };
    qjs_f64_prologue(e, frame_size);
    qjs_f64_emit_invariant(e, b, invariant);
    for (uint32_t mask = initialized; mask; mask &= mask - 1) {
        unsigned local = __builtin_ctz(mask);
        int dst = qjs_f64_slot_reg(e, local, 0);
        qjs_arm64_emit(e, 0xfd400060 | (local << 10) | dst); /* ldr dDst, [x3, #local] */
        qjs_f64_write(e, local, dst);
    }
    qjs_arm64_jump(e, 0x14000000, target);
}

static void qjs_f64_emit_precise_exits(QJSArm64Emitter *e, const QJSArm64Insn *insns,
                                      unsigned frame_size)
{
    for (unsigned i = 0; i < e->exit_count; i++) {
        QJSArm64ExitSite site = e->exit_sites[i];
        const QJSArm64Insn *insn = &insns[site.insn];
        e->offsets[QJS_ARM64_EXIT_BASE + i] = e->count;
        qjs_arm64_immediate(e, 16, insn->pc);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitF64Exit, pc), 16);
        qjs_arm64_immediate(e, 16, insn->depth);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitF64Exit, depth), 16);
        qjs_arm64_immediate(e, 16, insn->initialized);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitF64Exit, initialized), 16);
        qjs_arm64_immediate(e, 16, insn->uninitialized);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitF64Exit, uninitialized), 16);
        for (uint32_t mask = insn->initialized; mask; mask &= mask - 1) {
            unsigned j = __builtin_ctz(mask);
            unsigned slot = offsetof(QJSJitF64Exit, locals) / sizeof(double) + j;
            int src = qjs_f64_read(e, j, 0);
            qjs_arm64_emit(e, 0xfd000020 | (slot << 10) | src); /* str dSrc, [x1, #slot] */
        }
        for (int j = 0; j < insn->depth; j++) {
            unsigned slot = offsetof(QJSJitF64Exit, stack) / sizeof(double) + j;
            int src = qjs_f64_read(e, e->var_count + j, 0);
            qjs_arm64_emit(e, 0xfd000020 | (slot << 10) | src);
        }
        qjs_arm64_epilogue(e, site.status, frame_size);
    }
}

static size_t qjs_f64_compile(JSRuntime *rt, QJSArm64Workspace *w, const JSFunctionBytecode *b,
                             bool optimized, uint32_t *code, size_t code_words,
                             QJSJitOSR *osr)
{
    QJSArm64Insn *insns = w->insns;
    int count = qjs_arm64_decode(b, insns, w->scratch.indices, true);
    if (!count || !qjs_arm64_verify(insns, count, b->stack_size, w->scratch.queue))
        return 0;

    QJSArm64Emitter *e = &w->emitter;
    e->code = code;
    e->capacity = code_words;
    e->var_count = b->var_count;
    e->local_regs = optimized ? min_int(b->var_count, QJS_F64_LOCAL_REGS) : 0;
    e->stack_regs = optimized ? min_int(b->stack_size, QJS_F64_STACK_REGS) : 0;
    e->int_insns = insns;
    e->all_locals = (UINT64_C(1) << b->var_count) - 1;
    int osr_header = qjs_arm64_osr_header(insns, count);
    unsigned ssa_phis = 0, ssa_stores_elided = 0;
    uint8_t *ssa = optimized ?
        qjs_ssa_plan(rt, insns, count, b->var_count, osr_header, QJS_SSA_FLOAT64, &ssa_phis) : NULL;
    /* Helper calls may clobber the caller-saved argument and invariant registers. */
    bool keep_registers = optimized && qjs_f64_is_leaf(insns, count);
    int invariant_index = keep_registers ? qjs_f64_invariant(insns, count) : -1;
    if (keep_registers)
        qjs_f64_assign_arg_regs(e, insns, count, w->scratch.loop_boundaries, invariant_index);
    const QJSArm64Insn *invariant = invariant_index >= 0 ? &insns[invariant_index] : NULL;
    QJSJitCFGLayout layout = {.condition = -1};
    if (optimized && !invariant && b->jit_feedback)
        layout = qjs_cfg_layout(b, insns, count, NULL);
    unsigned slots = b->var_count - e->local_regs + b->stack_size - e->stack_regs;
    unsigned frame_size = (slots * sizeof(double) + 15) & ~15u;
    qjs_f64_prologue(e, frame_size);
    qjs_f64_emit_invariant(e, b, invariant);
    for (int position = 0; position < count; position++) {
        int i = qjs_cfg_layout_index(&layout, position);
        e->offsets[i] = e->count;
        if (insns[i].depth < 0)
            continue;
        if (ssa && ssa[i] == QJS_SSA_ELIDE_STORE) {
            ssa_stores_elided++;
            continue;
        }
        if (i == invariant_index) {
            qjs_f64_write(e, e->var_count + insns[i].depth, QJS_F64_INVARIANT_REG);
            position += 2;
            continue;
        }
        double reciprocal;
        if (optimized && i + 1 < count && position + 1 < count &&
            qjs_cfg_layout_index(&layout, position + 1) == i + 1 && insns[i + 1].op == OP_div &&
            !insns[i + 1].branch_target && qjs_f64_reciprocal(b, &insns[i], &reciprocal)) {
            int slot = e->var_count + insns[i].depth - 1;
            int lhs = qjs_f64_read(e, slot, 0);
            int dst = qjs_f64_slot_reg(e, slot, 2);
            qjs_f64_immediate(e, 1, reciprocal);
            qjs_arm64_emit(e, qjs_arm64_rrr(qjs_f64_binary_opcode(OP_mul), dst, lhs, 1));
            qjs_f64_write(e, slot, dst);
            position++;
            continue;
        }
        if (i == layout.condition) {
            QJSArm64Insn branch = qjs_cfg_layout_branch(&insns[i], i + 1);
            qjs_f64_lower(e, b, &branch, i, frame_size);
            continue;
        }
        if (!qjs_f64_lower(e, b, &insns[i], i, frame_size)) {
            js_free_rt(rt, ssa);
            return 0;
        }
    }
    js_free_rt(rt, ssa);
    e->offsets[QJS_ARM64_FUEL] = e->count;
    qjs_arm64_epilogue(e, 2, frame_size);
    qjs_f64_emit_precise_exits(e, insns, frame_size);
    qjs_f64_emit_osr(e, insns, osr_header, frame_size, osr, b, invariant);
    size_t words = qjs_arm64_finish(e);
    if (words) {
        rt->jit_ssa_f64_phis += ssa_phis;
        rt->jit_ssa_f64_stores_elided += ssa_stores_elided;
        rt->jit_cfg_invariants_hoisted += invariant != NULL;
        rt->jit_cfg_hot_layouts += layout.condition >= 0;
    }
    return words;
}
