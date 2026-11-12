/* Included in quickjs.c after short_opcode_info. */
enum {
    QJS_ARM64_MAX_BYTECODE = 512,
    QJS_ARM64_MAX_STACK = QJS_JIT_MAX_STACK,
    QJS_ARM64_LOCAL_REGS = 7,
    QJS_ARM64_STACK_REGS = 5,
    QJS_ARM64_REPLAY = QJS_ARM64_MAX_BYTECODE,
    QJS_ARM64_FUEL = QJS_ARM64_MAX_BYTECODE + 1,
    QJS_ARM64_EXIT_BASE = QJS_ARM64_MAX_BYTECODE + 2,
    QJS_ARM64_MAX_PRECISE_EXITS = 32,
};

_Static_assert(QJS_JIT_MAX_LOCALS <= 32, "JIT locals must fit the initialization mask");

typedef struct {
    int32_t operand;
    int16_t target;
    uint16_t pc;
    int8_t depth;
    uint8_t op, pop, push;
    uint32_t initialized, undefined, uninitialized;
    bool queued;
    bool branch_if_true;
    bool branch_target;
} QJSArm64Insn;

typedef struct {
    int16_t target;
    uint16_t word;
} QJSArm64Fixup;

typedef struct {
    uint16_t insn;
    uint8_t status;
} QJSArm64ExitSite;

typedef struct {
    uint32_t *code;
    size_t capacity, count;
    int var_count, local_regs, stack_regs;
    bool failed;
    uint8_t arg_regs[QJS_JIT_MAX_ARGS];
    uint16_t offsets[QJS_ARM64_EXIT_BASE + QJS_ARM64_MAX_PRECISE_EXITS];
    QJSArm64ExitSite exit_sites[QJS_ARM64_MAX_PRECISE_EXITS];
    const QJSArm64Insn *int_insns;
    uint32_t all_locals;
    unsigned exit_count;
    unsigned precise_insn_plus_one;
    unsigned unroll_latch_plus_one;
    QJSArm64Fixup fixups[QJS_ARM64_MAX_BYTECODE * 4 + 1];
    size_t fixup_count;
} QJSArm64Emitter;

typedef struct {
    QJSArm64Insn insns[QJS_ARM64_MAX_BYTECODE];
    QJSArm64Emitter emitter;
    union {
        int16_t indices[QJS_ARM64_MAX_BYTECODE];
        uint16_t queue[QJS_ARM64_MAX_BYTECODE];
        int16_t loop_boundaries[QJS_ARM64_MAX_BYTECODE + 1];
    } scratch;
} QJSArm64Workspace;

static bool qjs_arm64_is_comparison(int op)
{
    switch (op) {
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
    case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
        return true;
    default:
        return false;
    }
}

/* Decode a closed, side-effect-free int32 subset. Each instruction has an
 * integer-only stack effect. Comparison results cannot escape as integers. */
static int qjs_arm64_decode(const JSFunctionBytecode *b, QJSArm64Insn *insns,
                            int16_t *indices, bool allow_f64)
{
    int len = b->byte_code_len;
    const uint8_t *bytecode = b->byte_code_buf;
    int count = 0;

    for (int i = 0; i < len; i++)
        indices[i] = -1;
    for (int pc = 0; pc < len;) {
        int op = bytecode[pc];
        if (op >= OP_COUNT)
            return 0;
        const JSOpCode *info = &short_opcode_info(op);
        int size = info->size;
        if (size < 1 || size > len - pc)
            return 0;
        const uint8_t *imm = bytecode + pc + 1;
        QJSArm64Insn *insn = &insns[count];
        *insn = (QJSArm64Insn) {
            .op = op, .pop = info->n_pop, .push = info->n_push,
            .pc = pc, .depth = -1, .target = -1,
        };
        indices[pc] = count++;
        int64_t target = -1;
        int local = -1;
        int arg = -1;

        bool comparison = qjs_arm64_is_comparison(op);
        if (comparison) {
            if (pc + size == len)
                return 0;
            int branch = bytecode[pc + size];
            const uint8_t *branch_imm = imm + size;
            if (branch == OP_if_true8 || branch == OP_if_false8) {
                if (len - pc - size < 2)
                    return 0;
                target = pc + size + 1 + (int8_t)branch_imm[0];
                size += 2;
            } else if (branch == OP_if_true || branch == OP_if_false) {
                if (len - pc - size < 5)
                    return 0;
                target = pc + size + 1 + (int64_t)(int32_t)get_u32(branch_imm);
                size += 5;
            } else {
                return 0;
            }
            insn->branch_if_true = branch == OP_if_true || branch == OP_if_true8;
            insn->push = 0;
        } else {
            switch (op) {
            case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2:
            case OP_push_3: case OP_push_4: case OP_push_5: case OP_push_6:
            case OP_push_7:
                insn->op = OP_push_i32;
                insn->operand = op - OP_push_0;
                break;
            case OP_push_i8: case OP_push_i16: case OP_push_i32:
                insn->op = OP_push_i32;
                insn->operand = op == OP_push_i8 ? (int8_t)imm[0] :
                                op == OP_push_i16 ? (int16_t)get_u16(imm) :
                                (int32_t)get_u32(imm);
                break;
            case OP_push_const8: case OP_push_const: {
                uint32_t index = op == OP_push_const8 ? imm[0] : get_u32(imm);
                if (b->cpool_count <= 0 || index >= (uint32_t)b->cpool_count)
                    return 0;
                int tag = JS_VALUE_GET_TAG(b->cpool[index]);
                if (tag == JS_TAG_INT) {
                    insn->op = OP_push_i32;
                    insn->operand = JS_VALUE_GET_INT(b->cpool[index]);
                } else if (allow_f64 && JS_TAG_IS_FLOAT64(tag)) {
                    insn->op = OP_push_const;
                    insn->operand = index;
                } else {
                    return 0;
                }
                break;
            }
            case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
                arg = op - OP_get_arg0;
                insn->op = OP_get_arg;
                break;
            case OP_get_arg:
                arg = get_u16(imm);
                break;
            case OP_get_loc0_loc1:
                if (b->var_count < 2)
                    return 0;
                break;
            case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
                local = op - OP_get_loc0;
                insn->op = OP_get_loc;
                break;
            case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
                local = op - OP_put_loc0;
                insn->op = OP_put_loc;
                break;
            case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
                local = op - OP_set_loc0;
                insn->op = OP_set_loc;
                break;
            case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
                local = imm[0];
                insn->op = op == OP_get_loc8 ? OP_get_loc :
                           op == OP_put_loc8 ? OP_put_loc : OP_set_loc;
                break;
            case OP_get_loc: case OP_put_loc: case OP_set_loc:
            case OP_get_loc_check: case OP_put_loc_check: case OP_set_loc_uninitialized:
                local = get_u16(imm);
                if (op == OP_get_loc_check)
                    insn->op = OP_get_loc;
                break;
            case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
                local = imm[0];
                break;
            case OP_if_true: case OP_if_false: case OP_goto:
            case OP_if_true8: case OP_if_false8: case OP_goto8: case OP_goto16: {
                int32_t rel = size == 2 ? (int8_t)imm[0] :
                              size == 3 ? (int16_t)get_u16(imm) :
                              (int32_t)get_u32(imm);
                target = pc + 1 + (int64_t)rel;
                insn->op = op == OP_if_true8 || op == OP_if_true ? OP_if_true :
                           op == OP_if_false8 || op == OP_if_false ? OP_if_false :
                           OP_goto;
                break;
            }
            case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
            case OP_and: case OP_xor: case OP_or: case OP_shl: case OP_sar: case OP_shr:
            case OP_neg: case OP_plus: case OP_not: case OP_inc: case OP_dec:
            case OP_post_inc: case OP_post_dec:
            case OP_drop: case OP_dup: case OP_swap: case OP_nop: case OP_return:
                break;
            case OP_pow:
                if (!allow_f64)
                    return 0;
                break;
            default:
                return 0;
            }
        }
        if (comparison || insn->op == OP_goto || insn->op == OP_if_true ||
            insn->op == OP_if_false) {
            if (target < 0 || target >= len)
                return 0;
            insn->target = target;
        }
        if (local >= 0) {
            if (local >= b->var_count)
                return 0;
            insn->operand = local;
        }
        if (arg >= 0) {
            if (arg >= b->arg_count)
                return 0;
            insn->operand = arg;
        }
        pc += size;
    }

    for (int i = 0; i < count; i++) {
        int target = insns[i].target;
        if (target >= 0) {
            if (indices[target] < 0)
                return 0;
            insns[i].target = indices[target];
            insns[indices[target]].branch_target = true;
        }
    }
    return count;
}

static bool qjs_arm64_verify(QJSArm64Insn *insns, int count, int max_depth,
                             uint16_t *queue)
{
    unsigned head = 0, tail = 0;
    insns[0].depth = 0;
    insns[0].undefined = UINT32_MAX;
    insns[0].queued = true;
    queue[tail++ % QJS_ARM64_MAX_BYTECODE] = 0;

    while (head != tail) {
        int i = queue[head++ % QJS_ARM64_MAX_BYTECODE];
        QJSArm64Insn *insn = &insns[i];
        insn->queued = false;
        int depth = insn->depth - insn->pop + insn->push;
        if (insn->depth < insn->pop || depth > max_depth)
            return false;
        uint32_t initialized = insn->initialized;
        uint32_t undefined = insn->undefined;
        uint32_t uninitialized = insn->uninitialized;

        switch (insn->op) {
        case OP_get_loc0_loc1:
            if ((initialized & 3) != 3)
                return false;
            break;
        case OP_get_loc: case OP_put_loc_check:
        case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
            if (!(initialized & (UINT32_C(1) << insn->operand)))
                return false;
            break;
        case OP_put_loc: case OP_set_loc: {
            uint32_t bit = UINT32_C(1) << insn->operand;
            initialized |= bit;
            undefined &= ~bit;
            uninitialized &= ~bit;
            break;
        }
        case OP_set_loc_uninitialized: {
            uint32_t bit = UINT32_C(1) << insn->operand;
            initialized &= ~bit;
            undefined &= ~bit;
            uninitialized |= bit;
            break;
        }
        case OP_return:
            if (depth != 0)
                return false;
            continue;
        }
        int successors[] = { insn->target, insn->op == OP_goto ? -1 : i + 1 };
        for (int edge = 0; edge < 2; edge++) {
            int next = successors[edge];
            if (next < 0)
                continue;
            if (next >= count)
                return false;
            QJSArm64Insn *dest = &insns[next];
            if (dest->depth == depth) {
                if (!(dest->initialized & ~initialized) && !(dest->undefined & ~undefined) &&
                    !(dest->uninitialized & ~uninitialized))
                    continue;
                dest->initialized &= initialized;
                dest->undefined &= undefined;
                dest->uninitialized &= uninitialized;
            } else if (dest->depth < 0) {
                dest->depth = depth;
                dest->initialized = initialized;
                dest->undefined = undefined;
                dest->uninitialized = uninitialized;
            } else {
                return false;
            }
            if (!dest->queued) {
                dest->queued = true;
                queue[tail++ % QJS_ARM64_MAX_BYTECODE] = next;
            }
        }
    }
    return true;
}

/* AAPCS64 leaf entry: x0 int32 args, x1 result pointer, w2 back-edge fuel.
 * Only caller-saved registers are used. JIT slots hold no owning JSValues. */
static void qjs_arm64_emit(QJSArm64Emitter *e, uint32_t word)
{
    if (e->count < e->capacity)
        e->code[e->count++] = word;
    else
        e->failed = true;
}

static int qjs_arm64_precise_target(QJSArm64Emitter *e, unsigned index, int status, int fallback)
{
    for (unsigned i = 0; i < e->exit_count; i++) {
        if (e->exit_sites[i].insn == index && e->exit_sites[i].status == status)
            return QJS_ARM64_EXIT_BASE + i;
    }
    if (e->exit_count == QJS_ARM64_MAX_PRECISE_EXITS)
        return fallback;
    unsigned slot = e->exit_count++;
    e->exit_sites[slot] = (QJSArm64ExitSite) { index, status };
    return QJS_ARM64_EXIT_BASE + slot;
}

static void qjs_arm64_jump(QJSArm64Emitter *e, uint32_t word, int target)
{
    if (e->fixup_count == countof(e->fixups)) {
        e->failed = true;
        return;
    }
    if (target == QJS_ARM64_REPLAY && e->precise_insn_plus_one)
        target = qjs_arm64_precise_target(e, e->precise_insn_plus_one - 1,
                                          QJS_JIT_EXIT_PRECISE_GUARD, target);
    e->fixups[e->fixup_count++] = (QJSArm64Fixup) { target, e->count };
    qjs_arm64_emit(e, word);
}

enum {
    QJS_ARM64_EQ = 0, QJS_ARM64_NE = 1, QJS_ARM64_HS = 2, QJS_ARM64_MI = 4, QJS_ARM64_VS = 6,
    QJS_ARM64_GE = 10, QJS_ARM64_LT = 11, QJS_ARM64_GT = 12, QJS_ARM64_LE = 13,
};

static void qjs_arm64_bcond(QJSArm64Emitter *e, int cond, int target)
{
    qjs_arm64_jump(e, 0x54000000 | cond, target);
}

static uint32_t qjs_arm64_rrr(uint32_t opcode, int dst, int left, int right)
{
    return opcode | (right << 16) | (left << 5) | dst;
}

static void qjs_arm64_move(QJSArm64Emitter *e, int dst, int src)
{
    if (dst != src)
        qjs_arm64_emit(e, qjs_arm64_rrr(0x2a000000, dst, 31, src)); /* mov */
}

static void qjs_arm64_immediate(QJSArm64Emitter *e, int dst, uint32_t value)
{
    if ((value >> 16) == 0xffff) {
        qjs_arm64_emit(e, 0x12800000 | ((~value & 0xffff) << 5) | dst); /* movn */
    } else {
        qjs_arm64_emit(e, 0x52800000 | ((value & 0xffff) << 5) | dst); /* movz */
        if (value >> 16)
            qjs_arm64_emit(e, 0x72a00000 | ((value >> 16) << 5) | dst); /* movk */
    }
}

static int qjs_arm64_slot_reg(const QJSArm64Emitter *e, int slot, int temp)
{
    if (slot < e->local_regs)
        return 9 + slot;
    if (slot >= e->var_count && slot - e->var_count < e->stack_regs)
        return 4 + slot - e->var_count;
    return temp;
}

static int qjs_arm64_slot_offset(const QJSArm64Emitter *e, int slot)
{
    if (slot < e->var_count)
        return slot - e->local_regs;
    return slot - e->local_regs - e->stack_regs;
}

static int qjs_arm64_read(QJSArm64Emitter *e, int slot, int temp)
{
    int reg = qjs_arm64_slot_reg(e, slot, temp);
    if (reg == temp)
        qjs_arm64_emit(e, 0xb94003e0 | (qjs_arm64_slot_offset(e, slot) << 10) | reg); /* ldr */
    return reg;
}

static void qjs_arm64_write(QJSArm64Emitter *e, int slot, int src)
{
    int reg = qjs_arm64_slot_reg(e, slot, -1);
    if (reg < 0)
        qjs_arm64_emit(e, 0xb90003e0 | (qjs_arm64_slot_offset(e, slot) << 10) | src); /* str */
    else
        qjs_arm64_move(e, reg, src);
}

static int qjs_arm64_comparison(int op)
{
    switch (op) {
    case OP_lt: return QJS_ARM64_LT;
    case OP_lte: return QJS_ARM64_LE;
    case OP_gt: return QJS_ARM64_GT;
    case OP_gte: return QJS_ARM64_GE;
    case OP_eq: case OP_strict_eq: return QJS_ARM64_EQ;
    default: return QJS_ARM64_NE;
    }
}

static bool qjs_arm64_locals_known(const QJSArm64Emitter *e, const QJSArm64Insn *insn)
{
    return ((insn->initialized | insn->undefined | insn->uninitialized) & e->all_locals) ==
           e->all_locals;
}

static void qjs_arm64_branch(QJSArm64Emitter *e, int from, int target, int cond)
{
    if (target > from && cond >= 0) {
        qjs_arm64_bcond(e, cond, target);
        return;
    }
    if (target <= from) {
        int exit_target = QJS_ARM64_FUEL;
        /* Branch operands have been consumed. The target depth describes
           exactly the slots that remain live in the interpreter. */
        if (e->int_insns && qjs_arm64_locals_known(e, &e->int_insns[target]))
            exit_target = qjs_arm64_precise_target(e, target, QJS_JIT_EXIT_PRECISE_FUEL, exit_target);
        if (e->unroll_latch_plus_one == (unsigned)from + 1) {
            if (cond >= 0)
                qjs_arm64_bcond(e, cond ^ 1, from + 1);
            qjs_arm64_emit(e, 0x71000442); /* subs w2, w2, #1 */
            qjs_arm64_bcond(e, QJS_ARM64_HS ^ 1, exit_target);
            return; /* The duplicate loop body is physically next. */
        }
        if (cond >= 0)
            qjs_arm64_emit(e, 0x54000080 | (cond ^ 1)); /* b.<inverse> +16 */
        qjs_arm64_emit(e, 0x71000442); /* subs w2, w2, #1 */
        qjs_arm64_bcond(e, QJS_ARM64_HS, target);
        qjs_arm64_jump(e, 0x14000000, exit_target);
        return;
    }
    qjs_arm64_jump(e, 0x14000000, target);
}

static void qjs_arm64_epilogue(QJSArm64Emitter *e, int status, unsigned frame_size)
{
    qjs_arm64_immediate(e, 0, status);
    if (frame_size)
        qjs_arm64_emit(e, 0x910003ff | (frame_size << 10)); /* add sp, sp, #frame_size */
    qjs_arm64_emit(e, 0xd65f03c0); /* ret */
}

static size_t qjs_arm64_finish(QJSArm64Emitter *e)
{
    if (e->failed)
        return 0;

    /* The bytecode cap puts every branch well within the 19-bit range. */
    for (size_t i = 0; i < e->fixup_count; i++) {
        QJSArm64Fixup f = e->fixups[i];
        int displacement = e->offsets[f.target] - f.word;
        if (e->code[f.word] == 0x14000000)
            e->code[f.word] |= (uint32_t)displacement & 0x03ffffff;
        else
            e->code[f.word] |= ((uint32_t)displacement & 0x7ffff) << 5;
    }
    return e->count;
}

static void qjs_arm64_arithmetic(QJSArm64Emitter *e, int op, int dst_slot, int lhs_slot, int rhs_slot)
{
    int left = qjs_arm64_read(e, lhs_slot, 16);
    int right = qjs_arm64_read(e, rhs_slot, 17);
    int dst = qjs_arm64_slot_reg(e, dst_slot, 3);
    uint32_t opcode;

    switch (op) {
    case OP_mul:
        qjs_arm64_emit(e, qjs_arm64_rrr(0x9b207c00, 3, left, right)); /* smull x3 */
        qjs_arm64_emit(e, 0xeb23c07f); /* cmp x3, w3, sxtw */
        qjs_arm64_bcond(e, QJS_ARM64_NE, QJS_ARM64_REPLAY);
        qjs_arm64_emit(e, 0x35000083); /* cbnz w3, +16 */
        qjs_arm64_emit(e, qjs_arm64_rrr(0x2a000000, 16, left, right)); /* orr w16 */
        qjs_arm64_emit(e, 0x7100021f); /* cmp w16, #0 */
        qjs_arm64_bcond(e, QJS_ARM64_MI, QJS_ARM64_REPLAY); /* negative zero */
        qjs_arm64_write(e, dst_slot, 3);
        return;
    case OP_div: case OP_mod:
        qjs_arm64_jump(e, 0x34000000 | right, QJS_ARM64_REPLAY); /* cbz (zero divisor) */
        if (op == OP_div) {
            /* A 64-bit quotient catches INT32_MIN / -1 before truncation. */
            qjs_arm64_emit(e, 0x93407c10 | (left << 5)); /* sxtw x16, wLeft */
            qjs_arm64_emit(e, 0x93407c11 | (right << 5)); /* sxtw x17, wRight */
            qjs_arm64_emit(e, 0x9ad10e03); /* sdiv x3, x16, x17 */
            qjs_arm64_emit(e, 0xeb23c07f); /* cmp x3, w3, sxtw */
            qjs_arm64_bcond(e, QJS_ARM64_NE, QJS_ARM64_REPLAY);
            qjs_arm64_emit(e, 0x1b11c070); /* msub w16, w3, w17, w16 */
            qjs_arm64_jump(e, 0x35000010, QJS_ARM64_REPLAY); /* cbnz w16 (fractional quotient) */
            left = 17; /* An exact zero quotient has the divisor's sign. */
        } else {
            qjs_arm64_emit(e, qjs_arm64_rrr(0x1ac00c00, 3, left, right)); /* sdiv w3 */
            qjs_arm64_emit(e, qjs_arm64_rrr(0x1b008000 | (left << 10), 3, 3, right)); /* msub w3 */
        }
        qjs_arm64_emit(e, 0x35000063); /* cbnz w3, +12 */
        qjs_arm64_emit(e, 0x7100001f | (left << 5)); /* cmp sign source, #0 */
        qjs_arm64_bcond(e, QJS_ARM64_MI, QJS_ARM64_REPLAY);
        qjs_arm64_write(e, dst_slot, 3);
        return;
    case OP_add: case OP_add_loc: opcode = 0x2b000000; break; /* adds */
    case OP_sub: opcode = 0x6b000000; break; /* subs */
    case OP_and: opcode = 0x0a000000; break;
    case OP_xor: opcode = 0x4a000000; break;
    case OP_or:  opcode = 0x2a000000; break;
    case OP_shl: opcode = 0x1ac02000; break; /* lslv */
    case OP_sar: opcode = 0x1ac02800; break; /* asrv */
    case OP_shr: opcode = 0x1ac02400; break; /* lsrv */
    default: abort();
    }
    if (e->precise_insn_plus_one &&
        (op == OP_add || op == OP_add_loc || op == OP_sub || op == OP_shr))
        dst = 3;
    qjs_arm64_emit(e, qjs_arm64_rrr(opcode, dst, left, right));
    if (op == OP_add || op == OP_add_loc || op == OP_sub)
        qjs_arm64_bcond(e, QJS_ARM64_VS, QJS_ARM64_REPLAY);
    if (op == OP_shr) {
        qjs_arm64_emit(e, 0x7100001f | (dst << 5)); /* cmp wDst, #0 */
        qjs_arm64_bcond(e, QJS_ARM64_MI, QJS_ARM64_REPLAY);
    }
    qjs_arm64_write(e, dst_slot, dst);
}

static void qjs_arm64_step(QJSArm64Emitter *e, int op, int dst_slot, int src_slot)
{
    int src = qjs_arm64_read(e, src_slot, 16);
    int dst = qjs_arm64_slot_reg(e, dst_slot, 3);
    if (e->precise_insn_plus_one)
        dst = 3;
    uint32_t opcode = op == OP_dec || op == OP_post_dec || op == OP_dec_loc ?
                      0x71000400 : 0x31000400; /* subs/adds wDst, wSrc, #1 */
    qjs_arm64_emit(e, opcode | (src << 5) | dst);
    qjs_arm64_bcond(e, QJS_ARM64_VS, QJS_ARM64_REPLAY);
    qjs_arm64_write(e, dst_slot, dst);
}

static void qjs_arm64_lower_mul_constant(QJSArm64Emitter *e, int slot, int32_t value)
{
    if (value == 1)
        return;
    int lhs = qjs_arm64_read(e, slot, 16);
    if (value == 0) {
        qjs_arm64_emit(e, 0x7100001f | (lhs << 5)); /* cmp wLhs, #0 */
        qjs_arm64_bcond(e, QJS_ARM64_MI, QJS_ARM64_REPLAY);
        qjs_arm64_write(e, slot, 31);
        return;
    }
    if (value < 0)
        qjs_arm64_jump(e, 0x34000000 | lhs, QJS_ARM64_REPLAY); /* negative zero */

    uint32_t magnitude = value < 0 ? -(uint32_t)value : (uint32_t)value;
    if (!(magnitude & (magnitude - 1))) {
        unsigned rotation = (64 - __builtin_ctz(magnitude)) & 63;
        /* Sign-extend and shift in a single 64-bit instruction. The full
           result is required for the int32 overflow guard. */
        qjs_arm64_emit(e, 0x93407c03 | (rotation << 16) | (lhs << 5)); /* sbfiz x3 */
        if (value < 0)
            qjs_arm64_emit(e, 0xcb0303e3); /* neg x3, x3 */
    } else {
        qjs_arm64_immediate(e, 17, value);
        qjs_arm64_emit(e, qjs_arm64_rrr(0x9b207c00, 3, lhs, 17)); /* smull x3 */
    }
    qjs_arm64_emit(e, 0xeb23c07f); /* cmp x3, w3, sxtw */
    qjs_arm64_bcond(e, QJS_ARM64_NE, QJS_ARM64_REPLAY);
    qjs_arm64_write(e, slot, 3);
}

static bool qjs_arm64_lower_constant(QJSArm64Emitter *e, int32_t value,
                                      const QJSArm64Insn *insn, int index)
{
    int top = e->var_count + insn->depth;
    int lhs_slot = insn->op == OP_add_loc ? insn->operand : top - 2;
    uint32_t magnitude = value < 0 ? -(uint32_t)value : (uint32_t)value;
    bool negative = value < 0;

    if (qjs_arm64_is_comparison(insn->op)) {
        if (magnitude > 4095)
            return false;
        int lhs = qjs_arm64_read(e, lhs_slot, 16);
        uint32_t opcode = negative ? 0x3100001f : 0x7100001f; /* cmn/cmp immediate */
        qjs_arm64_emit(e, opcode | (magnitude << 10) | (lhs << 5));
        int cond = qjs_arm64_comparison(insn->op) ^ !insn->branch_if_true;
        qjs_arm64_branch(e, index, insn->target, cond);
        return true;
    }

    switch (insn->op) {
    case OP_mul:
        qjs_arm64_lower_mul_constant(e, lhs_slot, value);
        return true;
    case OP_add: case OP_add_loc: case OP_sub: {
        if (magnitude > 4095)
            return false;
        int lhs = qjs_arm64_read(e, lhs_slot, 16);
        int dst = qjs_arm64_slot_reg(e, lhs_slot, 3);
        if (e->precise_insn_plus_one)
            dst = 3;
        uint32_t opcode = (negative ^ (insn->op == OP_sub)) ?
                          0x71000000 : 0x31000000; /* subs/adds immediate */
        qjs_arm64_emit(e, opcode | (magnitude << 10) | (lhs << 5) | dst);
        qjs_arm64_bcond(e, QJS_ARM64_VS, QJS_ARM64_REPLAY);
        qjs_arm64_write(e, lhs_slot, dst);
        return true;
    }
    case OP_div: case OP_mod:
        if (!magnitude || (magnitude & (magnitude - 1)))
            return false;
        break;
    default:
        return false;
    }

    int lhs = qjs_arm64_read(e, lhs_slot, 16);
    unsigned shift = __builtin_ctz(magnitude);
    if (insn->op == OP_div) {
        if (shift) {
            /* Exact quotients have no low bits. Signed shift then divides
               exactly, including for negative operands. */
            qjs_arm64_emit(e, 0x7200001f | ((shift - 1) << 10) | (lhs << 5)); /* tst */
            qjs_arm64_bcond(e, QJS_ARM64_NE, QJS_ARM64_REPLAY);
            qjs_arm64_emit(e, 0x13007c03 | (shift << 16) | (lhs << 5)); /* asr w3 */
        } else {
            qjs_arm64_move(e, 3, lhs);
        }
        if (negative) {
            qjs_arm64_jump(e, 0x34000003, QJS_ARM64_REPLAY); /* negative zero */
            qjs_arm64_emit(e, 0x6b0303e3); /* negs w3, w3 */
            if (!shift)
                qjs_arm64_bcond(e, QJS_ARM64_VS, QJS_ARM64_REPLAY);
        }
    } else if (shift) {
        /* A negative dividend requires subtracting the divisor magnitude
           from the low bits, or preserving negative zero via the interpreter. */
        qjs_arm64_emit(e, 0x12000003 | ((shift - 1) << 10) | (lhs << 5)); /* and w3 */
        qjs_arm64_emit(e, 0x7100001f | (lhs << 5)); /* cmp wLhs, #0 */
        qjs_arm64_emit(e, 0x5400006a); /* b.ge +12 */
        qjs_arm64_jump(e, 0x34000003, QJS_ARM64_REPLAY);
        qjs_arm64_emit(e, 0x32000063 | ((32 - shift) << 16) |
                          ((31 - shift) << 10)); /* orr w3, w3, ~mask */
    } else {
        qjs_arm64_emit(e, 0x7100001f | (lhs << 5)); /* cmp wLhs, #0 */
        qjs_arm64_bcond(e, QJS_ARM64_MI, QJS_ARM64_REPLAY);
        qjs_arm64_move(e, 3, 31);
    }
    qjs_arm64_write(e, lhs_slot, 3);
    return true;
}

static void qjs_arm64_lower(QJSArm64Emitter *e, const QJSArm64Insn *insn, int index,
                            bool optimized, unsigned frame_size)
{
    int top = e->var_count + insn->depth;
    int local = insn->operand;
    int src, dst;

    if (qjs_arm64_is_comparison(insn->op)) {
        int lhs = qjs_arm64_read(e, top - 2, 16);
        int rhs = qjs_arm64_read(e, top - 1, 17);
        qjs_arm64_emit(e, qjs_arm64_rrr(0x6b000000, 31, lhs, rhs)); /* cmp */
        int cond = qjs_arm64_comparison(insn->op);
        if (!optimized) {
            qjs_arm64_emit(e, 0x1a9f07f0 | ((cond ^ 1) << 12)); /* cset w16 */
            qjs_arm64_emit(e, 0x7100021f); /* cmp w16, #0 */
            cond = QJS_ARM64_NE;
        }
        qjs_arm64_branch(e, index, insn->target, cond ^ !insn->branch_if_true);
        return;
    }

    switch (insn->op) {
    case OP_push_i32:
        dst = qjs_arm64_slot_reg(e, top, 16);
        qjs_arm64_immediate(e, dst, local);
        qjs_arm64_write(e, top, dst);
        break;
    case OP_get_arg:
        src = e->arg_regs[local];
        if (!src) {
            src = qjs_arm64_slot_reg(e, top, 16);
            qjs_arm64_emit(e, 0xb9400000 | (local << 10) | src); /* ldr wSrc, [x0, #arg] */
        }
        qjs_arm64_write(e, top, src);
        break;
    case OP_get_loc0_loc1:
        qjs_arm64_write(e, top, qjs_arm64_read(e, 0, 16));
        qjs_arm64_write(e, top + 1, qjs_arm64_read(e, 1, 16));
        break;
    case OP_get_loc:
        qjs_arm64_write(e, top, qjs_arm64_read(e, local, 16));
        break;
    case OP_put_loc: case OP_set_loc: case OP_put_loc_check:
        qjs_arm64_write(e, local, qjs_arm64_read(e, top - 1, 16));
        break;
    case OP_dup:
        qjs_arm64_write(e, top, qjs_arm64_read(e, top - 1, 16));
        break;
    case OP_swap:
        src = qjs_arm64_read(e, top - 1, 16);
        qjs_arm64_move(e, 3, src);
        qjs_arm64_write(e, top - 1, qjs_arm64_read(e, top - 2, 17));
        qjs_arm64_write(e, top - 2, 3);
        break;
    case OP_inc_loc: case OP_dec_loc:
        qjs_arm64_step(e, insn->op, local, local);
        break;
    case OP_inc: case OP_dec:
        qjs_arm64_step(e, insn->op, top - 1, top - 1);
        break;
    case OP_post_inc: case OP_post_dec:
        qjs_arm64_step(e, insn->op, top, top - 1);
        break;
    case OP_add_loc:
        qjs_arm64_arithmetic(e, insn->op, local, local, top - 1);
        break;
    case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
    case OP_and: case OP_xor: case OP_or: case OP_shl: case OP_sar: case OP_shr:
        qjs_arm64_arithmetic(e, insn->op, top - 2, top - 2, top - 1);
        break;
    case OP_neg: case OP_not:
        src = qjs_arm64_read(e, top - 1, 16);
        dst = qjs_arm64_slot_reg(e, top - 1, 3);
        if (insn->op == OP_neg) {
            if (e->precise_insn_plus_one)
                dst = 3;
            qjs_arm64_jump(e, 0x34000000 | src, QJS_ARM64_REPLAY); /* cbz (negative zero) */
            qjs_arm64_emit(e, qjs_arm64_rrr(0x6b000000, dst, 31, src)); /* negs */
            qjs_arm64_bcond(e, QJS_ARM64_VS, QJS_ARM64_REPLAY);
        } else {
            qjs_arm64_emit(e, qjs_arm64_rrr(0x2a200000, dst, 31, src)); /* mvn */
        }
        qjs_arm64_write(e, top - 1, dst);
        break;
    case OP_if_true: case OP_if_false:
        src = qjs_arm64_read(e, top - 1, 16);
        qjs_arm64_emit(e, 0x7100001f | (src << 5)); /* cmp wSrc, #0 */
        qjs_arm64_branch(e, index, insn->target,
                         insn->op == OP_if_true ? QJS_ARM64_NE : QJS_ARM64_EQ);
        break;
    case OP_goto:
        qjs_arm64_branch(e, index, insn->target, -1);
        break;
    case OP_return:
        src = qjs_arm64_read(e, top - 1, 16);
        qjs_arm64_emit(e, 0xb9000020 | src); /* str wSrc, [x1] */
        qjs_arm64_epilogue(e, 1, frame_size);
        break;
    case OP_plus: case OP_drop: case OP_nop:
    case OP_set_loc_uninitialized:
        /* Reads and checked stores require a dominating integer assignment. */
        break;
    default:
        abort();
    }
}

static void qjs_arm64_count_loop_args(const QJSArm64Insn *insns, int count, int16_t *loop_boundaries,
                                       int invariant, uint16_t *uses)
{
    memset(loop_boundaries, 0, (count + 1) * sizeof(*loop_boundaries));
    for (int i = 0; i < count; i++) {
        int target = insns[i].target;
        if (insns[i].depth >= 0 && target >= 0 && target <= i) {
            loop_boundaries[target]++;
            loop_boundaries[i + 1]--;
        }
    }
    int loops = 0;
    for (int i = 0; i < count; i++) {
        loops += loop_boundaries[i];
        if (loops && insns[i].depth >= 0 && insns[i].op == OP_get_arg &&
            (invariant < 0 || (i != invariant && i != invariant + 1)))
            uses[insns[i].operand]++;
    }
}

static int qjs_arm64_take_hot_arg(uint16_t *uses)
{
    int most_used = 0;
    for (int arg = 1; arg < QJS_JIT_MAX_ARGS; arg++) {
        if (uses[arg] > uses[most_used])
            most_used = arg;
    }
    if (!uses[most_used])
        return -1;
    uses[most_used] = 0;
    return most_used;
}

/* Arguments cannot be written in this subset. Reserve unused slot registers
 * for arguments referenced inside a loop, prioritizing the most used ones. */
static void qjs_arm64_assign_arg_regs(QJSArm64Emitter *e, const QJSArm64Insn *insns, int count,
                                      int16_t *loop_boundaries, int invariant, int reserved_reg,
                                      int induction_reg)
{
    uint16_t uses[QJS_JIT_MAX_ARGS] = {0};
    qjs_arm64_count_loop_args(insns, count, loop_boundaries, invariant, uses);
    for (int reg = 4; reg < 16; reg++) {
        if (reg == reserved_reg || reg == induction_reg || reg < 4 + e->stack_regs ||
            (reg >= 9 && reg < 9 + e->local_regs))
            continue;
        int arg = qjs_arm64_take_hot_arg(uses);
        if (arg < 0)
            break;
        e->arg_regs[arg] = reg;
    }
}

static unsigned qjs_arm64_emit_prologue(QJSArm64Emitter *e, int stack_size)
{
    unsigned slots = e->var_count - e->local_regs + stack_size - e->stack_regs;
    unsigned frame_size = (slots * sizeof(int32_t) + 15) & ~15u;
    if (frame_size)
        qjs_arm64_emit(e, 0xd10003ff | (frame_size << 10)); /* sub sp, sp, #frame_size */
    for (int arg = 0; arg < QJS_JIT_MAX_ARGS; arg++) {
        if (e->arg_regs[arg])
            qjs_arm64_emit(e, 0xb9400000 | (arg << 10) | e->arg_regs[arg]); /* ldr wArg, [x0, #arg] */
    }
    return frame_size;
}

#include "qjs-jit-cfg.h"
#include "qjs-jit-ssa.h"

static void qjs_arm64_emit_invariant(QJSArm64Emitter *e, const QJSArm64Insn *insns,
                                     const QJSJitCFGInvariant *invariant)
{
    if (invariant->start < 0)
        return;

    int start = invariant->start;
    int left = e->arg_regs[insns[start].operand];
    if (!left) {
        left = 16;
        qjs_arm64_emit(e, 0xb9400010 | (insns[start].operand << 10));
    }
    int right = e->arg_regs[insns[start + 1].operand];
    if (!right) {
        right = 17;
        qjs_arm64_emit(e, 0xb9400011 | (insns[start + 1].operand << 10));
    }

    uint32_t opcode;
    int op = insns[start + 2].op;
    switch (op) {
    case OP_add: opcode = 0x2b000000; break;
    case OP_sub: opcode = 0x6b000000; break;
    case OP_and: opcode = 0x0a000000; break;
    case OP_or: opcode = 0x2a000000; break;
    case OP_xor: opcode = 0x4a000000; break;
    default: abort();
    }
    qjs_arm64_emit(e, qjs_arm64_rrr(opcode, invariant->reg, left, right));
    if (op == OP_add || op == OP_sub) {
        /* The loop may not run. Replay from the current entry instead of
           materializing a body PC or locals that were never reached. */
        assert(!e->precise_insn_plus_one);
        qjs_arm64_bcond(e, QJS_ARM64_VS, QJS_ARM64_REPLAY);
    }
}

static void qjs_arm64_emit_induction_init(QJSArm64Emitter *e, const QJSJitCFGInduction *induction)
{
    int local = qjs_arm64_read(e, induction->local, 16);
    qjs_arm64_immediate(e, 17, induction->factor);
    qjs_arm64_emit(e, qjs_arm64_rrr(0x9b207c00, induction->reg, local, 17)); /* smull */
}

static void qjs_arm64_emit_induction_use(QJSArm64Emitter *e, const QJSArm64Insn *insn,
                                         const QJSJitCFGInduction *induction)
{
    int reg = induction->reg;
    /* The recurrence is wider than int32. Check at the original expression
       rather than rejecting at the preceding step, since the loop may exit. */
    qjs_arm64_emit(e, qjs_arm64_rrr(0xeb20c000, 31, reg, reg)); /* cmp xReg, wReg, sxtw */
    qjs_arm64_bcond(e, QJS_ARM64_NE, QJS_ARM64_REPLAY);
    int slot = e->var_count + insn->depth;
    if (insn->op == OP_get_loc0_loc1)
        qjs_arm64_write(e, slot++, qjs_arm64_read(e, 0, 16));
    qjs_arm64_write(e, slot, reg);
}

static void qjs_arm64_emit_induction_step(QJSArm64Emitter *e, const QJSJitCFGInduction *induction)
{
    int reg = induction->reg;
    qjs_arm64_emit(e, 0x91000000 | (induction->factor << 10) | (reg << 5) | reg); /* add xReg */
}

static int qjs_arm64_osr_header(const QJSArm64Insn *insns, int count)
{
    for (int i = 0; i < count; i++) {
        int target = insns[i].target;
        if (insns[i].depth >= 0 && target >= 0 && target <= i && insns[target].depth == 0)
            return target;
    }
    return -1;
}

static void qjs_arm64_emit_osr(QJSArm64Emitter *e, const QJSArm64Insn *insns,
                               int target, int stack_size, QJSJitOSR *osr,
                               const QJSJitCFGInvariant *invariant,
                               const QJSJitCFGInduction *induction, int induction_header)
{
    if (target < 0)
        return;

    uint32_t initialized = insns[target].initialized;
    *osr = (QJSJitOSR) { .pc = insns[target].pc, .word_offset = e->count,
                        .locals_mask = initialized };
    qjs_arm64_emit_prologue(e, stack_size);
    qjs_arm64_emit_invariant(e, insns, invariant);
    /* x3 points to validated int32 snapshots of the interpreter locals. */
    for (int local = 0; local < e->var_count; local++) {
        if (initialized & (UINT32_C(1) << local)) {
            int dst = qjs_arm64_slot_reg(e, local, 16);
            qjs_arm64_emit(e, 0xb9400060 | (local << 10) | dst); /* ldr wDst, [x3, #local] */
            qjs_arm64_write(e, local, dst);
        }
    }
    if (target == induction_header)
        qjs_arm64_emit_induction_init(e, induction);
    qjs_arm64_jump(e, 0x14000000, target);
}

static void qjs_arm64_store_exit_word(QJSArm64Emitter *e, size_t offset, int reg)
{
    qjs_arm64_emit(e, 0xb9000020 | ((offset / sizeof(int32_t)) << 10) | reg);
}

static void qjs_arm64_emit_precise_exits(QJSArm64Emitter *e, const QJSArm64Insn *insns,
                                        unsigned frame_size)
{
    for (unsigned i = 0; i < e->exit_count; i++) {
        QJSArm64ExitSite site = e->exit_sites[i];
        const QJSArm64Insn *insn = &insns[site.insn];
        e->offsets[QJS_ARM64_EXIT_BASE + i] = e->count;
        qjs_arm64_immediate(e, 16, insn->pc);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitExit, pc), 16);
        qjs_arm64_immediate(e, 16, insn->depth);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitExit, depth), 16);
        qjs_arm64_immediate(e, 16, insn->initialized);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitExit, initialized), 16);
        qjs_arm64_immediate(e, 16, insn->uninitialized);
        qjs_arm64_store_exit_word(e, offsetof(QJSJitExit, uninitialized), 16);
        for (uint32_t mask = insn->initialized; mask; mask &= mask - 1) {
            unsigned j = __builtin_ctz(mask);
            qjs_arm64_store_exit_word(e, offsetof(QJSJitExit, locals) + j * sizeof(int32_t),
                                      qjs_arm64_read(e, j, 16));
        }
        for (int j = 0; j < insn->depth; j++)
            qjs_arm64_store_exit_word(e, offsetof(QJSJitExit, stack) + j * sizeof(int32_t),
                                      qjs_arm64_read(e, e->var_count + j, 16));
        qjs_arm64_epilogue(e, site.status, frame_size);
    }
}

static size_t qjs_arm64_compile(JSRuntime *rt, QJSArm64Workspace *w, const JSFunctionBytecode *b,
                               bool optimized, uint32_t *code, size_t code_words,
                               QJSJitOSR *osr)
{
    QJSArm64Insn *insns = w->insns;
    int count = qjs_arm64_decode(b, insns, w->scratch.indices, false);
    if (!count || !qjs_arm64_verify(insns, count, b->stack_size, w->scratch.queue))
        return 0;

    QJSArm64Emitter *e = &w->emitter;
    e->code = code;
    e->capacity = code_words;
    e->var_count = b->var_count;
    e->int_insns = insns;
    e->all_locals = (UINT64_C(1) << b->var_count) - 1;
    e->local_regs = optimized ? min_int(b->var_count, QJS_ARM64_LOCAL_REGS) : 0;
    e->stack_regs = optimized ? min_int(b->stack_size, QJS_ARM64_STACK_REGS) : 0;
    int osr_header = qjs_arm64_osr_header(insns, count);
    QJSJitCFGPlan *plans = optimized ?
        qjs_cfg_plan(rt, insns, count, b->var_count, osr_header, w->scratch.queue) : NULL;
    unsigned ssa_phis = 0, ssa_stores_elided = 0, ssa_zero_folded = 0;
    uint8_t *ssa = optimized ?
        qjs_ssa_plan(rt, insns, count, b->var_count, osr_header, QJS_SSA_INT32, &ssa_phis) : NULL;
    QJSJitCFGLoop loop = optimized ? qjs_cfg_find_simple_loop(insns, count, plans) : (QJSJitCFGLoop){0};
    QJSJitCFGInvariant invariant = qjs_cfg_invariant(insns, &loop, e);
    QJSJitCFGInduction induction = qjs_cfg_induction(insns, count, &loop, plans, e, invariant.reg);
    int induction_header = induction.start >= 0 ? loop.header : -1;
    if (optimized)
        qjs_arm64_assign_arg_regs(e, insns, count, w->scratch.loop_boundaries, invariant.start,
                                  invariant.reg, induction.reg);
    QJSJitCFGLayout layout = {.condition = -1};
    if (optimized && !loop.length && b->jit_feedback)
        layout = qjs_cfg_layout(b, insns, count, plans);
    unsigned cfg_branches = 0, cfg_ops = 0;
    unsigned frame_size = qjs_arm64_emit_prologue(e, b->stack_size);
    qjs_arm64_emit_invariant(e, insns, &invariant);
    for (int position = 0; position < count + loop.length; position++) {
        int i = qjs_cfg_native_index(&layout, &loop, position);
        bool clone = qjs_cfg_clone_position(&loop, position);
        if (!clone) {
            if (i == induction_header)
                qjs_arm64_emit_induction_init(e, &induction);
            e->offsets[i] = e->count;
        }
        e->unroll_latch_plus_one = loop.length && !clone ? loop.latch + 1 : 0;
        if (insns[i].depth < 0)
            continue;
        if (plans) {
            switch (plans[i].action) {
            case QJS_CFG_DEAD:
                continue;
            case QJS_CFG_FALLTHROUGH:
                cfg_branches++;
                continue;
            case QJS_CFG_JUMP:
                qjs_arm64_branch(e, i, insns[i].target, -1);
                cfg_branches++;
                continue;
            case QJS_CFG_CONSTANT: {
                int dst = qjs_arm64_slot_reg(e, plans[i].slot, 16);
                qjs_arm64_immediate(e, dst, plans[i].value);
                qjs_arm64_write(e, plans[i].slot, dst);
                cfg_ops++;
                continue;
            }
            case QJS_CFG_KEEP:
                break;
            }
        }
        if (ssa && ssa[i] == QJS_SSA_ELIDE_STORE) {
            ssa_stores_elided++;
            continue;
        }
        if (ssa && ssa[i] == QJS_SSA_ZERO) {
            qjs_arm64_write(e, e->var_count + insns[i].depth - 2, 31);
            ssa_zero_folded++;
            continue;
        }
        if (i == invariant.start) {
            qjs_arm64_write(e, e->var_count + insns[i].depth, invariant.reg);
            position += 2;
            continue;
        }
        /* Only materialize locals whose tags are known on every incoming
           edge. A fused literal resumes before the literal was pushed. */
        e->precise_insn_plus_one = qjs_arm64_locals_known(e, &insns[i]) &&
                                  e->exit_count < QJS_ARM64_MAX_PRECISE_EXITS ? i + 1 : 0;
        if (i == induction.start) {
            qjs_arm64_emit_induction_use(e, &insns[i], &induction);
            position += 2;
            continue;
        }
        if (i == layout.condition) {
            QJSArm64Insn branch = qjs_cfg_layout_branch(&insns[i], i + 1);
            qjs_arm64_lower(e, &branch, i, optimized, frame_size);
            continue;
        }
        /* A branch may enter at the literal, but never bypass it and enter
           the consumer of the fused pair with a different stack value. */
        if (optimized && insns[i].op == OP_push_i32 && i + 1 < count &&
            i + 1 != layout.condition && qjs_cfg_native_index(&layout, &loop, position + 1) == i + 1 &&
            !insns[i + 1].branch_target &&
            (!plans || plans[i + 1].action == QJS_CFG_KEEP) &&
            (!ssa || ssa[i + 1] == QJS_SSA_KEEP) &&
            qjs_arm64_lower_constant(e, insns[i].operand, &insns[i + 1], i + 1)) {
            position++;
            continue;
        }
        qjs_arm64_lower(e, &insns[i], i, optimized, frame_size);
        if (induction.start >= 0 && i == induction.step)
            qjs_arm64_emit_induction_step(e, &induction);
    }
    e->offsets[QJS_ARM64_REPLAY] = e->count;
    qjs_arm64_epilogue(e, 0, frame_size);
    e->offsets[QJS_ARM64_FUEL] = e->count;
    qjs_arm64_epilogue(e, 2, frame_size);
    e->precise_insn_plus_one = 0;
    qjs_arm64_emit_precise_exits(e, insns, frame_size);
    qjs_arm64_emit_osr(e, insns, osr_header, b->stack_size, osr, &invariant,
                       &induction, induction_header);
    size_t words = qjs_arm64_finish(e);
    if (words) {
        rt->jit_cfg_branches_folded += cfg_branches;
        rt->jit_cfg_ops_folded += cfg_ops;
        rt->jit_cfg_hot_layouts += layout.condition >= 0;
        rt->jit_cfg_loops_unrolled += loop.length != 0;
        rt->jit_cfg_invariants_hoisted += invariant.start >= 0;
        rt->jit_cfg_inductions_reduced += induction.start >= 0;
        rt->jit_ssa_phis += ssa_phis;
        rt->jit_ssa_stores_elided += ssa_stores_elided;
        rt->jit_ssa_zero_folded += ssa_zero_folded;
    }
    js_free_rt(rt, ssa);
    js_free_rt(rt, plans);
    return words;
}
