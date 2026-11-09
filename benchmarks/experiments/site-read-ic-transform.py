"""Hashed-shape, two-way, per-bytecode-site own-data read IC.

Apply to integrated hint+fusion (or baseline). Lazy in-place quickening keeps
every active/suspended PC base stable. Canonical bytes own the original atoms.
No shape references are retained; new/clone reset lazy layout identity.
"""


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:140])
    return s.replace(old, new, 1)


TYPES = r'''
typedef struct JSReadICSite {
    uint64_t layout[2];
    uint32_t slot[2];
    JSAtom atom;
    uint8_t misses;
    uint8_t victim;
    uint8_t original_op;
    uint8_t reserved;
} JSReadICSite;

typedef struct JSReadICPlan {
    uint8_t *canonical;
    size_t bytes;
    uint32_t site_count;
    JSReadICSite sites[];
} JSReadICPlan;

#ifdef QJS_READ_IC_DIAGNOSTICS
#define READ_IC_STAT(rt, key, n) ((rt)->read_ic_stats.key += (n))
#else
#define READ_IC_STAT(rt, key, n) ((void)0)
#endif

'''


HELPERS = r'''
static const uint8_t js_read_ic_sizes[OP_COUNT] = {
#define DEF(id, size, n_pop, n_push, f) size,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
};

static bool js_read_ic_is_fused_load(int op)
{
    switch (op) {
    case OP_get_loc: case OP_get_arg: case OP_get_loc8:
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        return true;
    default: return false;
    }
}

/* Runtime allocation deliberately does not set a JS exception on failure.
   No allocations/callbacks follow publication; operand then opcode is patched.
   Code storage is the writable self buffer in both compiler and reader. */
static void js_read_ic_prepare(JSRuntime *rt, JSFunctionBytecode *b)
{
    JSReadICPlan *plan;
    JSReadICSite *site;
    uint8_t *code = b->byte_code_buf;
    int pos, op, size, prev = -1;
    uint32_t count = 0, index = 0;
    size_t bytes;
#if defined(ENABLE_DUMPS) || defined(JS_DEBUG_TRACE_DEBUGGER_STMT) || \
    defined(QJS_DISABLE_READ_IC)
    return;
#endif
    if (b->read_ic || b->byte_code_len <= 0 || b->byte_code_len > UINT16_MAX)
        return;
    for (pos = 0; pos < b->byte_code_len; pos += size) {
        op = code[pos];
        if (op >= OP_COUNT || !(size = js_read_ic_sizes[op]) ||
            pos + size > b->byte_code_len)
            return;
        if (op == OP_get_field2 ||
            (op == OP_get_field && !READ_IC_PRESERVE_FUSION))
            count++;
        prev = op;
    }
    if (!count)
        return;
    bytes = sizeof(*plan) + count * sizeof(*site) + b->byte_code_len;
    plan = js_mallocz_rt(rt, bytes);
    if (!plan)
        return;
    plan->bytes = bytes;
    plan->site_count = count;
    plan->canonical = (uint8_t *)(plan->sites + count);
    memcpy(plan->canonical, code, b->byte_code_len);
    b->read_ic = plan;
    prev = -1;
    for (pos = 0; pos < b->byte_code_len; pos += size) {
        op = plan->canonical[pos];
        size = js_read_ic_sizes[op];
        if (op == OP_get_field2 ||
            (op == OP_get_field && !READ_IC_PRESERVE_FUSION)) {
            site = &plan->sites[index];
            site->atom = get_u32(code + pos + 1);
            site->original_op = op;
            put_u32(code + pos + 1, index++);
            code[pos] = op == OP_get_field ? OP_read_ic : OP_read_ic2;
        }
        prev = op;
    }
    READ_IC_STAT(rt, plans, 1);
    READ_IC_STAT(rt, sites, count);
    READ_IC_STAT(rt, bytes, bytes);
}

/* Destructive descriptor/prototype changes first make the shape unhashed.
   Hashed append only extends the tail; existing own-data slots remain valid.
   New/clone reset id; resize preserves id; no cache holds an address or ref. */
static inline JSProperty *js_read_ic_hit(JSRuntime *rt, JSReadICSite *site,
                                        JSObject *p)
{
    JSShape *sh = p->shape;
    uint64_t id;
    if (p->class_id != JS_CLASS_OBJECT || p->is_exotic)
        return NULL;
    if (!sh->is_hashed) {
        READ_IC_STAT(rt, unhashed, 1);
        return NULL;
    }
    if (!(id = sh->read_ic_layout_id))
        return NULL;
    if (site->layout[0] == id) {
#ifdef QJS_READ_IC_VALIDATE
        assert(site->slot[0] < (uint32_t)sh->prop_count);
        assert(get_shape_prop(sh)[site->slot[0]].atom == site->atom);
        assert(!(get_shape_prop(sh)[site->slot[0]].flags & JS_PROP_TMASK));
#endif
        READ_IC_STAT(rt, hits, 1);
        return &p->prop[site->slot[0]];
    }
    if (site->layout[1] == id) {
#ifdef QJS_READ_IC_VALIDATE
        assert(site->slot[1] < (uint32_t)sh->prop_count);
        assert(get_shape_prop(sh)[site->slot[1]].atom == site->atom);
        assert(!(get_shape_prop(sh)[site->slot[1]].flags & JS_PROP_TMASK));
#endif
        READ_IC_STAT(rt, hits, 1);
        return &p->prop[site->slot[1]];
    }
    return NULL;
}

/* Called only after a current, ordinary own-data descriptor was found. */
static void js_read_ic_fill(JSRuntime *rt, JSReadICSite *site,
                            JSObject *p, JSProperty *pr)
{
    JSShape *sh = p->shape;
    uint32_t slot;
    unsigned way;
    if (p->class_id != JS_CLASS_OBJECT || p->is_exotic || !sh->is_hashed)
        return;
    if (!sh->read_ic_layout_id) {
        if (rt->read_ic_next_layout_id == UINT64_MAX)
            return; /* No wrap/ABA. Existing identities remain valid. */
        sh->read_ic_layout_id = ++rt->read_ic_next_layout_id;
        READ_IC_STAT(rt, layouts, 1);
    }
    slot = (uint32_t)(pr - p->prop);
    if (site->layout[0] == sh->read_ic_layout_id ||
        site->layout[1] == sh->read_ic_layout_id)
        return;
    way = !site->layout[0] ? 0 : !site->layout[1] ? 1 : site->victim;
    site->layout[way] = sh->read_ic_layout_id;
    site->slot[way] = slot;
    site->victim = way ^ 1;
}

static inline void js_read_ic_miss(JSRuntime *rt, JSReadICSite *site,
                                   uint8_t *opcode_pc)
{
    READ_IC_STAT(rt, misses, 1);
    if (++site->misses == 32) {
        /* Retire unstable/missing/exotic sites. No ongoing map/IC tax. */
        put_u32(opcode_pc + 1, site->atom);
        *opcode_pc = site->original_op;
        READ_IC_STAT(rt, retired, 1);
    }
}

'''


def transform(source):
    assert 'JSReadICPlan' not in source
    # Both supported creation paths copy into writable JSFunctionBytecode storage.
    assert source.count('b->byte_code_buf = (void *)((uint8_t*)b + byte_code_offset);') == 1
    assert source.count('    if (bc_get_buf(s, bc_buf, bc_len))\n        return -1;\n    b->byte_code_buf = bc_buf;') == 1
    assert source.count('b->byte_code_buf =') == 2
    combined = 'js_load_local_or_field' in source
    source = once(source, '    /* Shape hash table */\n',
        '    uint64_t read_ic_next_layout_id;\n'
        '#ifdef QJS_READ_IC_DIAGNOSTICS\n'
        '    struct { uint64_t plans, sites, bytes, layouts, calls, hits, misses, unhashed, retired; } read_ic_stats;\n'
        '#endif\n\n    /* Shape hash table */\n')
    source = once(source, 'typedef struct JSFunctionBytecode {\n',
                  TYPES + 'typedef struct JSFunctionBytecode {\n')
    source = once(source, '    int byte_code_len;\n',
        '    int byte_code_len;\n    JSReadICPlan *read_ic;\n    uint8_t read_ic_warmup;\n')
    source = once(source, '    JSObject *proto;\n    uint32_t hash_table[];',
        '    JSObject *proto;\n    uint64_t read_ic_layout_id;\n    uint32_t hash_table[];')
    source = once(source, '    sh->deleted_prop_count = 0;\n    sh->is_hashed = false;',
        '    sh->deleted_prop_count = 0;\n    sh->read_ic_layout_id = 0;\n    sh->is_hashed = false;')
    source = once(source, '    sh->is_hashed = false;\n    if (sh->proto) {',
        '    sh->is_hashed = false;\n    sh->read_ic_layout_id = 0;\n    if (sh->proto) {')
    source = once(source, '} OPCodeEnum;\n',
        '} OPCodeEnum;\n\n#define OP_read_ic 254\n#define OP_read_ic2 255\n'
        '_Static_assert(OP_COUNT <= OP_read_ic, "private read IC opcodes must be unused");\n'
        '#ifndef JS_DEBUG_TRACE_DEBUGGER_STMT\n'
        '_Static_assert(OP_COUNT <= 253, "default dispatch range must be nonempty");\n'
        '#endif\n')
    preserve = 'js_read_ic_is_fused_load(prev)' if combined else 'false'
    source = once(source, 'static bool needs_backtrace(JSValue exc)\n',
        HELPERS.replace('READ_IC_PRESERVE_FUSION', preserve) + 'static bool needs_backtrace(JSValue exc)\n')
    # This is the normal entry and the coroutine resume entry, both before execution.
    source = once(source, '    b = p->u.func.function_bytecode;\n\n    if (unlikely(argc < b->arg_count',
        '    b = p->u.func.function_bytecode;\n'
        '#if !defined(ENABLE_DUMPS) && !defined(JS_DEBUG_TRACE_DEBUGGER_STMT) && !defined(QJS_DISABLE_READ_IC)\n'
        '    if (unlikely(b->read_ic_warmup < 32) && ++b->read_ic_warmup == 32)\n'
        '        js_read_ic_prepare(rt, b);\n'
        '#endif\n\n    if (unlikely(argc < b->arg_count')
    source = once(source, '            b = p->u.func.function_bytecode;\n            ctx = b->realm;',
        '            b = p->u.func.function_bytecode;\n'
        '#if !defined(ENABLE_DUMPS) && !defined(JS_DEBUG_TRACE_DEBUGGER_STMT) && !defined(QJS_DISABLE_READ_IC)\n'
        '            if (unlikely(b->read_ic_warmup < 32) && ++b->read_ic_warmup == 32)\n'
        '                js_read_ic_prepare(rt, b);\n'
        '#endif\n            ctx = b->realm;')
    source = once(source, '        [ OP_COUNT ... 255 ] = &&case_default\n',
        '#if !defined(JS_DEBUG_TRACE_DEBUGGER_STMT)\n'
        '        [ OP_COUNT ... 253 ] = &&case_default,\n'
        '#endif\n'
        '        [ OP_read_ic ] = &&case_OP_read_ic,\n'
        '        [ OP_read_ic2 ] = &&case_OP_read_ic2\n')
    # Clone original handlers so untouched canonical handlers pay no opcode test.
    for op, nxt, private in [('OP_get_field','OP_get_field2','OP_read_ic'),
                             ('OP_get_field2','OP_put_field','OP_read_ic2')]:
        start = source.index(f'        CASE({op}):\n')
        end = source.index(f'        CASE({nxt}):\n', start)
        old = source[start:end]
        body = old.replace(f'CASE({op}):', f'CASE({private}):')
        body = body.replace(op[3:] + '_slow_path', private[3:] + '_slow_path')
        body = once(body, '                JSShapeProperty *prs;\n',
                    '                JSShapeProperty *prs;\n                JSReadICSite *site;\n                uint32_t site_index;\n')
        body = once(body, '                atom = get_u32(pc);\n                pc += 4;\n',
            '                site_index = get_u32(pc);\n'
            '                if (!b->read_ic || site_index >= b->read_ic->site_count)\n'
            '                    goto read_ic_invalid_opcode;\n'
            '                site = &b->read_ic->sites[site_index];\n'
            '                atom = site->atom;\n                pc += 4;\n'
            '                READ_IC_STAT(rt, calls, 1);\n'
            '                obj = sp[-1];\n'
            '                if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT &&\n'
            '                    (pr = js_read_ic_hit(rt, site, JS_VALUE_GET_OBJ(obj)))) {\n'
            '                    val = js_dup(pr->u.value);\n'
            f'                    goto {private[3:]}_done;\n'
            '                }\n'
            '                js_read_ic_miss(rt, site, pc - 5);\n')
        body = once(body, '                            val = js_dup(pr->u.value);\n',
            '                            if (p == JS_VALUE_GET_OBJ(obj))\n'
            '                                js_read_ic_fill(rt, site, p, pr);\n'
            '                            val = js_dup(pr->u.value);\n')
        finish = '                JS_FreeValue(ctx, sp[-1]);\n' if op=='OP_get_field' else '                *sp++ = val;\n'
        body = once(body, finish, f'            {private[3:]}_done:\n'+finish)
        source = source[:start] + body + source[start:]
    source = once(source, '        CASE(OP_invalid):\n        DEFAULT:\n',
                  '        CASE(OP_invalid):\n        DEFAULT:\n        read_ic_invalid_opcode:\n')
    source = once(source, '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n',
        '    memcpy(bc_buf, b->read_ic ? b->read_ic->canonical : b->byte_code_buf, bc_len);\n')
    source = once(source, 'static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b)\n{\n    int i;\n',
        'static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b)\n{\n    int i;\n\n'
        '    if (b->read_ic) {\n'
        '        memcpy(b->byte_code_buf, b->read_ic->canonical, b->byte_code_len);\n'
        '        js_free_rt(rt, b->read_ic);\n        b->read_ic = NULL;\n    }\n')
    source = once(source, '    hp->js_func_size += js_func_size;\n',
        '    if (b->read_ic) {\n        memory_used_count++;\n'
        '        js_func_size += b->read_ic->bytes;\n    }\n'
        '    hp->js_func_size += js_func_size;\n')
    source = once(source, 'void JS_FreeRuntime(JSRuntime *rt)\n{\n',
        'void JS_FreeRuntime(JSRuntime *rt)\n{\n'
        '#ifdef QJS_READ_IC_DIAGNOSTICS\n'
        '    fprintf(stderr, "QJS_READ_IC_STATS plans=%" PRIu64 " sites=%" PRIu64\n'
        '            " bytes=%" PRIu64 " layouts=%" PRIu64 " calls=%" PRIu64\n'
        '            " hits=%" PRIu64 " misses=%" PRIu64 " unhashed=%" PRIu64 " retired=%" PRIu64 "\\n",\n'
        '            rt->read_ic_stats.plans, rt->read_ic_stats.sites, rt->read_ic_stats.bytes,\n'
        '            rt->read_ic_stats.layouts, rt->read_ic_stats.calls, rt->read_ic_stats.hits,\n'
        '            rt->read_ic_stats.misses, rt->read_ic_stats.unhashed, rt->read_ic_stats.retired);\n'
        '#endif\n')
    return source
