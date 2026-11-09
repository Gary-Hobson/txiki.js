"""Unescaped straight-line object initialization plans; canonical BC stays portable.

Private opcodes 254/255 intentionally cannot be combined with the read-IC prototype.
No persistent opcode or bytecode version changes. Plan allocation failure is optional.
"""

def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:120])
    return s.replace(old, new, 1)

TYPES = r'''
typedef struct JSObjectLayoutSite {
    uint32_t pc;
    uint32_t first_field;
    uint32_t count;
    JSShape *shape;
} JSObjectLayoutSite;

typedef struct JSObjectLayoutField {
    uint32_t pc;
    JSAtom atom; /* owned by the canonical bytecode, not the plan */
} JSObjectLayoutField;

typedef struct JSObjectLayoutPlan {
    size_t bytes;
    uint32_t site_count;
    uint32_t field_count;
    JSObjectLayoutSite sites[];
} JSObjectLayoutPlan;

static JSObjectLayoutField *js_object_layout_fields(JSObjectLayoutPlan *p)
{
    return (JSObjectLayoutField *)(p->sites + p->site_count);
}

#ifdef QJS_OBJECT_LAYOUT_DIAGNOSTICS
#define OBJECT_LAYOUT_STAT(rt, key, n) ((rt)->object_layout_stats.key += (n))
#else
#define OBJECT_LAYOUT_STAT(rt, key, n) ((void)0)
#endif
'''

HELPERS = r'''
/* Metadata is also available with the parser disabled. */
static const struct { uint8_t size, pop, push, fmt; } js_object_layout_ops[OP_COUNT] = {
#define DEF(id, size, n_pop, n_push, f) { size, n_pop, n_push, OP_FMT_ ## f },
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
};

static bool js_object_layout_flow(int op)
{
    int fmt = js_object_layout_ops[op].fmt;
    return fmt == OP_FMT_label || fmt == OP_FMT_label8 ||
           fmt == OP_FMT_label16 || fmt == OP_FMT_label_u16 ||
           fmt == OP_FMT_atom_label_u8 || fmt == OP_FMT_atom_label_u16;
}

/* The target is stack[base]. No opcode may read it until the final consumer,
   except define_field, which is known not to expose a new ordinary object. */
static int js_object_layout_scan(const uint8_t *code, int len,
                                const uint8_t *targets, int start,
                                JSObjectLayoutField *fields)
{
    JSAtom atoms[64];
    int pos, op, pop, fmt, i, count = 0, depth = 1;
    for (pos = start + 1; pos < len; pos += js_object_layout_ops[op].size) {
        op = code[pos];
        if (targets[pos] || js_object_layout_flow(op))
            return 0;
        /* These have control flow or nonlocal stack effects beyond pop/push. */
        switch (op) {
        case OP_nip_catch: case OP_ret: case OP_initial_yield:
        case OP_yield: case OP_yield_star: case OP_async_yield_star: case OP_await:
        case OP_for_in_start: case OP_for_of_start: case OP_for_await_of_start:
        case OP_for_in_next: case OP_for_of_next: case OP_iterator_get_value_done:
        case OP_iterator_close: case OP_iterator_next: case OP_iterator_call:
        case OP_copy_data_properties:
            return 0;
        default: break;
        }
        pop = js_object_layout_ops[op].pop;
        fmt = js_object_layout_ops[op].fmt;
        if (fmt == OP_FMT_npop || fmt == OP_FMT_npop_u16)
            pop += get_u16(code + pos + 1);
        else if (fmt == OP_FMT_npopx)
            pop += op - OP_call0;
        if (op == OP_define_field && depth == 2) {
            JSAtom atom = get_u32(code + pos + 1);
            if (atom == JS_ATOM___proto__ || count == countof(atoms))
                return 0;
            for (i = 0; i < count; i++)
                if (atoms[i] == atom)
                    return 0;
            atoms[count] = atom;
            if (fields) {
                fields[count].pc = pos;
                fields[count].atom = atom;
            }
            count++;
        } else if (pop >= depth) {
            switch (op) {
            /* A different construction operation would invalidate the recipe. */
            case OP_set_proto: case OP_set_home_object: case OP_define_method:
            case OP_define_method_computed: case OP_define_array_el:
            case OP_define_private_field: case OP_append:
            case OP_dup: case OP_dup1: case OP_dup2: case OP_dup3:
            case OP_insert2: case OP_insert3: case OP_insert4:
            case OP_perm3: case OP_perm4: case OP_perm5:
            case OP_swap: case OP_swap2: case OP_rot3l: case OP_rot3r:
            case OP_rot4l: case OP_rot5l:
                return 0;
            default:
                return count >= 2 ? count : 0;
            }
        }
        /* Terminal instructions may not leave the target stranded. */
        switch (op) {
        case OP_tail_call: case OP_tail_call_method: case OP_return:
        case OP_return_undef: case OP_return_async: case OP_throw:
        case OP_throw_error:
            return 0;
        default: break;
        }
        depth += js_object_layout_ops[op].push - pop;
    }
    return 0;
}

/* Reconstruct canonical bytes for serialization and atom destruction. */
static void js_object_layout_restore(JSObjectLayoutPlan *plan, uint8_t *code)
{
    JSObjectLayoutField *fields = js_object_layout_fields(plan);
    uint32_t i;
    for (i = 0; i < plan->site_count; i++)
        code[plan->sites[i].pc] = OP_object;
    for (i = 0; i < plan->field_count; i++) {
        code[fields[i].pc] = OP_define_field;
        put_u32(code + fields[i].pc + 1, fields[i].atom);
    }
}

static void js_object_layout_prepare(JSRuntime *rt, JSFunctionBytecode *b)
{
    JSObjectLayoutPlan *plan;
    JSObjectLayoutField *fields;
    JSObjectLayoutField scanned[64];
    uint8_t *code = b->byte_code_buf, *targets;
    uint32_t sites = 0, fields_count = 0, si = 0, fi = 0;
    int pos, op, size, fmt, offset, count;
    int64_t target;
    bool has_object = false;
    size_t bytes;
#if defined(ENABLE_DUMPS) || defined(JS_DEBUG_TRACE_DEBUGGER_STMT) || \
    defined(QJS_DISABLE_OBJECT_LAYOUT)
    return;
#endif
    if (b->byte_code_len <= 0 || b->byte_code_len > UINT16_MAX)
        return;
    for (pos = 0; pos < b->byte_code_len; pos += size) {
        op = code[pos];
        if (op >= OP_COUNT || !(size = js_object_layout_ops[op].size) ||
            pos + size > b->byte_code_len)
            return;
        has_object |= op == OP_object;
    }
    if (!has_object)
        return;
    targets = js_mallocz_rt(rt, b->byte_code_len);
    if (!targets)
        return;
    for (pos = 0; pos < b->byte_code_len; pos += js_object_layout_ops[op].size) {
        op = code[pos];
        if (!js_object_layout_flow(op))
            continue;
        fmt = js_object_layout_ops[op].fmt;
        offset = fmt == OP_FMT_atom_label_u8 || fmt == OP_FMT_atom_label_u16 ? 5 : 1;
        target = pos + offset;
        if (fmt == OP_FMT_label8)
            target += (int8_t)code[pos + offset];
        else if (fmt == OP_FMT_label16)
            target += (int16_t)get_u16(code + pos + offset);
        else
            target += (int32_t)get_u32(code + pos + offset);
        if (target < 0 || target >= b->byte_code_len)
            goto done;
        targets[target] = 1;
    }
    for (pos = 0; pos < b->byte_code_len; pos += js_object_layout_ops[op].size) {
        op = code[pos];
        if (op == OP_object && (count = js_object_layout_scan(code, b->byte_code_len,
                                                             targets, pos, NULL))) {
            sites++;
            fields_count += count;
        }
    }
    if (!sites)
        goto done;
    bytes = sizeof(*plan) + sites * sizeof(plan->sites[0]) +
            fields_count * sizeof(*fields);
    plan = js_mallocz_rt(rt, bytes);
    if (!plan)
        goto done;
    plan->bytes = bytes;
    plan->site_count = sites;
    plan->field_count = fields_count;
    fields = js_object_layout_fields(plan);
    for (pos = 0; pos < b->byte_code_len; pos += js_object_layout_ops[op].size) {
        op = code[pos];
        if (op != OP_object)
            continue;
        count = js_object_layout_scan(code, b->byte_code_len, targets, pos, scanned);
        if (count) {
            memcpy(fields + fi, scanned, count * sizeof(*fields));
            plan->sites[si].pc = pos;
            plan->sites[si].first_field = fi;
            plan->sites[si++].count = count;
            fi += count;
        }
    }
    b->object_layout = plan;
    for (si = 0; si < sites; si++) {
        JSObjectLayoutSite *site = &plan->sites[si];
        code[site->pc] = OP_object_layout;
        for (fi = 0; fi < site->count; fi++) {
            uint32_t field_pc = fields[site->first_field + fi].pc;
            put_u32(code + field_pc + 1, fi);
            code[field_pc] = OP_init_object_field;
        }
    }
    OBJECT_LAYOUT_STAT(rt, plans, 1);
    OBJECT_LAYOUT_STAT(rt, sites, sites);
    OBJECT_LAYOUT_STAT(rt, fields, fields_count);
    OBJECT_LAYOUT_STAT(rt, bytes, bytes);
 done:
    js_free_rt(rt, targets);
}

static JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)
{
    JSObjectLayoutPlan *plan = b->object_layout;
    JSObjectLayoutField *fields = js_object_layout_fields(plan);
    JSObjectLayoutSite *site;
    JSObject *proto = object_or_null(ctx->class_proto[JS_CLASS_OBJECT]);
    JSShape *sh;
    JSValue obj;
    uint32_t lo = 0, hi = plan->site_count, mid, i, hash_size, hash;
    while (lo < hi) {
        mid = (lo + hi) / 2;
        if (plan->sites[mid].pc < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    assert(lo < plan->site_count && plan->sites[lo].pc == pc);
    site = &plan->sites[lo];
    sh = site->shape;
    if (!sh || sh->proto != proto) {
        if (sh) {
            site->shape = NULL;
            js_free_shape(ctx->rt, sh);
        }
        hash_size = JS_PROP_INITIAL_HASH_SIZE;
        while (hash_size < site->count)
            hash_size *= 2;
        if (2 * (ctx->rt->shape_hash_count + 1) > ctx->rt->shape_hash_size)
            resize_shape_hash(ctx->rt, ctx->rt->shape_hash_bits + 1);
        sh = js_new_shape_nohash(ctx, proto, hash_size, site->count);
        if (!sh)
            return JS_EXCEPTION;
        hash = shape_initial_hash(proto);
        for (i = 0; i < site->count; i++) {
            JSAtom atom = fields[site->first_field + i].atom;
            /* Exact capacity: this cannot allocate or fail. */
            int ret = add_shape_property(ctx, &sh, NULL, atom, JS_PROP_C_W_E);
            assert(ret == 0);
            (void)ret;
            hash = shape_hash(shape_hash(hash, atom), JS_PROP_C_W_E);
        }
        sh->hash = hash;
        sh->is_hashed = true;
        js_shape_hash_link(ctx->rt, sh);
        site->shape = sh;
        OBJECT_LAYOUT_STAT(ctx->rt, shapes, 1);
    }
    obj = JS_NewObjectFromShape(ctx, js_dup_shape(sh), JS_CLASS_OBJECT, NULL);
    if (!JS_IsException(obj)) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        /* No allocation/callback occurs between object publication and here. */
        for (i = 0; i < site->count; i++)
            p->prop[i].u.value = JS_UNDEFINED;
        OBJECT_LAYOUT_STAT(ctx->rt, objects, 1);
        OBJECT_LAYOUT_STAT(ctx->rt, initialized, site->count);
    }
    return obj;
}
'''

def transform(source):
    assert 'JSReadICPlan' not in source and 'JSObjectLayoutPlan' not in source
    source = once(source, '    /* Shape hash table */\n',
        '#ifdef QJS_OBJECT_LAYOUT_DIAGNOSTICS\n'
        '    struct { uint64_t plans, sites, fields, bytes, shapes, objects, initialized; } object_layout_stats;\n'
        '#endif\n\n    /* Shape hash table */\n')
    source = once(source, 'typedef struct JSFunctionBytecode {\n', TYPES + '\ntypedef struct JSFunctionBytecode {\n')
    source = once(source, '    int byte_code_len;\n', '    int byte_code_len;\n    JSObjectLayoutPlan *object_layout;\n')
    source = once(source, '} OPCodeEnum;\n', '} OPCodeEnum;\n\n#define OP_object_layout 254\n#define OP_init_object_field 255\n'
        '_Static_assert(OP_COUNT <= OP_object_layout, "private object opcodes must be unused");\n'
        '#ifndef JS_DEBUG_TRACE_DEBUGGER_STMT\n'
        '_Static_assert(OP_COUNT <= 253, "default dispatch range must be nonempty");\n'
        '#endif\n')
    source = once(source, 'static bool needs_backtrace(JSValue exc)\n', HELPERS + '\nstatic bool needs_backtrace(JSValue exc)\n')
    source = once(source, '        [ OP_COUNT ... 255 ] = &&case_default\n',
        '#if !defined(JS_DEBUG_TRACE_DEBUGGER_STMT)\n'
        '        [ OP_COUNT ... 253 ] = &&case_default,\n#endif\n'
        '        [ OP_object_layout ] = &&case_OP_object_layout,\n'
        '        [ OP_init_object_field ] = &&case_OP_init_object_field\n')
    source = once(source, '        CASE(OP_object):\n', r'''
        CASE(OP_object_layout):
            if (unlikely(!b->object_layout))
                goto object_layout_invalid_opcode;
            *sp++ = js_object_layout_new(ctx, b, pc - b->byte_code_buf - 1);
            if (unlikely(JS_IsException(sp[-1])))
                goto exception;
            BREAK;
        CASE(OP_init_object_field):
            {
                JSObject *p;
                uint32_t slot = get_u32(pc);
                if (unlikely(!b->object_layout))
                    goto object_layout_invalid_opcode;
                p = JS_VALUE_GET_OBJ(sp[-2]);
                pc += 4;
                assert(slot < p->shape->prop_count);
                assert(JS_IsUndefined(p->prop[slot].u.value));
                p->prop[slot].u.value = sp[-1];
                sp--;
            }
            BREAK;
        CASE(OP_object):
''')
    source = once(source, '        CASE(OP_invalid):\n        DEFAULT:\n',
                  '        CASE(OP_invalid):\n        DEFAULT:\n        object_layout_invalid_opcode:\n')
    source = once(source, '    js_free(ctx, fd);\n    return JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);',
        '    js_object_layout_prepare(ctx->rt, b);\n    js_free(ctx, fd);\n    return JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);')
    source = once(source, ' nodebug:\n    b->realm = JS_DupContext(ctx);\n    return obj;',
        ' nodebug:\n    b->realm = JS_DupContext(ctx);\n    js_object_layout_prepare(ctx->rt, b);\n    return obj;')
    source = once(source, '            if (b->realm)\n                mark_func(rt, &b->realm->header);',
        '            if (b->object_layout) {\n'
        '                uint32_t j;\n'
        '                for (j = 0; j < b->object_layout->site_count; j++) {\n'
        '                    JSShape *sh = b->object_layout->sites[j].shape;\n'
        '                    if (sh)\n                        mark_func(rt, &sh->header);\n'
        '                }\n            }\n'
        '            if (b->realm)\n                mark_func(rt, &b->realm->header);')
    source = once(source, 'static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b)\n{\n    int i;\n',
        'static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b)\n{\n    int i;\n'
        '    if (b->object_layout) {\n'
        '        js_object_layout_restore(b->object_layout, b->byte_code_buf);\n'
        '        for (i = 0; i < b->object_layout->site_count; i++)\n'
        '            js_free_shape_null(rt, b->object_layout->sites[i].shape);\n'
        '        js_free_rt(rt, b->object_layout);\n        b->object_layout = NULL;\n    }\n')
    source = once(source, '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n',
        '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n'
        '    if (b->object_layout)\n        js_object_layout_restore(b->object_layout, bc_buf);\n')
    source = once(source, '    hp->js_func_size += js_func_size;\n',
        '    if (b->object_layout) {\n        memory_used_count++;\n'
        '        js_func_size += b->object_layout->bytes;\n    }\n'
        '    hp->js_func_size += js_func_size;\n')
    source = once(source, 'void JS_FreeRuntime(JSRuntime *rt)\n{\n',
        'void JS_FreeRuntime(JSRuntime *rt)\n{\n'
        '#ifdef QJS_OBJECT_LAYOUT_DIAGNOSTICS\n'
        '    fprintf(stderr, "OBJECT_LAYOUT plans=%llu sites=%llu fields=%llu bytes=%llu shapes=%llu objects=%llu initialized=%llu\\n",\n'
        '        (unsigned long long)rt->object_layout_stats.plans, (unsigned long long)rt->object_layout_stats.sites,\n'
        '        (unsigned long long)rt->object_layout_stats.fields, (unsigned long long)rt->object_layout_stats.bytes,\n'
        '        (unsigned long long)rt->object_layout_stats.shapes, (unsigned long long)rt->object_layout_stats.objects,\n'
        '        (unsigned long long)rt->object_layout_stats.initialized);\n#endif\n')
    return source
