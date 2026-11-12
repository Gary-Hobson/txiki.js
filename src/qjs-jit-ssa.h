/* Typed value versions for the verified scalar stack and locals. Copies carry
   the source version. An ordinary entry and an OSR entry supply separate roots. */
enum {
    QJS_SSA_MAX_BLOCKS = 96,
    QJS_SSA_SLOTS = QJS_JIT_MAX_LOCALS + QJS_JIT_MAX_STACK,
    QJS_SSA_ARG_BASE = 3,
    QJS_SSA_OSR_BASE = QJS_SSA_ARG_BASE + QJS_JIT_MAX_ARGS,
    QJS_SSA_DEF_BASE = QJS_SSA_OSR_BASE + QJS_JIT_MAX_LOCALS,
    QJS_SSA_PHI_BASE = QJS_SSA_DEF_BASE + QJS_ARM64_MAX_BYTECODE,
};

typedef uint32_t QJSJitSSAValue;

enum {
    QJS_SSA_UNDEFINED = 1,
    QJS_SSA_TDZ = 2,
    QJS_SSA_INT32 = UINT32_C(1) << 16,
    QJS_SSA_FLOAT64 = UINT32_C(2) << 16,
    QJS_SSA_MIXED = UINT32_C(3) << 16,
};

_Static_assert(QJS_SSA_PHI_BASE + QJS_SSA_MAX_BLOCKS * QJS_SSA_SLOTS <= UINT16_MAX,
               "SSA version IDs must fit the low 16 bits");

typedef enum {
    QJS_SSA_KEEP,
    QJS_SSA_ELIDE_STORE,
    QJS_SSA_ZERO,
} QJSJitSSAAction;

typedef struct {
    QJSJitSSAValue slots[QJS_SSA_SLOTS];
} QJSJitSSAFrame;

typedef struct {
    uint16_t start, end;
    int16_t incoming, successors[2];
    bool reached, queued;
    QJSJitSSAFrame out;
} QJSJitSSABlock;

typedef struct {
    uint16_t block;
    int16_t next;
} QJSJitSSAEdge;

typedef struct {
    bool leaders[QJS_ARM64_MAX_BYTECODE];
    uint16_t block_at[QJS_ARM64_MAX_BYTECODE];
    uint16_t queue[QJS_SSA_MAX_BLOCKS];
    QJSJitSSABlock blocks[QJS_SSA_MAX_BLOCKS];
    QJSJitSSAEdge edges[2 * QJS_SSA_MAX_BLOCKS];
    QJSJitSSAFrame frame, osr;
} QJSJitSSAWorkspace;

static QJSJitSSAValue qjs_ssa_type(QJSJitSSAValue value)
{
    return value & UINT32_C(0xffff0000);
}

static QJSJitSSAAction qjs_ssa_transfer(QJSJitSSAFrame *frame, const QJSArm64Insn *insn,
                                         int index, int vars, QJSJitSSAValue number_type)
{
    QJSJitSSAValue *slots = frame->slots;
    int top = vars + insn->depth;
    int local = insn->operand;
    QJSJitSSAValue version = number_type | (QJS_SSA_DEF_BASE + index);
    QJSJitSSAAction action = QJS_SSA_KEEP;
    switch (insn->op) {
    case OP_get_arg:
        slots[top] = number_type | (QJS_SSA_ARG_BASE + local);
        break;
    case OP_get_loc:
        slots[top] = slots[local];
        break;
    case OP_get_loc0_loc1:
        slots[top] = slots[0];
        slots[top + 1] = slots[1];
        break;
    case OP_put_loc: case OP_set_loc: case OP_put_loc_check:
        if (slots[local] == slots[top - 1] && qjs_ssa_type(slots[local]) == number_type)
            action = QJS_SSA_ELIDE_STORE;
        slots[local] = slots[top - 1];
        break;
    case OP_set_loc_uninitialized:
        slots[local] = QJS_SSA_TDZ;
        break;
    case OP_dup:
        slots[top] = slots[top - 1];
        break;
    case OP_swap: {
        QJSJitSSAValue saved = slots[top - 1];
        slots[top - 1] = slots[top - 2];
        slots[top - 2] = saved;
        break;
    }
    case OP_plus:
        break;
    case OP_add_loc: case OP_inc_loc: case OP_dec_loc:
        slots[local] = version;
        break;
    case OP_post_inc: case OP_post_dec:
        slots[top] = version;
        break;
    case OP_sub: case OP_xor:
        if (number_type == QJS_SSA_INT32 && slots[top - 2] == slots[top - 1] &&
            qjs_ssa_type(slots[top - 2]) == QJS_SSA_INT32)
            action = QJS_SSA_ZERO;
        slots[top - 2] = version;
        break;
    default:
        /* Every other admitted producer has a single scalar Number result. */
        assert(insn->push <= 1);
        if (insn->push)
            slots[top - insn->pop] = version;
        break;
    }
    return action;
}

static int qjs_ssa_blocks(QJSJitSSAWorkspace *work, const QJSArm64Insn *insns, int count,
                          int osr_header)
{
    work->leaders[0] = true;
    if (osr_header >= 0)
        work->leaders[osr_header] = true;
    for (int i = 0; i < count; i++) {
        if (insns[i].target >= 0)
            work->leaders[insns[i].target] = true;
        if (i + 1 < count && (insns[i].target >= 0 || insns[i].op == OP_return))
            work->leaders[i + 1] = true;
    }

    int blocks = 0;
    for (int i = 0; i < count; i++) {
        if (work->leaders[i]) {
            if (blocks == QJS_SSA_MAX_BLOCKS)
                return 0;
            if (blocks)
                work->blocks[blocks - 1].end = i;
            work->blocks[blocks].start = i;
            work->blocks[blocks++].incoming = -1;
        }
        work->block_at[i] = blocks - 1;
    }
    work->blocks[blocks - 1].end = count;

    int edges = 0;
    for (int block = 0; block < blocks; block++) {
        QJSJitSSABlock *b = &work->blocks[block];
        const QJSArm64Insn *last = &insns[b->end - 1];
        b->successors[0] = -1;
        b->successors[1] = -1;
        if (last->depth < 0 || last->op == OP_return)
            continue;
        if (last->target >= 0)
            b->successors[0] = work->block_at[last->target];
        if (last->op != OP_goto && b->end < count)
            b->successors[1] = work->block_at[b->end];
        for (unsigned i = 0; i < countof(b->successors); i++) {
            int successor = b->successors[i];
            if (successor < 0)
                continue;
            QJSJitSSABlock *next = &work->blocks[successor];
            work->edges[edges] = (QJSJitSSAEdge){ .block = block, .next = next->incoming };
            next->incoming = edges++;
        }
    }
    return blocks;
}

static void qjs_ssa_merge(QJSJitSSAFrame *frame, const QJSJitSSAFrame *incoming,
                           int block, int slots, bool *reached)
{
    if (!*reached) {
        memcpy(frame->slots, incoming->slots, slots * sizeof(*frame->slots));
        *reached = true;
        return;
    }
    for (int slot = 0; slot < slots; slot++) {
        QJSJitSSAValue current = frame->slots[slot];
        QJSJitSSAValue incoming_value = incoming->slots[slot];
        if (current == incoming_value)
            continue;
        QJSJitSSAValue type = qjs_ssa_type(current);
        if (!type || type != qjs_ssa_type(incoming_value))
            type = QJS_SSA_MIXED;
        frame->slots[slot] = type | (QJS_SSA_PHI_BASE + block * QJS_SSA_SLOTS + slot);
    }
}

static bool qjs_ssa_entry(QJSJitSSAWorkspace *work, const QJSArm64Insn *insns,
                           int block, int vars, int osr_header)
{
    QJSJitSSABlock *b = &work->blocks[block];
    bool reached = block == 0;
    if (reached) {
        for (int slot = 0; slot < vars; slot++)
            work->frame.slots[slot] = QJS_SSA_UNDEFINED;
    }
    if (b->start == osr_header)
        qjs_ssa_merge(&work->frame, &work->osr, block, vars, &reached);

    /* The predecessor's output at this slot is the phi operand for that edge. */
    int slots = vars + insns[b->start].depth;
    for (int edge = b->incoming; edge >= 0; edge = work->edges[edge].next) {
        QJSJitSSABlock *previous = &work->blocks[work->edges[edge].block];
        if (previous->reached)
            qjs_ssa_merge(&work->frame, &previous->out, block, slots, &reached);
    }
    return reached;
}

static void qjs_ssa_enqueue(QJSJitSSAWorkspace *work, int block, unsigned *tail)
{
    if (!work->blocks[block].queued) {
        work->blocks[block].queued = true;
        work->queue[(*tail)++ % QJS_SSA_MAX_BLOCKS] = block;
    }
}

static bool qjs_ssa_resolve(QJSJitSSAWorkspace *work, const QJSArm64Insn *insns,
                            int count, int vars, int osr_header, QJSJitSSAValue number_type)
{
    unsigned head = 0, tail = 0;
    qjs_ssa_enqueue(work, 0, &tail);
    if (osr_header >= 0) {
        const QJSArm64Insn *header = &insns[osr_header];
        for (int slot = 0; slot < vars; slot++) {
            uint32_t bit = UINT32_C(1) << slot;
            if (header->initialized & bit)
                work->osr.slots[slot] = number_type | (QJS_SSA_OSR_BASE + slot);
            else if (header->undefined & bit)
                work->osr.slots[slot] = QJS_SSA_UNDEFINED;
            else if (header->uninitialized & bit)
                work->osr.slots[slot] = QJS_SSA_TDZ;
            else
                work->osr.slots[slot] = QJS_SSA_MIXED | (QJS_SSA_OSR_BASE + slot);
        }
        qjs_ssa_enqueue(work, work->block_at[osr_header], &tail);
    }

    unsigned budget = 16 * count;
    while (head != tail) {
        int block = work->queue[head++ % QJS_SSA_MAX_BLOCKS];
        QJSJitSSABlock *b = &work->blocks[block];
        b->queued = false;
        unsigned length = b->end - b->start;
        if (length > budget)
            return false;
        budget -= length;
        if (!qjs_ssa_entry(work, insns, block, vars, osr_header))
            continue;
        for (int i = b->start; i < b->end; i++)
            qjs_ssa_transfer(&work->frame, &insns[i], i, vars, number_type);
        const QJSArm64Insn *last = &insns[b->end - 1];
        size_t bytes = (vars + last->depth - last->pop + last->push) * sizeof(*work->frame.slots);
        if (b->reached && !memcmp(b->out.slots, work->frame.slots, bytes))
            continue;
        memcpy(b->out.slots, work->frame.slots, bytes);
        b->reached = true;
        for (unsigned i = 0; i < countof(b->successors); i++) {
            if (b->successors[i] >= 0)
                qjs_ssa_enqueue(work, b->successors[i], &tail);
        }
    }
    return true;
}

static uint8_t *qjs_ssa_plan(JSRuntime *rt, const QJSArm64Insn *insns, int count,
                             int vars, int osr_header, QJSJitSSAValue number_type, unsigned *phis)
{
    int candidate = 0;
    while (candidate < count) {
        int op = insns[candidate].op;
        if (op == OP_put_loc || op == OP_set_loc || op == OP_put_loc_check ||
            (number_type == QJS_SSA_INT32 && (op == OP_sub || op == OP_xor)))
            break;
        candidate++;
    }
    if (candidate == count)
        return NULL;

    QJSJitSSAWorkspace *work = js_mallocz_rt(rt, sizeof(*work));
    if (!work)
        return NULL;
    int blocks = qjs_ssa_blocks(work, insns, count, osr_header);
    uint8_t *plan = NULL;
    if (blocks && qjs_ssa_resolve(work, insns, count, vars, osr_header, number_type))
        plan = js_mallocz_rt(rt, count * sizeof(*plan));
    if (plan) {
        for (int block = 0; block < blocks; block++) {
            QJSJitSSABlock *b = &work->blocks[block];
            if (!b->reached)
                continue;
            bool reached = qjs_ssa_entry(work, insns, block, vars, osr_header);
            assert(reached);
            (void)reached;
            int slots = vars + insns[b->start].depth;
            for (int slot = 0; slot < slots; slot++) {
                uint16_t id = work->frame.slots[slot];
                if (id == QJS_SSA_PHI_BASE + block * QJS_SSA_SLOTS + slot)
                    (*phis)++;
            }
            for (int i = b->start; i < b->end; i++)
                plan[i] = qjs_ssa_transfer(&work->frame, &insns[i], i, vars, number_type);
        }
    }
    js_free_rt(rt, work);
    return plan;
}
