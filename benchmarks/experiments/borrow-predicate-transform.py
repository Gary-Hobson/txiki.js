"""Portable, hot-function borrowed predicate regions; no machine code.

Apply after property-hint and/or load-field-fusion if composing. The canonical
bytecode stays unchanged. A per-function decoded map owns no JS/atom references.
Only complete readonly field-chain / strict-comparison regions are executed.
"""


HELPERS = r'''
typedef struct JSBorrowPredicateRecipe {
    uint16_t right_offset;
    uint16_t compare_offset;
    uint8_t left_fields;
    uint8_t right_fields;
    uint8_t right_size;
    uint8_t is_neq;
} JSBorrowPredicateRecipe;

typedef struct JSBorrowPredicateTable {
    uint16_t *map;
    JSBorrowPredicateRecipe *recipes;
} JSBorrowPredicateTable;

static const uint8_t js_borrow_opcode_sizes[OP_COUNT] = {
#define DEF(id, size, n_pop, n_push, f) size,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
};

static int js_borrow_load_size(int op, bool local_only)
{
    switch (op) {
    case OP_get_loc: case OP_get_arg:
        return 3;
    case OP_get_loc8:
        return 2;
    case OP_get_loc0: case OP_get_loc1:
    case OP_get_loc2: case OP_get_loc3:
    case OP_get_arg0: case OP_get_arg1:
    case OP_get_arg2: case OP_get_arg3:
        return 1;
    default:
        if (local_only)
            return 0;
        switch (op) {
        case OP_push_const: case OP_push_atom_value: case OP_push_i32:
            return 5;
        case OP_push_i16:
            return 3;
        case OP_push_const8: case OP_push_i8:
            return 2;
        case OP_push_minus1: case OP_push_0: case OP_push_1:
        case OP_push_2: case OP_push_3: case OP_push_4:
        case OP_push_5: case OP_push_6: case OP_push_7:
        case OP_null: case OP_undefined: case OP_push_false:
        case OP_push_true:
            return 1;
        default:
            return 0;
        }
    }
}

static bool js_borrow_decode(JSFunctionBytecode *b, int pos,
                             JSBorrowPredicateRecipe *r)
{
    const uint8_t *code = b->byte_code_buf;
    int len = b->byte_code_len, n;
    r->left_fields = r->right_fields = 0;
    while (pos < len && code[pos] == OP_get_field && r->left_fields < 4) {
        if (pos + 5 > len)
            return false;
        pos += 5;
        r->left_fields++;
    }
    if (pos >= len)
        return false;
    r->right_offset = pos;
    n = js_borrow_load_size(code[pos], false);
    if (!n || pos + n > len)
        return false;
    r->right_size = n;
    pos += n;
    while (pos < len && code[pos] == OP_get_field && r->right_fields < 4) {
        if (pos + 5 > len)
            return false;
        pos += 5;
        r->right_fields++;
    }
    if (!(r->left_fields + r->right_fields) || pos >= len ||
        (code[pos] != OP_strict_eq && code[pos] != OP_strict_neq))
        return false;
    r->compare_offset = pos;
    r->is_neq = code[pos] == OP_strict_neq;
    return true;
}

/* Optional optimization allocation must not create a JS exception. */
static void js_borrow_prepare(JSRuntime *rt, JSFunctionBytecode *b)
{
    JSBorrowPredicateTable *t;
    JSBorrowPredicateRecipe r;
    const uint8_t *code = b->byte_code_buf;
    size_t map_bytes, bytes;
    int pass, pos, size, count = 0, slot;
    if (b->byte_code_len <= 0 || b->byte_code_len > UINT16_MAX)
        return;
    for (pass = 0; pass < 2; pass++) {
        slot = 0;
        for (pos = 0; pos < b->byte_code_len; pos += size) {
            if (code[pos] >= OP_COUNT)
                return;
            size = js_borrow_opcode_sizes[code[pos]];
            if (!size || pos + size > b->byte_code_len)
                return;
            if (js_borrow_load_size(code[pos], true) &&
                js_borrow_decode(b, pos + size, &r)) {
                if (pass) {
                    t->map[pos + size] = slot + 1;
                    t->recipes[slot] = r;
                }
                slot++;
            }
        }
        if (pass)
            break;
        count = slot;
        if (!count)
            return;
        map_bytes = ((size_t)b->byte_code_len * sizeof(uint16_t) + 7) & ~(size_t)7;
        bytes = sizeof(*t) + map_bytes + count * sizeof(*t->recipes);
        t = js_mallocz_rt(rt, bytes);
        if (!t)
            return;
        t->map = (uint16_t *)(t + 1);
        t->recipes = (JSBorrowPredicateRecipe *)((uint8_t *)t->map + map_bytes);
        b->borrow_predicates = t;
    }
}

/* Borrowed only while every owner and all fields remain untouched. */
static bool js_borrow_fields(JSValue *value, const uint8_t *pc, int count)
{
    JSObject *obj;
    JSProperty *pr;
    JSShapeProperty *prs;
    while (count--) {
        if (JS_VALUE_GET_TAG(*value) != JS_TAG_OBJECT)
            return false;
        obj = JS_VALUE_GET_OBJ(*value);
        if (obj->class_id != JS_CLASS_OBJECT || obj->is_exotic)
            return false;
        prs = find_own_property(&pr, obj, get_u32(pc + 1));
        if (!prs || (prs->flags & JS_PROP_TMASK))
            return false;
        *value = pr->u.value;
        pc += 5;
    }
    return true;
}

static bool js_borrow_right(JSContext *ctx, JSFunctionBytecode *b,
                            const uint8_t *pc, JSValue *locals,
                            JSValue *args, JSValue *out)
{
    uint32_t index;
    JSAtom atom;
    JSAtomStruct *str;
    int op = *pc++;
    switch (op) {
    case OP_get_loc: *out = locals[get_u16(pc)]; return true;
    case OP_get_arg: *out = args[get_u16(pc)]; return true;
    case OP_get_loc8: *out = locals[*pc]; return true;
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
        *out = locals[op - OP_get_loc0]; return true;
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        *out = args[op - OP_get_arg0]; return true;
    case OP_push_const: case OP_push_const8:
        index = op == OP_push_const ? get_u32(pc) : *pc;
        if (index >= (uint32_t)b->cpool_count) return false;
        *out = b->cpool[index]; return true;
    case OP_push_atom_value:
        atom = get_u32(pc);
        /* Integer atoms would allocate a decimal string. */
        if (__JS_AtomIsTaggedInt(atom)) return false;
        str = ctx->rt->atom_array[atom];
        *out = JS_MKPTR(str->atom_type == JS_ATOM_TYPE_STRING ?
                       JS_TAG_STRING : JS_TAG_SYMBOL, str);
        return true;
    case OP_push_i32: *out = js_int32(get_u32(pc)); return true;
    case OP_push_i16: *out = js_int32(get_i16(pc)); return true;
    case OP_push_i8: *out = js_int32(get_i8(pc)); return true;
    case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2:
    case OP_push_3: case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
        *out = js_int32(op - OP_push_0); return true;
    case OP_null: *out = JS_NULL; return true;
    case OP_undefined: *out = JS_UNDEFINED; return true;
    case OP_push_false: *out = JS_FALSE; return true;
    case OP_push_true: *out = JS_TRUE; return true;
    default: return false;
    }
}

static bool js_borrow_eq_tag(JSValue value)
{
    switch (JS_VALUE_GET_NORM_TAG(value)) {
    case JS_TAG_INT: case JS_TAG_FLOAT64: case JS_TAG_BOOL:
    case JS_TAG_NULL: case JS_TAG_UNDEFINED: case JS_TAG_OBJECT:
    case JS_TAG_SYMBOL: case JS_TAG_STRING:
        return true;
    default:
        /* Keep rope, BigInt and internal sentinel paths in Tier 0. */
        return false;
    }
}

static inline JSValue js_load_borrow_predicate(JSContext *ctx,
                                               JSFunctionBytecode *b,
                                               JSValueConst initial,
                                               uint8_t **ppc,
                                               JSValue *locals, JSValue *args)
{
    JSBorrowPredicateTable *t = b->borrow_predicates;
    JSBorrowPredicateRecipe *r;
    JSValue left, right;
    uint32_t pos;
    uint16_t site;
#ifndef ENABLE_DUMPS
    if (t) {
        pos = (uint32_t)(*ppc - b->byte_code_buf);
        if (pos < (uint32_t)b->byte_code_len && (site = t->map[pos]) != 0) {
            r = &t->recipes[site - 1];
            left = unsafe_unconst(initial);
            if (js_borrow_fields(&left, *ppc, r->left_fields) &&
                js_borrow_right(ctx, b, b->byte_code_buf + r->right_offset,
                                locals, args, &right) &&
                js_borrow_fields(&right, b->byte_code_buf + r->right_offset +
                                 r->right_size, r->right_fields) &&
                js_borrow_eq_tag(left) && js_borrow_eq_tag(right)) {
                bool equal = js_strict_eq2(ctx, left, right, JS_EQ_STRICT);
                *ppc = b->byte_code_buf + r->compare_offset + 1;
                return js_bool(equal ^ r->is_neq);
            }
        }
    }
#endif
    return BORROW_FALLBACK;
}

'''


def transform(source):
    if 'js_load_borrow_predicate' in source:
        raise ValueError('borrow-predicate already applied')
    combined = 'js_load_local_or_field' in source
    declaration = 'typedef struct JSFunctionBytecode {\n'
    assert source.count(declaration) == 1
    source = source.replace(declaration,
        'typedef struct JSBorrowPredicateTable JSBorrowPredicateTable;\n\n' + declaration)
    old = '    int byte_code_len;\n'
    assert source.count(old) == 1
    source = source.replace(old, old +
        '    JSBorrowPredicateTable *borrow_predicates;\n'
        '    uint16_t borrow_warmup;\n', 1)
    anchor = 'static bool needs_backtrace(JSValue exc)\n'
    assert source.count(anchor) == 1
    fallback = 'js_load_local_or_field(initial, ppc)' if combined else 'js_dup(initial)'
    source = source.replace(anchor, HELPERS.replace('BORROW_FALLBACK', fallback) + anchor)
    start = source.index('static JSValue JS_CallInternal(JSContext *caller_ctx')
    end = source.index('\nJSValue JS_Call(', start)
    body = source[start:end]
    anchor = '    b = p->u.func.function_bytecode;\n\n    if (unlikely(argc < b->arg_count'
    assert body.count(anchor) == 1
    body = body.replace(anchor,
        '    b = p->u.func.function_bytecode;\n'
        '#ifndef ENABLE_DUMPS\n'
        '    if (unlikely(b->borrow_warmup < 32) && ++b->borrow_warmup == 32)\n'
        '        js_borrow_prepare(rt, b);\n'
        '#endif\n\n    if (unlikely(argc < b->arg_count')
    expressions = ['var_buf[*pc++]'] + [f'var_buf[{i}]' for i in range(4)] + [f'arg_buf[{i}]' for i in range(4)]
    for expr in expressions:
        old = f'js_load_local_or_field({expr}, &pc)' if combined else f'js_dup({expr})'
        # get_loc0_loc1 also has js_dup(var_buf[0/1]); change only the one-line handlers.
        name = 'OP_get_loc8' if expr == 'var_buf[*pc++]' else ('OP_get_loc' if expr.startswith('var') else 'OP_get_arg') + expr[-2]
        line = f'CASE({name}): *sp++ = {old}; BREAK;'
        assert body.count(line) == 1, line
        replacement = f'js_load_borrow_predicate(ctx, b, {expr}, &pc, var_buf, arg_buf)'
        body = body.replace(line, line.replace(old, replacement))
    for op, nextop, buf in [('OP_get_loc','OP_put_loc','var_buf'), ('OP_get_arg','OP_put_arg','arg_buf')]:
        a = body.index(f'        CASE({op}):')
        z = body.index(f'        CASE({nextop}):', a)
        part = body[a:z]
        old = f'js_load_local_or_field({buf}[idx], &pc)' if combined else f'js_dup({buf}[idx])'
        assert part.count(old) == 1
        body = body[:a] + part.replace(old, f'js_load_borrow_predicate(ctx, b, {buf}[idx], &pc, var_buf, arg_buf)') + body[z:]
    source = source[:start] + body + source[end:]
    anchor = 'static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b)\n{\n    int i;\n'
    assert source.count(anchor) == 1
    source = source.replace(anchor, anchor + '\n    js_free_rt(rt, b->borrow_predicates);\n')
    return source
