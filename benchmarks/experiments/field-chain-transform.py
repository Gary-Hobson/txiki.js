"""Borrow complete plain own-field chains from the existing owned local root."""


def transform(source, diagnostic=False):
    old = '''/* Borrow the local receiver while executing an adjacent plain own-field
   load. No allocation or JavaScript reentry occurs on this fused path. */
static inline JSValue js_load_local_or_field(JSValueConst value,
                                             uint8_t **ppc)
{
    uint8_t *pc = *ppc;
    JSProperty *pr;
    JSShapeProperty *prs;
#ifdef ENABLE_DUMPS
    return js_dup(value);
#endif
    if (*pc == OP_get_field && JS_VALUE_GET_TAG(value) == JS_TAG_OBJECT) {
        prs = find_own_property(&pr, JS_VALUE_GET_OBJ(value), get_u32(pc + 1));
        if (prs && !(prs->flags & JS_PROP_TMASK)) {
            *ppc = pc + 5;
            return js_dup(pr->u.value);
        }
    }
    return js_dup(value);
}
'''
    new = '''/* The local or argument keeps the root and each plain own-field edge alive. Borrow
   the whole chain until a potentially observable property access is reached. */
static inline JSValue js_load_local_or_field(JSValueConst value,
                                             uint8_t **ppc)
{
    uint8_t *pc = *ppc;
    JSProperty *pr;
    JSShapeProperty *prs;
#ifdef ENABLE_DUMPS
    return js_dup(value);
#endif
    while (*pc == OP_get_field && JS_VALUE_GET_TAG(value) == JS_TAG_OBJECT) {
        prs = find_own_property(&pr, JS_VALUE_GET_OBJ(value), get_u32(pc + 1));
        if (!prs || (prs->flags & JS_PROP_TMASK))
            break;
        value = pr->u.value;
        pc += 5;
    }
    *ppc = pc;
    return js_dup(value);
}
'''
    assert source.count(old) == 1
    if diagnostic:
        new = 'static _Thread_local uint64_t field_chain_depths[9], field_chain_extra, field_chain_dispatches;\n' + new
        new = new.replace('    JSProperty *pr;', '    unsigned depth = 0;\n    JSProperty *pr;')
        new = new.replace('        pc += 5;', '        pc += 5;\n        depth++;')
        new = new.replace('    *ppc = pc;', '    field_chain_depths[depth < 8 ? depth : 8]++;\n    if (depth > 1) field_chain_extra += depth - 1;\n    *ppc = pc;')
        for old_dispatch, new_dispatch in [
            ('switch (opcode = *pc++)', 'switch ((field_chain_dispatches++, opcode = *pc++))'),
            ('__extension__ ({ goto *dispatch_table[opcode = *pc++]; });',
             '__extension__ ({ field_chain_dispatches++; goto *dispatch_table[opcode = *pc++]; });'),
        ]:
            assert source.count(old_dispatch) == 1
            source = source.replace(old_dispatch, new_dispatch)
        marker = 'void JS_RunGC(JSRuntime *rt)\n{\n'
        assert source.count(marker) == 1
        source = source.replace(marker, marker + '''    fprintf(stderr, "FIELD_CHAIN_STATS extra_fields=%" PRIu64, field_chain_extra);
    fprintf(stderr, " dispatches=%" PRIu64, field_chain_dispatches);
    for (int i = 0; i < 9; i++)
        fprintf(stderr, " depth%d=%" PRIu64, i, field_chain_depths[i]);
    fputc('\\n', stderr);
''')
    return source.replace(old, new)
