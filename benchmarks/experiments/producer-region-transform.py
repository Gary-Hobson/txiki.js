"""Isolated charCodeAt producer plus three observed local/comparison suffixes.

The private entry replaces only call_method's three bytes. Its continuation is
published before any callback. Canonical suffixes stay untouched, and a failed
post-call guard resumes there without repeating the call or its interrupt poll.
"""


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:140])
    return s.replace(old, new, 1)


STRUCTS = r'''
typedef struct JSProducerRegionSite {
    uint32_t pc, branch_pc;
    int32_t rhs;
    uint16_t local;
    uint8_t cmp, keep; /* keep: 0 none, 1 original code unit, 2 boolean */
    uint8_t retired, saved_dispatches;
} JSProducerRegionSite;
typedef struct JSProducerRegionPlan {
    size_t bytes;
    uint32_t count;
    JSProducerRegionSite sites[];
} JSProducerRegionPlan;

#ifdef QJS_PRODUCER_REGION_DIAGNOSTICS
#define PRODUCER_REGION_STAT(rt, k, n) ((rt)->producer_region_stats.k += (n))
#else
#define PRODUCER_REGION_STAT(rt, k, n) ((void)0)
#endif
'''

HELPERS = r'''
static JSValue js_string_charCodeAt(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);

static int js_pr_op(const uint8_t *code, int pos)
{
    int op = code[pos];
    if (op == OP_object_layout) return OP_object;
    if (op == OP_init_object_field) return OP_define_field;
    return op;
}

/* Only inspect complete canonical instructions. Existing object-plan private
   operands are opaque, but their lengths are identical to the canonical ones. */
static int js_pr_size(JSFunctionBytecode *b, int pos)
{
    int op, size;
    if (pos < 0 || pos >= b->byte_code_len) return 0;
    op = js_pr_op(b->byte_code_buf, pos);
    if (op >= OP_COUNT) return 0;
    size = js_object_layout_ops[op].size;
    return size && size <= b->byte_code_len - pos ? size : 0;
}

static bool js_pr_match(JSFunctionBytecode *b, int start,
                        JSProducerRegionSite *site)
{
    const uint8_t *code = b->byte_code_buf;
    int pos = start, size, op, local, keep = 0, cmp;
    int32_t rhs;
    int64_t target;
    if (js_pr_size(b, pos) != 3 || code[pos] != OP_call_method ||
        get_u16(code + pos + 1) != 1) return false;
    pos += 3;
    if (!(size = js_pr_size(b, pos))) return false;
    op = code[pos];
    if (op == OP_set_loc) local = get_u16(code + pos + 1);
    else if (op == OP_set_loc8) local = code[pos + 1];
    else if (op >= OP_set_loc0 && op <= OP_set_loc3) local = op - OP_set_loc0;
    else return false;
    if (local >= b->var_count) return false;
    pos += size;
    if (!(size = js_pr_size(b, pos))) return false;
    if (code[pos] == OP_dup) {
        keep = 1;
        pos += size;
        if (!(size = js_pr_size(b, pos))) return false;
    }
    op = code[pos];
    if (op == OP_push_i32) rhs = (int32_t)get_u32(code + pos + 1);
    else if (op == OP_push_i16) rhs = (int16_t)get_u16(code + pos + 1);
    else if (op == OP_push_i8) rhs = (int8_t)code[pos + 1];
    else if (op >= OP_push_minus1 && op <= OP_push_7) rhs = op - OP_push_0;
    else return false;
    pos += size;
    if (!(size = js_pr_size(b, pos))) return false;
    cmp = code[pos];
    if ((keep == 1 && cmp != OP_strict_eq) ||
        (keep == 0 && cmp != OP_gt && cmp != OP_lte)) return false;
    pos += size;
    if (!(size = js_pr_size(b, pos))) return false;
    if (code[pos] == OP_dup) {
        if (keep || cmp != OP_lte) return false;
        keep = 2;
        pos += size;
        if (!(size = js_pr_size(b, pos))) return false;
    } else if (!keep && cmp != OP_gt) return false;
    op = code[pos];
    if (op == OP_if_false8 || op == OP_if_true8)
        target = (int64_t)pos + 1 + (int8_t)code[pos + 1];
    else if (op == OP_if_false || op == OP_if_true)
        target = (int64_t)pos + 1 + (int32_t)get_u32(code + pos + 1);
    else return false;
    if (target < 0 || target >= b->byte_code_len) return false;
    *site = (JSProducerRegionSite){
        .pc = start, .branch_pc = pos, .rhs = rhs, .local = local,
        .cmp = cmp, .keep = keep,
        /* Existing comparison/branch fusion already saves one dispatch when
           comparison and branch are adjacent. Keep-bool's dup separates them. */
        .saved_dispatches = keep == 2 ? 5 : keep == 1 ? 4 : 3,
    };
    return true;
}

static void js_producer_region_restore(JSProducerRegionPlan *plan, uint8_t *code)
{
    uint32_t i;
    for (i = 0; i < plan->count; i++) {
        uint8_t *entry = code + plan->sites[i].pc;
        entry[0] = OP_call_method;
        put_u16(entry + 1, 1);
    }
}

static void js_producer_region_prepare(JSRuntime *rt, JSFunctionBytecode *b)
{
#if !defined(ENABLE_DUMPS) && !defined(JS_DEBUG_TRACE_DEBUGGER_STMT) && !defined(QJS_DISABLE_PRODUCER_REGION)
    int pos, size, op;
    uint32_t count = 0, slot = 0;
    size_t bytes;
    JSProducerRegionSite site;
    JSProducerRegionPlan *plan;
    /* Own captured locals/arguments have references even before a closure is
       instantiated. Exclude all dynamic scope and coroutine functions too. */
    if (b->func_kind != JS_FUNC_NORMAL || b->var_ref_count ||
        b->byte_code_len <= 0 || b->byte_code_len > UINT16_MAX) return;
    for (pos = 0; pos < b->byte_code_len; pos += size) {
        if (!(size = js_pr_size(b, pos))) return;
        op = js_pr_op(b->byte_code_buf, pos);
        switch (op) {
        case OP_eval: case OP_apply_eval: case OP_make_loc_ref: case OP_make_arg_ref:
        case OP_with_get_var: case OP_with_put_var: case OP_with_delete_var:
        case OP_with_make_ref: case OP_with_get_ref: case OP_with_get_ref_undef:
            return;
        case OP_special_object:
            if (b->byte_code_buf[pos + 1] == OP_SPECIAL_OBJECT_MAPPED_ARGUMENTS) return;
            break;
        default: break;
        }
    }
    PRODUCER_REGION_STAT(rt, scanned, 1);
    for (pos = 0; pos < b->byte_code_len; pos += js_pr_size(b, pos))
        if (js_pr_match(b, pos, &site)) count++;
    if (!count || count > UINT16_MAX) return;
    bytes = sizeof(*plan) + count * sizeof(plan->sites[0]);
    plan = js_mallocz_rt(rt, bytes); /* optional: failure is non-throwing */
    if (!plan) return;
    plan->bytes = bytes;
    plan->count = count;
    for (pos = 0; pos < b->byte_code_len; pos += js_pr_size(b, pos))
        if (js_pr_match(b, pos, &site)) plan->sites[slot++] = site;
    b->producer_regions = plan;
    for (slot = 0; slot < count; slot++) {
        uint8_t *entry = b->byte_code_buf + plan->sites[slot].pc;
        entry[0] = OP_producer_region;
        put_u16(entry + 1, slot);
    }
    PRODUCER_REGION_STAT(rt, plans, 1);
    PRODUCER_REGION_STAT(rt, sites, count);
    PRODUCER_REGION_STAT(rt, bytes, bytes);
#ifdef QJS_PRODUCER_REGION_DIAGNOSTICS
    rt->producer_region_stats.live_bytes += bytes;
    if (rt->producer_region_stats.live_bytes > rt->producer_region_stats.peak_bytes)
        rt->producer_region_stats.peak_bytes = rt->producer_region_stats.live_bytes;
#endif
#endif
}

#ifndef JS_DEBUG_TRACE_DEBUGGER_STMT
static bool js_pr_identity(JSValue callee)
{
    JSObject *fn;
    if (JS_VALUE_GET_TAG(callee) != JS_TAG_OBJECT) return false;
    fn = JS_VALUE_GET_OBJ(callee);
    return fn->class_id == JS_CLASS_C_FUNCTION &&
           fn->u.cfunc.cproto == JS_CFUNC_generic &&
           fn->u.cfunc.c_function.generic == js_string_charCodeAt &&
           fn->u.cfunc.magic == 0 && fn->u.cfunc.length == 1;
}

/* The call operand stack owns receiver throughout this borrowed view. Cached
   ropes own their flat left child. No coercion, allocation or callback occurs. */
static JSString *js_pr_string(JSValue receiver)
{
    if (JS_VALUE_GET_TAG(receiver) == JS_TAG_STRING)
        return JS_VALUE_GET_STRING(receiver);
    if (JS_VALUE_GET_TAG(receiver) == JS_TAG_STRING_ROPE) {
        JSStringRope *r = JS_VALUE_GET_STRING_ROPE(receiver);
        if (JS_VALUE_GET_TAG(r->left) == JS_TAG_STRING &&
            JS_VALUE_GET_TAG(r->right) == JS_TAG_STRING &&
            JS_VALUE_GET_STRING(r->right)->len == 0)
            return JS_VALUE_GET_STRING(r->left);
    }
    return NULL;
}
#endif
'''

HANDLER = r'''
#ifndef JS_DEBUG_TRACE_DEBUGGER_STMT
        CASE(OP_producer_region):
            {
                uint32_t pos = pc - b->byte_code_buf - 1;
                uint32_t index;
                JSProducerRegionPlan *plan = b->producer_regions;
                JSProducerRegionSite *site;
                JSString *string;
                bool fast = false, value;
                int32_t char_index, code_unit;
                int old_tag;
                if (!plan || pos + 3 > (uint32_t)b->byte_code_len)
                    goto object_layout_invalid_opcode;
                index = get_u16(pc);
                if (index >= plan->count || plan->sites[index].pc != pos)
                    goto object_layout_invalid_opcode;
                site = &plan->sites[index];
                pc += 2;
                call_argc = 1;
                call_argv = sp - 1;
                /* No callback can suspend inside the three overwritten bytes. */
                sf->cur_pc = pc;
                PRODUCER_REGION_STAT(rt, entries, 1);
                if (!js_pr_identity(call_argv[-1])) {
                    /* In-flight outer handlers may retain site; never free or
                       rewrite its recipe until the bytecode itself is freed. */
                    b->byte_code_buf[pos] = OP_call_method;
                    put_u16(b->byte_code_buf + pos + 1, 1);
                    site->retired = 1;
                    PRODUCER_REGION_STAT(rt, nontarget_retired, 1);
                    goto producer_region_generic_call;
                }
                PRODUCER_REGION_STAT(rt, target, 1);
                if (JS_VALUE_GET_TAG(call_argv[0]) != JS_TAG_INT ||
                    !(string = js_pr_string(call_argv[-2]))) {
                    PRODUCER_REGION_STAT(rt, domain_miss, 1);
                    goto producer_region_generic_call;
                }
                char_index = JS_VALUE_GET_INT(call_argv[0]);
                if ((uint32_t)char_index >= string->len) {
                    PRODUCER_REGION_STAT(rt, domain_miss, 1);
                    goto producer_region_generic_call;
                }
                /* Exactly the poll originally at JS_CallInternal's entry.
                   No local value or borrowed string view crosses this poll. */
                if (unlikely(js_poll_interrupts(ctx))) goto exception;
                string = js_pr_string(call_argv[-2]);
                if (!string || (uint32_t)char_index >= string->len) {
                    /* Poll already happened: continue at native dispatch, not
                       JS_CallInternal, which would repeat it. */
                    PRODUCER_REGION_STAT(rt, postpoll_domain_miss, 1);
                    ret_val = js_call_c_function(ctx, call_argv[-1], call_argv[-2],
                                                 1, vc(call_argv), 0);
                    goto producer_region_cleanup;
                }
                /* Same native argument-buffer stack check, before publishing
                   a native frame/realm. The guarded body cannot throw. */
                if (js_check_stack_overflow(rt, sizeof(JSValue))) {
                    JS_ThrowStackOverflow(ctx);
                    goto exception;
                }
                code_unit = string_get(string, char_index);
                ret_val = js_int32(code_unit);
                fast = true;
                if (JS_VALUE_GET_TAG(call_argv[-2]) == JS_TAG_STRING)
                    PRODUCER_REGION_STAT(rt, fast_flat, 1);
                else
                    PRODUCER_REGION_STAT(rt, fast_cached_rope, 1);
                goto producer_region_cleanup;
            producer_region_generic_call:
                PRODUCER_REGION_STAT(rt, generic_calls, 1);
                ret_val = JS_CallInternal(ctx, call_argv[-1], call_argv[-2],
                                          JS_UNDEFINED, 1, vc(call_argv), 0);
            producer_region_cleanup:
                if (unlikely(JS_IsException(ret_val))) goto exception;
                /* Preserve the baseline receiver, callee, argument release
                   order. Finalizers can run here, before local assignment. */
                for (i = -2; i < 1; i++) JS_FreeValue(ctx, call_argv[i]);
                sp -= 3;
                if (!fast) {
                    *sp++ = ret_val;
                } else {
                    old_tag = JS_VALUE_GET_TAG(var_buf[site->local]);
                    if (old_tag != JS_TAG_INT && old_tag != JS_TAG_UNDEFINED) {
                        PRODUCER_REGION_STAT(rt, local_guard_fail, 1);
                        *sp++ = ret_val;
                    } else {
                        /* Commit only after both observable checkpoints.
                           Old INT/undefined needs no free; result is immediate. */
                        var_buf[site->local] = ret_val;
                        value = site->cmp == OP_gt ? code_unit > site->rhs :
                                site->cmp == OP_lte ? code_unit <= site->rhs :
                                                     code_unit == site->rhs;
                        if (site->keep == 1) *sp++ = ret_val;
                        else if (site->keep == 2) *sp++ = js_bool(value);
                        pc = b->byte_code_buf + site->branch_pc;
                        js_take_comparison_branch(value, &pc);
                        PRODUCER_REGION_STAT(rt, suffix_hits, 1);
                        if (site->keep == 0) PRODUCER_REGION_STAT(rt, hit_none, 1);
                        else if (site->keep == 1) PRODUCER_REGION_STAT(rt, hit_code, 1);
                        else PRODUCER_REGION_STAT(rt, hit_bool, 1);
                        PRODUCER_REGION_STAT(rt, saved_dispatches, site->saved_dispatches);
                        /* Original conditional branch polls on either edge. */
                        if (unlikely(js_poll_interrupts(ctx))) goto exception;
                    }
                }
            }
            BREAK;
#endif
'''


def transform(source):
    assert 'JSProducerRegionPlan' not in source
    assert 'JSObjectLayoutPlan' in source and 'js_take_comparison_branch' in source
    source = once(source, 'typedef struct JSFunctionBytecode {\n', STRUCTS + '\ntypedef struct JSFunctionBytecode {\n')
    source = once(source, '    JSObjectLayoutPlan *object_layout;\n', '    JSObjectLayoutPlan *object_layout;\n    JSProducerRegionPlan *producer_regions;\n')
    source = once(source, '#define OP_object_layout 254\n', '#ifndef JS_DEBUG_TRACE_DEBUGGER_STMT\n#define OP_producer_region 253\n_Static_assert(OP_COUNT <= 252, "producer/default opcode space");\n#endif\n#define OP_object_layout 254\n')
    source = once(source, 'static JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n', HELPERS + '\nstatic JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n')
    source = once(source, '        [ OP_COUNT ... 253 ] = &&case_default,\n', '        [ OP_COUNT ... 252 ] = &&case_default,\n        [ OP_producer_region ] = &&case_OP_producer_region,\n')
    source = once(source, '        CASE(OP_call_method):\n', HANDLER + '\n        CASE(OP_call_method):\n')
    stats = ['scanned', 'plans', 'sites', 'bytes', 'live_bytes', 'peak_bytes', 'entries', 'target', 'nontarget_retired', 'domain_miss', 'postpoll_domain_miss', 'generic_calls', 'fast_flat', 'fast_cached_rope', 'local_guard_fail', 'suffix_hits', 'hit_none', 'hit_code', 'hit_bool', 'saved_dispatches', 'dispatches']
    source = once(source, '    /* Shape hash table */\n', '#ifdef QJS_PRODUCER_REGION_DIAGNOSTICS\n    struct { uint64_t ' + ', '.join(stats) + '; } producer_region_stats;\n#endif\n    /* Shape hash table */\n')
    old = '    js_object_layout_prepare(ctx->rt, b);\n'
    assert source.count(old) == 2
    source = source.replace(old, old + '    js_producer_region_prepare(ctx->rt, b);\n')
    source = once(source, '    if (b->object_layout) {\n        js_object_layout_restore(b->object_layout, b->byte_code_buf);', '    if (b->producer_regions) {\n        js_producer_region_restore(b->producer_regions, b->byte_code_buf);\n#ifdef QJS_PRODUCER_REGION_DIAGNOSTICS\n        rt->producer_region_stats.live_bytes -= b->producer_regions->bytes;\n#endif\n        js_free_rt(rt, b->producer_regions);\n        b->producer_regions = NULL;\n    }\n    if (b->object_layout) {\n        js_object_layout_restore(b->object_layout, b->byte_code_buf);')
    source = once(source, '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n', '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n    if (b->producer_regions)\n        js_producer_region_restore(b->producer_regions, bc_buf);\n')
    source = once(source, '    hp->js_func_size += js_func_size;\n', '    if (b->producer_regions) {\n        memory_used_count++;\n        js_func_size += b->producer_regions->bytes;\n    }\n    hp->js_func_size += js_func_size;\n')
    source = once(source, '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) switch (opcode = *pc++)\n', '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) switch ((PRODUCER_REGION_STAT(rt, dispatches, 1), opcode = *pc++))\n')
    source = once(source, '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) __extension__ ({ goto *dispatch_table[opcode = *pc++]; });\n', '#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) __extension__ ({ PRODUCER_REGION_STAT(rt, dispatches, 1); goto *dispatch_table[opcode = *pc++]; });\n')
    printing = '\n'.join('    PR_PRINT(' + s + ');' for s in stats)
    source = once(source, 'void JS_RunGC(JSRuntime *rt)\n{\n', 'void JS_RunGC(JSRuntime *rt)\n{\n#ifdef QJS_PRODUCER_REGION_DIAGNOSTICS\n#define PR_PRINT(k) fprintf(stderr, " " #k "=%" PRIu64, rt->producer_region_stats.k)\n    fprintf(stderr, "PRODUCER_REGION_STATS");\n' + printing + '\n    fputc(\'\\n\', stderr);\n#undef PR_PRINT\n#endif\n')
    return source
