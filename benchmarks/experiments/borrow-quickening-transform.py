"""Entry-only revision of borrow-predicate; private execution bytecode copy.

Plans are created before a function can have an active frame, so byte_code_buf
and every saved PC always share the execution buffer base. Serialization and
atom destruction use the original canonical bytes. No published opcode changes.
"""
import importlib.util
import re
from pathlib import Path


def once(source, old, new):
    assert source.count(old) == 1, old[:160]
    return source.replace(old, new, 1)


def transform(original):
    path = Path(__file__).with_name('borrow-predicate-transform.py')
    spec = importlib.util.spec_from_file_location('borrow_predicate_base', path)
    base = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(base)
    combined = 'js_load_local_or_field' in original
    source = base.transform(original)
    source = once(source,
        '    uint16_t borrow_warmup;\n',
        '    uint8_t *borrow_canonical_code;\n    size_t borrow_plan_bytes;\n')
    source = once(source,
        '} OPCodeEnum;\n',
        '} OPCodeEnum;\n\n#define OP_borrow_predicate 255\n'
        '_Static_assert(OP_COUNT < OP_borrow_predicate, "private opcode must be unused");\n')
    source = once(source,
        '    JSBorrowPredicateRecipe *recipes;\n} JSBorrowPredicateTable;',
        '    JSBorrowPredicateRecipe *recipes;\n    uint8_t *exec_code;\n} JSBorrowPredicateTable;')
    source = once(source,
        '    int pass, pos, size, count = 0, slot;\n',
        '    int pass, pos, size, count = 0, slot;\n'
        '#if defined(ENABLE_DUMPS) || defined(JS_DEBUG_TRACE_DEBUGGER_STMT) || \\\n'
        '    defined(QJS_DISABLE_BORROW_QUICKENING)\n'
        '    /* The inspector macro comes from quickjs.h, independent of patch order. */\n'
        '    return;\n#endif\n')
    source = once(source,
        '                    t->recipes[slot] = r;\n',
        '                    t->recipes[slot] = r;\n'
        '                    t->exec_code[pos] = OP_borrow_predicate;\n')
    source = once(source,
        '        bytes = sizeof(*t) + map_bytes + count * sizeof(*t->recipes);\n',
        '        bytes = sizeof(*t) + map_bytes + count * sizeof(*t->recipes) +\n'
        '                b->byte_code_len;\n')
    source = once(source,
        '        b->borrow_predicates = t;\n    }\n}\n',
        '        t->exec_code = (uint8_t *)(t->recipes + count);\n'
        '        memcpy(t->exec_code, code, b->byte_code_len);\n'
        '        b->borrow_predicates = t;\n'
        '        b->borrow_canonical_code = b->byte_code_buf;\n'
        '        b->borrow_plan_bytes = bytes;\n'
        '    }\n'
        '    b->byte_code_buf = t->exec_code;\n'
        '}\n')
    # Right-hand producers must read original opcodes, never a private opcode.
    source = once(source,
        'js_borrow_right(ctx, b, b->byte_code_buf + r->right_offset,',
        'js_borrow_right(ctx, b, b->borrow_canonical_code + r->right_offset,')
    source = once(source,
        'js_borrow_fields(&right, b->byte_code_buf + r->right_offset +',
        'js_borrow_fields(&right, b->borrow_canonical_code + r->right_offset +')
    warm = ('#ifndef ENABLE_DUMPS\n'
            '    if (unlikely(b->borrow_warmup < 32) && ++b->borrow_warmup == 32)\n'
            '        js_borrow_prepare(rt, b);\n'
            '#endif\n')
    source = once(source, warm, '')
    pattern = r'js_load_borrow_predicate\(ctx, b, ([^,\n]+), &pc, var_buf, arg_buf\)'
    assert len(re.findall(pattern, source)) == 11
    source = re.sub(pattern,
        lambda m: f'js_load_local_or_field({m[1]}, &pc)' if combined else f'js_dup({m[1]})',
        source)
    source = once(source,
        '        [ OP_COUNT ... 255 ] = &&case_default\n',
        '        [ OP_COUNT ... 254 ] = &&case_default,\n'
        '        [ OP_borrow_predicate ] = &&case_OP_borrow_predicate\n')
    private_handler = r'''
        CASE(OP_borrow_predicate):
            {
                uint32_t pos = (uint32_t)(pc - b->byte_code_buf - 1);
                int size;
                JSValue initial;
                const uint8_t *canonical;
                /* Serialized input cannot select this internal opcode. */
                if (!b->borrow_predicates || !b->borrow_canonical_code ||
                    pos >= (uint32_t)b->byte_code_len)
                    goto borrow_invalid_opcode;
                canonical = b->borrow_canonical_code + pos;
                size = js_borrow_load_size(*canonical, true);
                if (!size || pos + size >= (uint32_t)b->byte_code_len ||
                    !b->borrow_predicates->map[pos + size] ||
                    !js_borrow_right(ctx, b, canonical, var_buf, arg_buf, &initial))
                    goto borrow_invalid_opcode;
                pc += size - 1;
                *sp++ = js_load_borrow_predicate(ctx, b, initial, &pc,
                                                var_buf, arg_buf);
            }
            BREAK;
'''
    source = once(source, '        CASE(OP_get_loc):\n',
                  private_handler + '        CASE(OP_get_loc):\n')
    source = once(source,
        '        CASE(OP_invalid):\n        DEFAULT:\n',
        '        CASE(OP_invalid):\n        DEFAULT:\n        borrow_invalid_opcode:\n')
    source = once(source,
        '    js_free(ctx, fd);\n    return JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);',
        '    js_borrow_prepare(ctx->rt, b);\n'
        '    js_free(ctx, fd);\n    return JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);')
    source = once(source,
        ' nodebug:\n    b->realm = JS_DupContext(ctx);\n    return obj;',
        ' nodebug:\n    b->realm = JS_DupContext(ctx);\n'
        '    js_borrow_prepare(ctx->rt, b);\n    return obj;')
    source = once(source,
        '    js_free_rt(rt, b->borrow_predicates);\n',
        '    if (b->borrow_canonical_code)\n'
        '        b->byte_code_buf = b->borrow_canonical_code;\n'
        '    js_free_rt(rt, b->borrow_predicates);\n'
        '    b->borrow_predicates = NULL;\n'
        '    b->borrow_plan_bytes = 0;\n')
    source = once(source,
        '    memcpy(bc_buf, b->byte_code_buf, bc_len);\n',
        '    memcpy(bc_buf, b->borrow_canonical_code ? b->borrow_canonical_code :\n'
        '                   b->byte_code_buf, bc_len);\n')
    source = once(source,
        '    hp->js_func_size += js_func_size;\n',
        '    if (b->borrow_predicates) {\n'
        '        memory_used_count++;\n'
        '        js_func_size += b->borrow_plan_bytes;\n'
        '    }\n'
        '    hp->js_func_size += js_func_size;\n')
    return source
