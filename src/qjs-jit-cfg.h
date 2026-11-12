/* Included by the integer compiler after decoding and verification. */
typedef enum {
    QJS_CFG_KEEP,
    QJS_CFG_DEAD,
    QJS_CFG_FALLTHROUGH,
    QJS_CFG_JUMP,
    QJS_CFG_CONSTANT,
} QJSJitCFGAction;

typedef struct {
    int32_t value;
    uint8_t slot;
    QJSJitCFGAction action;
} QJSJitCFGPlan;

typedef struct {
    uint64_t known;
    int32_t values[QJS_JIT_MAX_LOCALS + QJS_JIT_MAX_STACK];
} QJSJitCFGFacts;

_Static_assert(QJS_JIT_MAX_LOCALS + QJS_JIT_MAX_STACK <= 64, "CFG constants must fit a slot mask");

typedef struct {
    QJSJitCFGFacts facts;
    bool reached, queued;
} QJSJitCFGState;

static bool qjs_cfg_known(const QJSJitCFGFacts *facts, int slot)
{
    return (facts->known >> slot) & 1;
}

static void qjs_cfg_set(QJSJitCFGFacts *facts, int slot, int32_t value)
{
    facts->known |= UINT64_C(1) << slot;
    facts->values[slot] = value;
}

static void qjs_cfg_copy(QJSJitCFGFacts *to, int dst, const QJSJitCFGFacts *from, int src)
{
    to->known &= ~(UINT64_C(1) << dst);
    if (qjs_cfg_known(from, src))
        qjs_cfg_set(to, dst, from->values[src]);
}

/* Preserve JavaScript's Number semantics at signed overflow, fractional
   quotients, and negative zero by leaving those operations to normal lowering. */
static bool qjs_cfg_binary(int op, int32_t lhs, int32_t rhs, int32_t *result)
{
    int64_t wide;
    switch (op) {
    case OP_add: case OP_add_loc: wide = (int64_t)lhs + rhs; break;
    case OP_sub: wide = (int64_t)lhs - rhs; break;
    case OP_mul:
        wide = (int64_t)lhs * rhs;
        if (!wide && (lhs < 0 || rhs < 0))
            return false;
        break;
    case OP_div:
        if (!rhs || (!lhs && rhs < 0) || (int64_t)lhs % rhs)
            return false;
        wide = (int64_t)lhs / rhs;
        break;
    case OP_mod:
        if (!rhs)
            return false;
        wide = (int64_t)lhs % rhs;
        if (!wide && lhs < 0)
            return false;
        break;
    case OP_and: wide = lhs & rhs; break;
    case OP_or: wide = lhs | rhs; break;
    case OP_xor: wide = lhs ^ rhs; break;
    case OP_shl: wide = (int32_t)((uint32_t)lhs << ((uint32_t)rhs & 31)); break;
    case OP_sar: wide = lhs >> ((uint32_t)rhs & 31); break;
    case OP_shr: wide = (uint32_t)lhs >> ((uint32_t)rhs & 31); break;
    default: return false;
    }
    if (wide < INT32_MIN || wide > INT32_MAX)
        return false;
    *result = wide;
    return true;
}

static bool qjs_cfg_evaluate_binary(const QJSJitCFGFacts *facts, int op,
                                    int lhs, int rhs, int32_t *result)
{
    return qjs_cfg_known(facts, lhs) && qjs_cfg_known(facts, rhs) &&
           qjs_cfg_binary(op, facts->values[lhs], facts->values[rhs], result);
}

static void qjs_cfg_transfer(const QJSArm64Insn *insn, int vars, const QJSJitCFGFacts *in,
                             QJSJitCFGFacts *out, QJSJitCFGPlan *plan)
{
    int top = vars + insn->depth;
    int first_popped = top - insn->pop;
    int local = insn->operand;
    int slot = -1;
    int32_t value = 0;

    *out = *in;
    if (first_popped < 64)
        out->known &= (UINT64_C(1) << first_popped) - 1;
    switch (insn->op) {
    case OP_push_i32:
        qjs_cfg_set(out, top, insn->operand);
        break;
    case OP_get_loc:
        qjs_cfg_copy(out, top, in, local);
        break;
    case OP_get_loc0_loc1:
        qjs_cfg_copy(out, top, in, 0);
        qjs_cfg_copy(out, top + 1, in, 1);
        break;
    case OP_put_loc: case OP_put_loc_check: case OP_set_loc:
        qjs_cfg_copy(out, local, in, top - 1);
        if (insn->op == OP_set_loc)
            qjs_cfg_copy(out, top - 1, in, top - 1);
        break;
    case OP_set_loc_uninitialized:
        out->known &= ~(UINT64_C(1) << local);
        break;
    case OP_dup:
        qjs_cfg_copy(out, top - 1, in, top - 1);
        qjs_cfg_copy(out, top, in, top - 1);
        break;
    case OP_swap:
        qjs_cfg_copy(out, top - 2, in, top - 1);
        qjs_cfg_copy(out, top - 1, in, top - 2);
        break;
    case OP_plus:
        qjs_cfg_copy(out, top - 1, in, top - 1);
        break;
    case OP_neg: case OP_not:
        if (qjs_cfg_known(in, top - 1)) {
            int32_t operand = in->values[top - 1];
            if (insn->op == OP_not) {
                value = ~operand;
                slot = top - 1;
            } else if (operand && operand != INT32_MIN) {
                value = -operand;
                slot = top - 1;
            }
        }
        break;
    case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
    case OP_and: case OP_or: case OP_xor: case OP_shl: case OP_sar: case OP_shr:
        if (qjs_cfg_evaluate_binary(in, insn->op, top - 2, top - 1, &value))
            slot = top - 2;
        break;
    case OP_add_loc:
        out->known &= ~(UINT64_C(1) << local);
        if (qjs_cfg_evaluate_binary(in, insn->op, local, top - 1, &value))
            slot = local;
        break;
    case OP_inc_loc: case OP_dec_loc:
    case OP_inc: case OP_dec: case OP_post_inc: case OP_post_dec: {
        bool local_step = insn->op == OP_inc_loc || insn->op == OP_dec_loc;
        bool post = insn->op == OP_post_inc || insn->op == OP_post_dec;
        bool decrement = insn->op == OP_dec_loc || insn->op == OP_dec || insn->op == OP_post_dec;
        int src = local_step ? local : top - 1;
        if (local_step)
            out->known &= ~(UINT64_C(1) << src);
        if (post)
            qjs_cfg_copy(out, src, in, src);
        if (qjs_cfg_known(in, src) && qjs_cfg_binary(OP_add, in->values[src], decrement ? -1 : 1, &value))
            slot = post ? top : src;
        break;
    }
    default:
        break;
    }
    if (slot >= 0) {
        qjs_cfg_set(out, slot, value);
        if (plan)
            *plan = (QJSJitCFGPlan){.action = QJS_CFG_CONSTANT, .slot = slot, .value = value};
    }
}

static QJSJitCFGAction qjs_cfg_branch(const QJSArm64Insn *insn, int top,
                                     const QJSJitCFGFacts *facts)
{
    bool taken;
    if (insn->op == OP_if_true || insn->op == OP_if_false) {
        if (!qjs_cfg_known(facts, top - 1))
            return QJS_CFG_KEEP;
        taken = (facts->values[top - 1] != 0) == (insn->op == OP_if_true);
    } else if (qjs_arm64_is_comparison(insn->op)) {
        if (!qjs_cfg_known(facts, top - 2) || !qjs_cfg_known(facts, top - 1))
            return QJS_CFG_KEEP;
        int32_t lhs = facts->values[top - 2];
        int32_t rhs = facts->values[top - 1];
        switch (insn->op) {
        case OP_lt: taken = lhs < rhs; break;
        case OP_lte: taken = lhs <= rhs; break;
        case OP_gt: taken = lhs > rhs; break;
        case OP_gte: taken = lhs >= rhs; break;
        case OP_eq: case OP_strict_eq: taken = lhs == rhs; break;
        default: taken = lhs != rhs; break;
        }
        taken = taken == insn->branch_if_true;
    } else {
        return QJS_CFG_KEEP;
    }
    return taken ? QJS_CFG_JUMP : QJS_CFG_FALLTHROUGH;
}

static void qjs_cfg_merge(QJSJitCFGState *states, int next, const QJSJitCFGFacts *incoming,
                          uint16_t *queue, unsigned *tail)
{
    QJSJitCFGState *state = &states[next];
    if (!state->reached) {
        state->facts = *incoming;
        state->reached = true;
    } else {
        uint64_t common = state->facts.known & incoming->known;
        for (uint64_t bits = common; bits; bits &= bits - 1) {
            unsigned slot = __builtin_ctzll(bits);
            if (state->facts.values[slot] != incoming->values[slot])
                common &= ~(UINT64_C(1) << slot);
        }
        if (state->facts.known == common)
            return;
        state->facts.known = common;
    }
    if (!state->queued) {
        state->queued = true;
        queue[(*tail)++ % QJS_ARM64_MAX_BYTECODE] = next;
    }
}

static QJSJitCFGPlan *qjs_cfg_plan(JSRuntime *rt, const QJSArm64Insn *insns, int count,
                                  int vars, int osr_header, uint16_t *queue)
{
    QJSJitCFGState *states = js_mallocz_rt(rt, count * sizeof(*states));
    if (!states)
        return NULL;

    QJSJitCFGFacts unknown = {0};
    unsigned head = 0, tail = 0;
    qjs_cfg_merge(states, 0, &unknown, queue, &tail);
    /* The OSR prologue loads the interpreter's current locals. It does not
       execute the initializer or any predecessor in this native body. */
    if (osr_header >= 0)
        qjs_cfg_merge(states, osr_header, &unknown, queue, &tail);

    QJSJitCFGFacts out;
    unsigned budget = 16 * count;
    while (head != tail && budget--) {
        int i = queue[head++ % QJS_ARM64_MAX_BYTECODE];
        const QJSArm64Insn *insn = &insns[i];
        QJSJitCFGState *state = &states[i];
        state->queued = false;
        if (insn->op == OP_return)
            continue;

        qjs_cfg_transfer(insn, vars, &state->facts, &out, NULL);
        QJSJitCFGAction branch = qjs_cfg_branch(insn, vars + insn->depth, &state->facts);
        if (insn->target >= 0 && branch != QJS_CFG_FALLTHROUGH)
            qjs_cfg_merge(states, insn->target, &out, queue, &tail);
        if (insn->op != OP_goto && branch != QJS_CFG_JUMP)
            qjs_cfg_merge(states, i + 1, &out, queue, &tail);
    }

    QJSJitCFGPlan *plans = NULL;
    if (head == tail) {
        plans = js_mallocz_rt(rt, count * sizeof(*plans));
        if (plans) {
            for (int i = 0; i < count; i++) {
                if (!states[i].reached) {
                    plans[i].action = QJS_CFG_DEAD;
                    continue;
                }
                plans[i].action = qjs_cfg_branch(&insns[i], vars + insns[i].depth, &states[i].facts);
                if (plans[i].action == QJS_CFG_KEEP)
                    qjs_cfg_transfer(&insns[i], vars, &states[i].facts, &out, &plans[i]);
            }
        }
    }
    js_free_rt(rt, states);
    return plans;
}

typedef struct {
    int condition, hot_start, hot_end;
} QJSJitCFGLayout;

static QJSJitCFGLayout qjs_cfg_layout(const JSFunctionBytecode *b, const QJSArm64Insn *insns,
                                      int count, const QJSJitCFGPlan *plans)
{
    QJSJitCFGLayout layout = {.condition = -1};
    uint32_t hottest = 0;

    for (int i = 0; i < count; i++) {
        const QJSArm64Insn *insn = &insns[i];
        int target = insn->target;
        bool comparison = qjs_arm64_is_comparison(insn->op);
        if (insn->depth < 0 || target <= i + 1 ||
            (plans && plans[i].action != QJS_CFG_KEEP) ||
            (!comparison && insn->op != OP_if_true && insn->op != OP_if_false))
            continue;
        if (insns[target - 1].op != OP_return && insns[target - 1].op != OP_goto)
            continue;

        uint32_t branch_pc = insn->pc;
        if (comparison)
            branch_pc += short_opcode_info(insn->op).size;
        const QJSJitFeedbackSite *site = qjs_jit_feedback_lookup(b, branch_pc, QJS_JIT_FEEDBACK_BRANCH);
        if (!site || site->u.branch.taken < 16 || site->u.branch.taken <= hottest ||
            site->u.branch.taken < UINT64_C(4) * site->u.branch.fallthrough)
            continue;

        /* Moving a single-entry return block adds no repair branches. Its
           original predecessor cannot fall through, and neither can the block. */
        for (int j = target; j < count && j < target + 8; j++) {
            if (insns[j].depth < 0 || insns[j].target >= 0 || (j > target && insns[j].branch_target))
                break;
            if (insns[j].op == OP_return) {
                layout = (QJSJitCFGLayout){.condition = i, .hot_start = target, .hot_end = j};
                hottest = site->u.branch.taken;
                break;
            }
        }
    }
    return layout;
}

static int qjs_cfg_layout_index(const QJSJitCFGLayout *layout, int position)
{
    if (layout->condition < 0 || position <= layout->condition || position > layout->hot_end)
        return position;
    int length = layout->hot_end - layout->hot_start + 1;
    if (position <= layout->condition + length)
        return layout->hot_start + position - layout->condition - 1;
    return position - length;
}

static QJSArm64Insn qjs_cfg_layout_branch(const QJSArm64Insn *insn, int fallthrough)
{
    QJSArm64Insn branch = *insn;
    branch.target = fallthrough;
    if (qjs_arm64_is_comparison(branch.op))
        branch.branch_if_true = !branch.branch_if_true;
    else
        branch.op = branch.op == OP_if_true ? OP_if_false : OP_if_true;
    return branch;
}

typedef struct {
    int header, latch, length;
} QJSJitCFGLoop;

static QJSJitCFGLoop qjs_cfg_find_simple_loop(const QJSArm64Insn *insns, int count,
                                              const QJSJitCFGPlan *plans)
{
    for (int latch = 0; latch < count; latch++) {
        const QJSArm64Insn *last = &insns[latch];
        int header = last->target;
        if (last->depth < 0 || header < 0 || header >= latch || latch - header >= 24 ||
            insns[header].depth != 0 || (plans && plans[latch].action != QJS_CFG_KEEP))
            continue;

        /* Only clone single-entry loops with one exit. All references from
           either copy still name the original bytecode indices. */
        int exits = last->op == OP_goto ? 0 : 1;
        int i;
        for (i = header; i < latch; i++) {
            const QJSArm64Insn *insn = &insns[i];
            if (insn->depth < 0 || insn->op == OP_return || (i > header && insn->branch_target))
                break;
            if (insn->target >= 0) {
                if (insn->target <= latch || insn->op == OP_goto || ++exits > 1)
                    break;
            }
        }
        if (i == latch && exits == 1 && !last->branch_target)
            return (QJSJitCFGLoop){.header = header, .latch = latch, .length = latch - header + 1};
    }
    return (QJSJitCFGLoop){0};
}

static bool qjs_cfg_clone_position(const QJSJitCFGLoop *loop, int position)
{
    return position > loop->latch && position <= loop->latch + loop->length;
}

static int qjs_cfg_native_index(const QJSJitCFGLayout *layout, const QJSJitCFGLoop *loop,
                                int position)
{
    if (!loop->length)
        return qjs_cfg_layout_index(layout, position);
    if (position <= loop->latch)
        return position;
    if (qjs_cfg_clone_position(loop, position))
        return loop->header + position - loop->latch - 1;
    return position - loop->length;
}

typedef struct {
    int start, reg;
} QJSJitCFGInvariant;

static int qjs_cfg_spare_register(const QJSArm64Emitter *e, int reserved)
{
    for (int reg = 15; reg >= 4; reg--) {
        if (reg != reserved && reg >= 4 + e->stack_regs &&
            (reg < 9 || reg >= 9 + e->local_regs))
            return reg;
    }
    return -1;
}

static QJSJitCFGInvariant qjs_cfg_invariant(const QJSArm64Insn *insns, const QJSJitCFGLoop *loop,
                                            const QJSArm64Emitter *e)
{
    QJSJitCFGInvariant invariant = {.start = -1};
    if (!loop->length)
        return invariant;

    int reg = qjs_cfg_spare_register(e, -1);
    if (reg < 4)
        return invariant;

    for (int i = loop->header; i + 2 < loop->latch; i++) {
        if (insns[i].op != OP_get_arg || insns[i + 1].op != OP_get_arg ||
            insns[i + 1].branch_target || insns[i + 2].branch_target)
            continue;
        switch (insns[i + 2].op) {
        case OP_add: case OP_sub: case OP_and: case OP_or: case OP_xor:
            return (QJSJitCFGInvariant){.start = i, .reg = reg};
        default:
            break;
        }
    }
    return invariant;
}

typedef struct {
    int start, step, local, factor, reg;
} QJSJitCFGInduction;

static bool qjs_cfg_writes_local(const QJSArm64Insn *insn, int local)
{
    if (insn->operand != local)
        return false;
    switch (insn->op) {
    case OP_put_loc: case OP_put_loc_check: case OP_set_loc: case OP_set_loc_uninitialized:
    case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
        return true;
    default:
        return false;
    }
}

static QJSJitCFGInduction qjs_cfg_induction(const QJSArm64Insn *insns, int count,
                                            const QJSJitCFGLoop *loop, const QJSJitCFGPlan *plans,
                                            const QJSArm64Emitter *e, int reserved)
{
    QJSJitCFGInduction induction = {.start = -1, .reg = -1};
    if (loop->length < 7 || !loop->header || insns[loop->latch].op != OP_goto)
        return induction;

    int header = loop->header;
    const QJSArm64Insn *test = &insns[header + 2];
    const QJSArm64Insn *preheader = &insns[header - 1];
    if (insns[header].op != OP_get_loc || insns[header + 1].op != OP_get_arg ||
        (test->op != OP_lt && test->op != OP_lte) || test->branch_if_true ||
        test->target <= loop->latch || preheader->depth < 0 ||
        preheader->op == OP_goto || preheader->op == OP_return ||
        (plans && (plans[header - 1].action == QJS_CFG_DEAD ||
                   plans[header - 1].action == QJS_CFG_JUMP)))
        return induction;

    /* Only the latch and the synthesized OSR entry may jump to the
       header, so every other arrival executes the native initializer. */
    for (int i = 0; i < count; i++) {
        if (i != loop->latch && insns[i].target == header)
            return induction;
    }

    int local = insns[header].operand;
    if (!(insns[header].initialized & (UINT32_C(1) << local)))
        return induction;
    int step = -1;
    for (int i = header + 3; i < loop->latch; i++) {
        if (qjs_cfg_writes_local(&insns[i], local)) {
            if (step >= 0 || insns[i].op != OP_inc_loc ||
                (plans && plans[i].action != QJS_CFG_KEEP))
                return induction;
            step = i;
        }
    }
    int reg = qjs_cfg_spare_register(e, reserved);
    if (step < 0 || reg < 0)
        return induction;

    for (int i = header + 3; i + 2 < step; i++) {
        int factor = insns[i + 1].operand;
        bool get_induction = (insns[i].op == OP_get_loc && insns[i].operand == local) ||
                             (insns[i].op == OP_get_loc0_loc1 && local == 1);
        if (get_induction &&
            insns[i + 1].op == OP_push_i32 && factor >= 2 && factor <= 4095 &&
            insns[i + 2].op == OP_mul && !insns[i + 1].branch_target && !insns[i + 2].branch_target &&
            (!plans || (plans[i].action == QJS_CFG_KEEP &&
                        plans[i + 1].action == QJS_CFG_KEEP &&
                        plans[i + 2].action == QJS_CFG_KEEP)))
            return (QJSJitCFGInduction){.start = i, .step = step, .local = local, .factor = factor, .reg = reg};
    }
    return induction;
}
