"""Transactional int32 DAG regions on the retained object-layout baseline.

Private opcode 253 exists only without the inspector. Bytecode storage remains
at its original address; a site's first five canonical bytes are saved and
restored on guard failure, serialization and destruction. No partial local write.
"""


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:160])
    return s.replace(old, new, 1)


STRUCTS = r'''
#define IR_MAX_INPUTS 16
#define IR_MAX_NODES 16
#define IR_NODE_BASE 32
#define IR_MAX_WRITES 8
#define IR_MAX_STACK 8
#define IR_MAX_INSNS 32

typedef struct JSIntRegionInput {
    int32_t value;
    uint8_t kind; /* 0 constant, 1 local, 2 argument */
    uint8_t pad[3];
} JSIntRegionInput;
typedef struct JSIntRegionNode {
    uint8_t op, left, right, pad;
} JSIntRegionNode;
typedef struct JSIntRegionWrite {
    uint16_t local;
    uint8_t value, pad;
} JSIntRegionWrite;
typedef struct JSIntRegionSite {
    uint32_t pc, end, payload;
    uint8_t inputs, nodes, writes, output;
    uint8_t insns, output_kind, retired; /* output_kind: 0 none, 1 int, 2 bool */
    uint8_t original[5];
} JSIntRegionSite;
typedef struct JSIntRegionPlan {
    size_t bytes;
    uint32_t count;
    JSIntRegionSite sites[];
} JSIntRegionPlan;

#ifdef QJS_INT_REGION_DIAGNOSTICS
#define INT_REGION_STAT(rt, k, n) ((rt)->int_region_stats.k += (n))
#else
#define INT_REGION_STAT(rt, k, n) ((void)0)
#endif
'''

HELPERS = r'''
/* This builder is a compile/read-time virtual stack. Each local write creates
   a new value version; later loads use that version instead of the old input. */
typedef struct JSIntRegionBuild {
    JSIntRegionInput input[IR_MAX_INPUTS];
    JSIntRegionNode node[IR_MAX_NODES];
    JSIntRegionWrite write[IR_MAX_WRITES];
    uint8_t stack[IR_MAX_STACK];
    int inputs, nodes, writes, depth, computes, arithmetic, insns, end;
    bool comparison;
} JSIntRegionBuild;

static int js_ir_canonical_op(int op)
{
    if (op == OP_object_layout) return OP_object;
    if (op == OP_init_object_field) return OP_define_field;
    return op;
}

static int js_ir_input(JSIntRegionBuild *r, int kind, int32_t value)
{
    int i;
    if (kind == 1) {
        for (i = 0; i < r->writes; i++)
            if (r->write[i].local == value)
                return r->write[i].value;
    }
    for (i = 0; i < r->inputs; i++)
        if (r->input[i].kind == kind && r->input[i].value == value)
            return i;
    if (r->inputs == IR_MAX_INPUTS) return -1;
    r->input[r->inputs].kind = kind;
    r->input[r->inputs].value = value;
    return r->inputs++;
}

static bool js_ir_push(JSIntRegionBuild *r, int v)
{
    if (v < 0 || r->depth == IR_MAX_STACK) return false;
    r->stack[r->depth++] = v;
    return true;
}

static int js_ir_node(JSIntRegionBuild *r, int op, int a, int b)
{
    int i;
    if (a < 0 || b < 0) return -1;
    r->computes++;
    for (i = 0; i < r->nodes; i++) {
        JSIntRegionNode *n = &r->node[i];
        if (n->op == op && n->left == a && n->right == b)
            return IR_NODE_BASE + i;
    }
    if (r->nodes == IR_MAX_NODES) return -1;
    r->node[r->nodes] = (JSIntRegionNode){ op, a, b, 0 };
    return IR_NODE_BASE + r->nodes++;
}

static bool js_ir_write(JSIntRegionBuild *r, int local, int value)
{
    int i;
    if (value < 0) return false;
    for (i = 0; i < r->writes; i++) {
        if (r->write[i].local == local) {
            r->write[i].value = value;
            return true;
        }
    }
    if (r->writes == IR_MAX_WRITES) return false;
    r->write[r->writes++] = (JSIntRegionWrite){ local, value, 0 };
    return true;
}

static bool js_ir_scan(JSFunctionBytecode *b, const uint8_t *targets,
                       int start, JSIntRegionBuild *best)
{
    JSIntRegionBuild r = {0};
    const uint8_t *code = b->byte_code_buf;
    int pos = start, op, size, idx, a, c, v;
    bool keep, cut;
    memset(best, 0, sizeof(*best));
    while (pos < b->byte_code_len && r.insns < IR_MAX_INSNS) {
        if (pos != start && targets[pos]) break;
        op = js_ir_canonical_op(code[pos]);
        size = js_object_layout_ops[op].size;
        keep = false;
        cut = true;
        idx = -1;
        if (op == OP_get_loc || op == OP_get_loc8 ||
            (op >= OP_get_loc0 && op <= OP_get_loc3)) {
            idx = op == OP_get_loc ? get_u16(code + pos + 1) :
                  op == OP_get_loc8 ? code[pos + 1] : op - OP_get_loc0;
            if (idx >= b->var_count || !js_ir_push(&r, js_ir_input(&r, 1, idx))) break;
            cut = false;
        } else if (op == OP_get_arg || (op >= OP_get_arg0 && op <= OP_get_arg3)) {
            idx = op == OP_get_arg ? get_u16(code + pos + 1) : op - OP_get_arg0;
            if (idx >= b->arg_count || !js_ir_push(&r, js_ir_input(&r, 2, idx))) break;
            cut = false;
        } else if (op == OP_get_loc0_loc1) {
            if (b->var_count < 2 || !js_ir_push(&r, js_ir_input(&r, 1, 0)) ||
                !js_ir_push(&r, js_ir_input(&r, 1, 1))) break;
            cut = false;
        } else if (op == OP_push_i32 || op == OP_push_i8 ||
                   (op >= OP_push_minus1 && op <= OP_push_7)) {
            v = op == OP_push_i32 ? (int32_t)get_u32(code + pos + 1) :
                op == OP_push_i8 ? (int8_t)code[pos + 1] : op - OP_push_0;
            if (!js_ir_push(&r, js_ir_input(&r, 0, v))) break;
            cut = false;
        } else if (op == OP_dup) {
            if (!r.depth || !js_ir_push(&r, r.stack[r.depth - 1])) break;
        } else if (op == OP_drop) {
            if (!r.depth) break;
            r.depth--;
        } else if (op == OP_swap) {
            if (r.depth < 2) break;
            a = r.stack[r.depth - 1];
            r.stack[r.depth - 1] = r.stack[r.depth - 2];
            r.stack[r.depth - 2] = a;
        } else if (op == OP_put_loc || op == OP_set_loc ||
                   op == OP_put_loc8 || op == OP_set_loc8 ||
                   (op >= OP_put_loc0 && op <= OP_put_loc3) ||
                   (op >= OP_set_loc0 && op <= OP_set_loc3)) {
            keep = op == OP_set_loc || op == OP_set_loc8 ||
                   (op >= OP_set_loc0 && op <= OP_set_loc3);
            if (op == OP_put_loc || op == OP_set_loc) idx = get_u16(code + pos + 1);
            else if (op == OP_put_loc8 || op == OP_set_loc8) idx = code[pos + 1];
            else idx = op - (keep ? OP_set_loc0 : OP_put_loc0);
            if (!r.depth || idx >= b->var_count ||
                !js_ir_write(&r, idx, r.stack[r.depth - 1])) break;
            if (!keep) r.depth--;
        } else if (op == OP_inc_loc || op == OP_dec_loc || op == OP_add_loc) {
            idx = code[pos + 1];
            if (idx >= b->var_count || (op == OP_add_loc && !r.depth)) break;
            a = js_ir_input(&r, 1, idx);
            c = op == OP_add_loc ? r.stack[--r.depth] :
                js_ir_input(&r, 0, op == OP_inc_loc ? 1 : -1);
            v = js_ir_node(&r, OP_add, a, c);
            if (!js_ir_write(&r, idx, v)) break;
            r.arithmetic++;
        } else {
            switch (op) {
            case OP_add: case OP_sub: case OP_mul:
            case OP_shl: case OP_sar: case OP_shr:
            case OP_and: case OP_or: case OP_xor:
                if (r.depth < 2) goto done;
                c = r.stack[--r.depth]; a = r.stack[--r.depth];
                if (!js_ir_push(&r, js_ir_node(&r, op, a, c))) goto done;
                r.arithmetic++;
                break;
            case OP_neg: case OP_not: case OP_inc: case OP_dec:
                if (!r.depth) goto done;
                a = r.stack[--r.depth];
                if (!js_ir_push(&r, js_ir_node(&r, op, a, a))) goto done;
                r.arithmetic++;
                break;
            case OP_lt: case OP_lte: case OP_gt: case OP_gte:
            case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
                /* Boolean results are terminal; subsequent arithmetic must
                   not reinterpret the boolean as an int without conversion. */
                if (r.depth != 2) goto done;
                c = r.stack[--r.depth]; a = r.stack[--r.depth];
                if (!js_ir_push(&r, js_ir_node(&r, op, a, c))) goto done;
                r.comparison = true;
                break;
            default: goto done;
            }
        }
        r.insns++;
        pos += size;
        r.end = pos;
        if (cut && r.depth <= 1 && r.insns >= 6 && r.computes >= 2 &&
            r.arithmetic >= 1 && pos - start >= 5)
            *best = r;
        if (r.comparison) break;
    }
 done:
    return best->end != 0;
}

static void js_int_region_restore(JSIntRegionPlan *p, uint8_t *code)
{
    uint32_t i;
    for (i = 0; i < p->count; i++)
        memcpy(code + p->sites[i].pc, p->sites[i].original, 5);
}

static void js_int_region_prepare(JSRuntime *rt, JSFunctionBytecode *b)
{
#if !defined(ENABLE_DUMPS) && !defined(JS_DEBUG_TRACE_DEBUGGER_STMT) && \
    !defined(QJS_DISABLE_INT_REGION)
    JSIntRegionPlan *plan;
    JSIntRegionBuild r;
    uint8_t *targets, *payload;
    const uint8_t *code = b->byte_code_buf;
    int pos, op, size, fmt, offset, pass, count = 0, numeric = 0;
    int64_t target;
    size_t bytes = 0;
    uint32_t slot;
    /* Captured own locals, coroutines and dynamic lexical environments are
       excluded. External closure values are never accessed by this IR. */
    if (b->func_kind != JS_FUNC_NORMAL || b->var_ref_count ||
        b->byte_code_len <= 0 || b->byte_code_len > UINT16_MAX) return;
    for (pos = 0; pos < b->byte_code_len; pos += size) {
        op = js_ir_canonical_op(code[pos]);
        if (op >= OP_COUNT || !(size = js_object_layout_ops[op].size) ||
            pos + size > b->byte_code_len) return;
        switch (op) {
        case OP_eval: case OP_apply_eval: case OP_make_loc_ref: case OP_make_arg_ref:
        case OP_with_get_var: case OP_with_put_var: case OP_with_delete_var:
        case OP_with_make_ref: case OP_with_get_ref: case OP_with_get_ref_undef:
            return;
        case OP_special_object:
            if (code[pos + 1] == OP_SPECIAL_OBJECT_MAPPED_ARGUMENTS) return;
            break;
        case OP_add: case OP_sub: case OP_mul: case OP_inc_loc: case OP_dec_loc:
        case OP_add_loc: case OP_shl: case OP_sar: case OP_shr: case OP_and:
        case OP_or: case OP_xor: case OP_neg: case OP_not: case OP_inc: case OP_dec:
            numeric++;
            break;
        default: break;
        }
    }
    if (!numeric) return;
    INT_REGION_STAT(rt, scanned_functions, 1);
    targets = js_mallocz_rt(rt, b->byte_code_len);
    if (!targets) return;
    for (pos = 0; pos < b->byte_code_len; pos += size) {
        op = js_ir_canonical_op(code[pos]);
        size = js_object_layout_ops[op].size;
        if (!js_object_layout_flow(op)) continue;
        fmt = js_object_layout_ops[op].fmt;
        offset = fmt == OP_FMT_atom_label_u8 || fmt == OP_FMT_atom_label_u16 ? 5 : 1;
        target = pos + offset;
        if (fmt == OP_FMT_label8) target += (int8_t)code[pos + offset];
        else if (fmt == OP_FMT_label16) target += (int16_t)get_u16(code + pos + offset);
        else target += (int32_t)get_u32(code + pos + offset);
        if (target < 0 || target >= b->byte_code_len) goto done;
        targets[target] = 1;
    }
    plan = NULL;
    for (pass = 0; pass < 2; pass++) {
        slot = 0;
        for (pos = 0; pos < b->byte_code_len; ) {
            op = js_ir_canonical_op(code[pos]);
            if (js_ir_scan(b, targets, pos, &r)) {
                size_t n = r.inputs * sizeof(JSIntRegionInput) +
                           r.nodes * sizeof(JSIntRegionNode) +
                           r.writes * sizeof(JSIntRegionWrite);
                if (!pass) { count++; bytes += n; }
                else {
                    JSIntRegionSite *s = &plan->sites[slot++];
                    s->pc = pos; s->end = r.end;
                    s->payload = payload - (uint8_t *)plan;
                    s->inputs = r.inputs; s->nodes = r.nodes; s->writes = r.writes;
                    s->insns = r.insns;
                    s->output_kind = !r.depth ? 0 : r.comparison ? 2 : 1;
                    s->output = r.depth ? r.stack[0] : 0;
                    memcpy(s->original, code + pos, 5);
                    memcpy(payload, r.input, r.inputs * sizeof(JSIntRegionInput));
                    payload += r.inputs * sizeof(JSIntRegionInput);
                    memcpy(payload, r.node, r.nodes * sizeof(JSIntRegionNode));
                    payload += r.nodes * sizeof(JSIntRegionNode);
                    memcpy(payload, r.write, r.writes * sizeof(JSIntRegionWrite));
                    payload += r.writes * sizeof(JSIntRegionWrite);
                }
                pos = r.end;
            } else pos += js_object_layout_ops[op].size;
        }
        if (!pass) {
            if (!count) goto done;
            bytes += sizeof(*plan) + count * sizeof(plan->sites[0]);
            plan = js_mallocz_rt(rt, bytes);
            if (!plan) goto done;
            plan->bytes = bytes; plan->count = count;
            payload = (uint8_t *)(plan->sites + count);
        }
    }
    b->int_regions = plan;
    for (slot = 0; slot < plan->count; slot++) {
        uint8_t *entry = b->byte_code_buf + plan->sites[slot].pc;
        entry[0] = OP_int_region;
        put_u32(entry + 1, slot);
    }
    INT_REGION_STAT(rt, plans, 1);
    INT_REGION_STAT(rt, sites, count);
    INT_REGION_STAT(rt, bytes, bytes);
#ifdef QJS_INT_REGION_DIAGNOSTICS
    rt->int_region_stats.live_bytes += bytes;
    if (rt->int_region_stats.live_bytes > rt->int_region_stats.peak_bytes)
        rt->int_region_stats.peak_bytes = rt->int_region_stats.live_bytes;
#endif
 done:
    js_free_rt(rt, targets);
#endif
}

/* Inputs and all old write targets are guarded before computing. The compute
   phase operates on unboxed int32 values. Failure never mutates VM state. */
static bool js_int_region_run(JSRuntime *rt, JSIntRegionPlan *plan,
                              JSIntRegionSite *site, JSValue *locals,
                              JSValue *args, JSValue *result)
{
    const JSIntRegionInput *inputs = (const void *)((const uint8_t *)plan + site->payload);
    const JSIntRegionNode *nodes = (const void *)(inputs + site->inputs);
    const JSIntRegionWrite *writes = (const void *)(nodes + site->nodes);
    int32_t values[IR_NODE_BASE + IR_MAX_NODES];
    int i;
    for (i = 0; i < site->writes; i++) {
        int tag = JS_VALUE_GET_TAG(locals[writes[i].local]);
        if (tag != JS_TAG_INT && tag != JS_TAG_UNDEFINED) {
            INT_REGION_STAT(rt, write_guard_fail, 1);
            return false;
        }
    }
    for (i = 0; i < site->inputs; i++) {
        const JSIntRegionInput *in = &inputs[i];
        if (!in->kind) values[i] = in->value;
        else {
            JSValue value = in->kind == 1 ? locals[in->value] : args[in->value];
            if (JS_VALUE_GET_TAG(value) != JS_TAG_INT) {
                INT_REGION_STAT(rt, type_guard_fail, 1);
                return false;
            }
            values[i] = JS_VALUE_GET_INT(value);
        }
    }
    for (i = 0; i < site->nodes; i++) {
        const JSIntRegionNode *n = &nodes[i];
        int32_t a = values[n->left], c = values[n->right], v;
        int64_t wide;
        switch (n->op) {
        case OP_add: wide = (int64_t)a + c; goto checked;
        case OP_sub: wide = (int64_t)a - c; goto checked;
        case OP_mul:
            wide = (int64_t)a * c;
            if (!wide && (a | c) < 0) goto arithmetic_fail;
            goto checked;
        case OP_neg:
            if (!a) goto arithmetic_fail;
            wide = -(int64_t)a; goto checked;
        case OP_inc: wide = (int64_t)a + 1; goto checked;
        case OP_dec: wide = (int64_t)a - 1;
        checked:
            if (wide < INT32_MIN || wide > INT32_MAX) goto arithmetic_fail;
            v = (int32_t)wide; break;
        case OP_not: v = ~a; break;
        case OP_and: v = a & c; break;
        case OP_or: v = a | c; break;
        case OP_xor: v = a ^ c; break;
        case OP_shl: v = (int32_t)((uint32_t)a << ((uint32_t)c & 31)); break;
        case OP_sar: v = a >> ((uint32_t)c & 31); break;
        case OP_shr:
            wide = (uint32_t)a >> ((uint32_t)c & 31);
            goto checked;
        case OP_lt: v = a < c; break;
        case OP_lte: v = a <= c; break;
        case OP_gt: v = a > c; break;
        case OP_gte: v = a >= c; break;
        case OP_eq: case OP_strict_eq: v = a == c; break;
        case OP_neq: case OP_strict_neq: v = a != c; break;
        default: return false;
        }
        values[IR_NODE_BASE + i] = v;
    }
    /* No callbacks, allocation, refcounted inputs or interrupt polls occurred.
       Only now can writes become observable to the subsequent original op. */
    for (i = 0; i < site->writes; i++)
        locals[writes[i].local] = js_int32(values[writes[i].value]);
    if (site->output_kind)
        *result = site->output_kind == 2 ? js_bool(values[site->output]) :
                                         js_int32(values[site->output]);
    INT_REGION_STAT(rt, hits, 1);
    INT_REGION_STAT(rt, covered_insns, site->insns);
    INT_REGION_STAT(rt, saved_dispatches, site->insns - 1);
    return true;
 arithmetic_fail:
    INT_REGION_STAT(rt, arithmetic_fail, 1);
    return false;
}
'''

HANDLER = r'''
#if !defined(JS_DEBUG_TRACE_DEBUGGER_STMT)
        CASE(OP_int_region):
            {
                uint32_t pos = pc - b->byte_code_buf - 1;
                uint32_t index;
                JSIntRegionPlan *plan = b->int_regions;
                JSIntRegionSite *site;
                JSValue result;
                if (!plan || pos + 5 > (uint32_t)b->byte_code_len)
                    goto object_layout_invalid_opcode;
                index = get_u32(pc);
                if (index >= plan->count || plan->sites[index].pc != pos)
                    goto object_layout_invalid_opcode;
                site = &plan->sites[index];
                INT_REGION_STAT(rt, calls, 1);
                if (!js_int_region_run(rt, plan, site, var_buf, arg_buf, &result)) {
                    /* Retire once: type-unstable sites impose no recurring
                       guard tax and canonical fallback has untouched state. */
                    memcpy(b->byte_code_buf + pos, site->original, 5);
                    site->retired = 1;
                    INT_REGION_STAT(rt, retired, 1);
                    pc = b->byte_code_buf + pos;
                } else {
                    pc = b->byte_code_buf + site->end;
                    if (site->output_kind) *sp++ = result;
                    if (site->output_kind == 2 &&
                        js_take_comparison_branch(JS_VALUE_GET_INT(result), &pc)) {
                        sp--;
                        if (unlikely(js_poll_interrupts(ctx))) goto exception;
                    }
                }
            }
            BREAK;
#endif
'''


def transform(source):
    assert 'JSIntRegionPlan' not in source
    assert 'JSObjectLayoutPlan' in source and 'js_take_comparison_branch' in source
    source = once(source, 'typedef struct JSFunctionBytecode {\n', STRUCTS + '\ntypedef struct JSFunctionBytecode {\n')
    source = once(source, '    JSObjectLayoutPlan *object_layout;\n',
                  '    JSObjectLayoutPlan *object_layout;\n    JSIntRegionPlan *int_regions;\n')
    source = once(source, '#define OP_object_layout 254\n',
                  '#ifndef JS_DEBUG_TRACE_DEBUGGER_STMT\n#define OP_int_region 253\n'
                  '_Static_assert(OP_COUNT <= 252, "region/default opcodes must be unused");\n'
                  '#endif\n#define OP_object_layout 254\n')
    source = once(source, 'static JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n',
                  HELPERS + '\nstatic JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n')
    source = once(source, '        [ OP_COUNT ... 253 ] = &&case_default,\n',
                  '        [ OP_COUNT ... 252 ] = &&case_default,\n'
                  '        [ OP_int_region ] = &&case_OP_int_region,\n')
    source = once(source, '        CASE(OP_get_loc):\n', HANDLER + '\n        CASE(OP_get_loc):\n')
    source = once(source, '    /* Shape hash table */\n',
                  '#ifdef QJS_INT_REGION_DIAGNOSTICS\n'
                  '    struct { uint64_t scanned_functions, plans, sites, bytes, live_bytes, peak_bytes,\n'
                  '             calls, hits, retired, type_guard_fail, write_guard_fail, arithmetic_fail,\n'
                  '             covered_insns, saved_dispatches, dispatches; } int_region_stats;\n'
                  '#endif\n    /* Shape hash table */\n')
    # Compile and bytecode-read paths use zero-initialized JSFunctionBytecode.
    old = '    js_object_layout_prepare(ctx->rt, b);\n'
    assert source.count(old) == 2
    source = source.replace(old, old + '    js_int_region_prepare(ctx->rt, b);\n')
    source = once(source, '    if (b->object_layout) {\n        js_object_layout_restore(b->object_layout, b->byte_code_buf);',
                  '    if (b->int_regions) {\n'
                  '        js_int_region_restore(b->int_regions, b->byte_code_buf);\n'
                  '#ifdef QJS_INT_REGION_DIAGNOSTICS\n'
                  '        rt->int_region_stats.live_bytes -= b->int_regions->bytes;\n'
                  '#endif\n'
                  '        js_free_rt(rt, b->int_regions);\n'
                  '        b->int_regions = NULL;\n'
                  '    }\n'
                  '    if (b->object_layout) {\n        js_object_layout_restore(b->object_layout, b->byte_code_buf);')
    source = once(source, '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n',
                  '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n'
                  '    if (b->int_regions)\n        js_int_region_restore(b->int_regions, bc_buf);\n')
    source = once(source, '    hp->js_func_size += js_func_size;\n',
                  '    if (b->int_regions) {\n        memory_used_count++;\n'
                  '        js_func_size += b->int_regions->bytes;\n    }\n'
                  '    hp->js_func_size += js_func_size;\n')
    source = once(source, '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) switch (opcode = *pc++)\n',
                  '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) switch ((INT_REGION_STAT(rt, dispatches, 1), opcode = *pc++))\n')
    source = once(source, '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) __extension__ ({ goto *dispatch_table[opcode = *pc++]; });\n',
                  '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) __extension__ ({ INT_REGION_STAT(rt, dispatches, 1); goto *dispatch_table[opcode = *pc++]; });\n')
    source = once(source, 'void JS_RunGC(JSRuntime *rt)\n{\n',
                  'void JS_RunGC(JSRuntime *rt)\n{\n'
                  '#ifdef QJS_INT_REGION_DIAGNOSTICS\n'
                  '#define IR_PRINT(k) fprintf(stderr, " " #k "=%" PRIu64, rt->int_region_stats.k)\n'
                  '    fprintf(stderr, "INT_REGION_STATS");\n'
                  '    IR_PRINT(scanned_functions); IR_PRINT(plans); IR_PRINT(sites); IR_PRINT(bytes);\n'
                  '    IR_PRINT(live_bytes); IR_PRINT(peak_bytes); IR_PRINT(calls); IR_PRINT(hits);\n'
                  '    IR_PRINT(retired); IR_PRINT(type_guard_fail); IR_PRINT(write_guard_fail);\n'
                  '    IR_PRINT(arithmetic_fail); IR_PRINT(covered_insns); IR_PRINT(saved_dispatches);\n'
                  '    IR_PRINT(dispatches); fputc(\'\\n\', stderr);\n'
                  '#undef IR_PRINT\n#endif\n')
    return source
