"""Diagnostic only: actual direct charCodeAt call_method producers and successors.

Instrumentation runs in the JS bytecode call_method handler after successful
return and before original cleanup. It stores no JSValue, atom, b or pc pointer.
No optimization, reordered call, changed poll or changed ownership operation.
"""


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:140])
    return s.replace(old, new, 1)


HELPERS = r'''
/* Shape buckets, not unique call-site counts. A sampled site's opcode syntax is
   decoded independently in its current function on every successful call. */
enum {
    PS_DIRECT_COMPARE, PS_LOCAL_COMPARE, PS_MULTI_COMPUTE, PS_ARITHMETIC,
    PS_STORE_ONLY, PS_TRUTHINESS, PS_OTHER, PS_CATEGORIES
};
enum {
    PS_BRANCH, PS_CALL, PS_PROPERTY, PS_CLOSURE, PS_RETURN, PS_OTHER_STOP,
    PS_STACK, PS_LIMIT, PS_INVALID, PS_REASONS
};
static const char * const ps_categories[] = {
    "direct_compare", "local_compare", "multi_compute", "arithmetic",
    "store_only", "truthiness", "other"
};
static const char * const ps_reasons[] = {
    "branch", "call", "property", "closure", "return", "other",
    "preexisting_stack", "limit", "invalid"
};
static const char * const ps_opnames[OP_COUNT] = {
#define DEF(id, size, n_pop, n_push, f) #id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
};

static int js_ps_local_taint(int index, const uint16_t *locals,
                             const uint8_t *taints, int count)
{
    int i;
    for (i = 0; i < count; i++) if (locals[i] == index) return taints[i];
    return 0;
}

static void js_profile_charcode_successor(JSContext *ctx, JSStackFrame *sf,
                                          JSFunctionBytecode *b, const uint8_t *pc,
                                          JSValue callee, JSValue receiver,
                                          int argc, JSValue *argv, JSValue result)
{
    JSRuntime *rt = ctx->rt;
    JSObject *fn, *caller;
    uint8_t stack[16] = {1}, taints[16] = {0}, ops[24];
    uint16_t locals[16];
    int depth = 1, local_count = 0, pos, op, size, idx, v, i;
    int len = 0, computes = 0, tainted_computes = 0, comparisons = 0, stores = 0;
    int reason = PS_LIMIT, category = PS_OTHER, branch_taint = 0;
    int last_compare = 0;
    bool keep;
    uint32_t hash = 2166136261U, slot, probe;
    rt->producer_site_stats.method_success++;
    if (JS_VALUE_GET_TAG(callee) != JS_TAG_OBJECT) return;
    fn = JS_VALUE_GET_OBJ(callee);
    if (fn->class_id != JS_CLASS_C_FUNCTION ||
        fn->u.cfunc.c_function.generic != js_string_charCodeAt ||
        fn->u.cfunc.cproto != JS_CFUNC_generic || fn->u.cfunc.magic != 0 ||
        fn->u.cfunc.length != 1) return;
    rt->producer_site_stats.charcode_identity++;
    if (argc < 1 || JS_VALUE_GET_TAG(receiver) != JS_TAG_STRING ||
        JS_VALUE_GET_TAG(argv[0]) != JS_TAG_INT) return;
    rt->producer_site_stats.pure_domain++;
    if (argc == 1) rt->producer_site_stats.single_argument++;
    else rt->producer_site_stats.extra_arguments++;
    idx = JS_VALUE_GET_INT(argv[0]);
    if (idx < 0 || idx >= JS_VALUE_GET_STRING(receiver)->len) {
        rt->producer_site_stats.out_of_bounds++;
        return;
    }
    rt->producer_site_stats.in_bounds++;
    /* The originating OP_call_method owns these values and b/sf. A nested
       native call cannot impersonate this site's callee: callee comes from
       this handler's original operand stack, not the current C-frame chain. */
    if (rt->current_stack_frame != sf || sf->cur_pc != pc ||
        JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT) goto invalid_caller;
    caller = JS_VALUE_GET_OBJ(sf->cur_func);
    if (caller->class_id != JS_CLASS_BYTECODE_FUNCTION ||
        caller->u.func.function_bytecode != b ||
        pc < b->byte_code_buf + 3 || pc > b->byte_code_buf + b->byte_code_len ||
        pc[-3] != OP_call_method || get_u16(pc - 2) != argc ||
        JS_VALUE_GET_TAG(result) != JS_TAG_INT) goto invalid_caller;
    if (b->func_kind != JS_FUNC_NORMAL) rt->producer_site_stats.coroutine++;
    if (b->var_ref_count) rt->producer_site_stats.own_captured++;
    pos = pc - b->byte_code_buf;
    while (pos < b->byte_code_len && len < 24) {
        op = b->byte_code_buf[pos];
        if (op == OP_object_layout) op = OP_object;
        if (op == OP_init_object_field) op = OP_define_field;
        if (op >= OP_COUNT || !(size = js_object_layout_ops[op].size) ||
            pos + size > b->byte_code_len) { reason = PS_INVALID; break; }
        ops[len++] = op;
        hash = (hash ^ op) * 16777619U;
        keep = false;
        last_compare = 0;
        if (op == OP_get_loc || op == OP_get_loc8 ||
            (op >= OP_get_loc0 && op <= OP_get_loc3)) {
            idx = op == OP_get_loc ? get_u16(b->byte_code_buf + pos + 1) :
                  op == OP_get_loc8 ? b->byte_code_buf[pos + 1] : op - OP_get_loc0;
            if (depth == 16) { reason = PS_LIMIT; break; }
            stack[depth++] = js_ps_local_taint(idx, locals, taints, local_count);
        } else if (op == OP_get_loc0_loc1) {
            if (depth > 14) { reason = PS_LIMIT; break; }
            stack[depth++] = js_ps_local_taint(0, locals, taints, local_count);
            stack[depth++] = js_ps_local_taint(1, locals, taints, local_count);
        } else if (op == OP_get_arg || (op >= OP_get_arg0 && op <= OP_get_arg3) ||
                   op == OP_push_i8 || op == OP_push_i16 || op == OP_push_i32 ||
                   op == OP_push_const || op == OP_push_const8 ||
                   (op >= OP_push_minus1 && op <= OP_push_7) ||
                   op == OP_push_true || op == OP_push_false || op == OP_null ||
                   op == OP_undefined || op == OP_push_atom_value) {
            if (depth == 16) { reason = PS_LIMIT; break; }
            stack[depth++] = 0;
        } else if (op == OP_put_loc || op == OP_set_loc ||
                   op == OP_put_loc8 || op == OP_set_loc8 ||
                   (op >= OP_put_loc0 && op <= OP_put_loc3) ||
                   (op >= OP_set_loc0 && op <= OP_set_loc3)) {
            keep = op == OP_set_loc || op == OP_set_loc8 ||
                   (op >= OP_set_loc0 && op <= OP_set_loc3);
            if (!depth) { reason = PS_STACK; break; }
            if (op == OP_put_loc || op == OP_set_loc) idx = get_u16(b->byte_code_buf + pos + 1);
            else if (op == OP_put_loc8 || op == OP_set_loc8) idx = b->byte_code_buf[pos + 1];
            else idx = op - (keep ? OP_set_loc0 : OP_put_loc0);
            v = stack[depth - 1];
            if (!keep) depth--;
            if (v) stores++;
            for (i = 0; i < local_count; i++) if (locals[i] == idx) break;
            if (i == local_count) {
                if (local_count == 16) { reason = PS_LIMIT; break; }
                locals[local_count++] = idx;
            }
            taints[i] = v;
        } else if (op == OP_dup) {
            if (!depth) { reason = PS_STACK; break; }
            if (depth == 16) { reason = PS_LIMIT; break; }
            stack[depth] = stack[depth - 1]; depth++;
        } else if (op == OP_drop) {
            if (!depth) { reason = PS_STACK; break; }
            depth--;
        } else if (op == OP_swap) {
            if (depth < 2) { reason = PS_STACK; break; }
            v = stack[depth - 1]; stack[depth - 1] = stack[depth - 2]; stack[depth - 2] = v;
        } else {
            switch (op) {
            case OP_lt: case OP_lte: case OP_gt: case OP_gte:
            case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
                last_compare = 1;
                /* fall through */
            case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
            case OP_shl: case OP_sar: case OP_shr: case OP_and: case OP_or: case OP_xor:
                if (depth < 2) { reason = PS_STACK; goto finished; }
                v = stack[--depth]; v |= stack[depth - 1]; stack[depth - 1] = v;
                computes++;
                if (v) { tainted_computes++; comparisons += last_compare; }
                break;
            case OP_neg: case OP_not: case OP_inc: case OP_dec: case OP_lnot:
                if (!depth) { reason = PS_STACK; goto finished; }
                computes++;
                tainted_computes += stack[depth - 1] != 0;
                break;
            case OP_if_false: case OP_if_true: case OP_if_false8: case OP_if_true8:
                branch_taint = depth ? stack[depth - 1] : 0;
                reason = PS_BRANCH; goto finished;
            case OP_goto: case OP_goto8: case OP_goto16: case OP_catch:
            case OP_gosub: case OP_ret:
                reason = PS_BRANCH; goto finished;
            case OP_call: case OP_call0: case OP_call1: case OP_call2: case OP_call3:
            case OP_call_method: case OP_tail_call: case OP_tail_call_method:
            case OP_call_constructor: case OP_apply: case OP_eval: case OP_apply_eval:
                reason = PS_CALL; goto finished;
            case OP_get_field: case OP_get_field2: case OP_get_length:
            case OP_get_array_el: case OP_get_array_el2: case OP_put_field:
            case OP_put_array_el: case OP_define_field: case OP_object:
                reason = PS_PROPERTY; goto finished;
            case OP_get_var_ref: case OP_get_var_ref0: case OP_get_var_ref1:
            case OP_get_var_ref2: case OP_get_var_ref3: case OP_get_var_ref_check:
            case OP_put_var_ref: case OP_put_var_ref0: case OP_put_var_ref1:
            case OP_put_var_ref2: case OP_put_var_ref3: case OP_put_var_ref_check:
            case OP_set_var_ref: case OP_set_var_ref0: case OP_set_var_ref1:
            case OP_set_var_ref2: case OP_set_var_ref3:
                reason = PS_CLOSURE; goto finished;
            case OP_return: case OP_return_undef: case OP_return_async:
                reason = PS_RETURN; goto finished;
            default:
                reason = PS_OTHER_STOP; goto finished;
            }
        }
        pos += size;
    }
 finished:
    if (tainted_computes >= 2) category = PS_MULTI_COMPUTE;
    else if (comparisons) category = stores ? PS_LOCAL_COMPARE : PS_DIRECT_COMPARE;
    else if (tainted_computes) category = PS_ARITHMETIC;
    else if (branch_taint) category = PS_TRUTHINESS;
    else if (stores) category = PS_STORE_ONLY;
    rt->producer_site_stats.categories[category]++;
    rt->producer_site_stats.reasons[reason]++;
    rt->producer_site_stats.successor_insns += len;
    rt->producer_site_stats.tainted_computes += tainted_computes;
    if (b->func_kind == JS_FUNC_NORMAL && argc == 1 && !b->var_ref_count)
        rt->producer_site_stats.normal_single_uncaptured[category]++;
    hash = (hash ^ category) * 16777619U;
    hash = (hash ^ reason) * 16777619U;
    hash = (hash ^ tainted_computes) * 16777619U;
    hash = (hash ^ stores) * 16777619U;
    for (probe = 0; probe < 256; probe++) {
        slot = (hash + probe) & 255;
        if (!rt->producer_patterns[slot].count) {
            char fn_name[160];
            rt->producer_patterns[slot].len = len;
            rt->producer_patterns[slot].category = category;
            rt->producer_patterns[slot].reason = reason;
            rt->producer_patterns[slot].computes = tainted_computes;
            rt->producer_patterns[slot].stores = stores;
            memcpy(rt->producer_patterns[slot].ops, ops, len);
            fprintf(stderr, "PRODUCER_PATTERN_EXAMPLE slot=%u function=%s line=%d call_pc=%u category=%s stop=%s\n",
                    slot, JS_AtomGetStr(ctx, fn_name, sizeof(fn_name), b->func_name),
                    b->line_num, (unsigned)(pc - b->byte_code_buf - 3),
                    ps_categories[category], ps_reasons[reason]);
        }
        if (rt->producer_patterns[slot].len == len &&
            rt->producer_patterns[slot].category == category &&
            rt->producer_patterns[slot].reason == reason &&
            rt->producer_patterns[slot].computes == tainted_computes &&
            rt->producer_patterns[slot].stores == stores &&
            !memcmp(rt->producer_patterns[slot].ops, ops, len)) {
            rt->producer_patterns[slot].count++;
            return;
        }
    }
    rt->producer_site_stats.pattern_overflow++;
    return;
 invalid_caller:
    rt->producer_site_stats.invalid_caller++;
}

static void js_dump_producer_site_stats(JSRuntime *rt)
{
    unsigned int i, j;
#define PS_PRINT(k) fprintf(stderr, " " #k "=%" PRIu64, rt->producer_site_stats.k)
    fprintf(stderr, "PRODUCER_SITE_STATS");
    PS_PRINT(method_success); PS_PRINT(charcode_identity); PS_PRINT(pure_domain);
    PS_PRINT(single_argument); PS_PRINT(extra_arguments); PS_PRINT(in_bounds); PS_PRINT(out_of_bounds);
    PS_PRINT(invalid_caller); PS_PRINT(coroutine); PS_PRINT(own_captured);
    PS_PRINT(successor_insns); PS_PRINT(tainted_computes); PS_PRINT(pattern_overflow);
    fputc('\n', stderr);
#undef PS_PRINT
    for (i = 0; i < PS_CATEGORIES; i++)
        fprintf(stderr, "PRODUCER_CATEGORY name=%s count=%" PRIu64 " normal_single_uncaptured=%" PRIu64 "\n",
                ps_categories[i], rt->producer_site_stats.categories[i], rt->producer_site_stats.normal_single_uncaptured[i]);
    for (i = 0; i < PS_REASONS; i++)
        fprintf(stderr, "PRODUCER_STOP name=%s count=%" PRIu64 "\n", ps_reasons[i], rt->producer_site_stats.reasons[i]);
    for (i = 0; i < 256; i++) {
        if (!rt->producer_patterns[i].count) continue;
        fprintf(stderr, "PRODUCER_PATTERN slot=%u count=%" PRIu64 " category=%s stop=%s computes=%u stores=%u ops=",
                i, rt->producer_patterns[i].count, ps_categories[rt->producer_patterns[i].category],
                ps_reasons[rt->producer_patterns[i].reason], rt->producer_patterns[i].computes,
                rt->producer_patterns[i].stores);
        for (j = 0; j < rt->producer_patterns[i].len; j++)
            fprintf(stderr, "%s%s", j ? "," : "", ps_opnames[rt->producer_patterns[i].ops[j]]);
        fputc('\n', stderr);
    }
}
'''


def transform(source):
    assert 'producer_site_stats' not in source
    declaration = ('static JSValue js_string_charCodeAt(JSContext *ctx, JSValueConst this_val,\n'
                   '                                     int argc, JSValueConst *argv);\n'
                   'static void js_dump_producer_site_stats(JSRuntime *rt);\n')
    source = once(source, 'static int JS_InitAtoms(JSRuntime *rt);\n', declaration + '\nstatic int JS_InitAtoms(JSRuntime *rt);\n')
    source = once(source, '    /* Shape hash table */\n',
        '    struct {\n'
        '        uint64_t method_success, charcode_identity, pure_domain, single_argument, extra_arguments,\n'
        '                 in_bounds, out_of_bounds, invalid_caller, coroutine, own_captured,\n'
        '                 successor_insns, tainted_computes, pattern_overflow;\n'
        '        uint64_t categories[7], normal_single_uncaptured[7], reasons[9];\n'
        '    } producer_site_stats;\n'
        '    struct { uint64_t count; uint8_t len, category, reason, computes, stores; uint8_t ops[24]; } producer_patterns[256];\n'
        '    /* Shape hash table */\n')
    source = once(source, 'static JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n',
                  HELPERS + '\nstatic JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n')
    old = '''                if (opcode == OP_tail_call_method)
                    goto done;
'''
    source = once(source, old,
                  '                if (opcode == OP_call_method)\n'
                  '                    js_profile_charcode_successor(ctx, sf, b, pc, call_argv[-1],\n'
                  '                                                  call_argv[-2], call_argc, call_argv, ret_val);\n' + old)
    source = once(source, 'void JS_RunGC(JSRuntime *rt)\n{\n',
                  'void JS_RunGC(JSRuntime *rt)\n{\n    js_dump_producer_site_stats(rt);\n')
    return source
