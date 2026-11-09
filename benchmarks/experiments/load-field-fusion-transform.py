def transform(source):
    anchor='/* Release a counted reference an open var_ref held on the coroutine\n'
    helper='''/* Borrow the local receiver while executing an adjacent plain own-field
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
    assert source.count(anchor)==1
    source=source.replace(anchor,helper+anchor)
    for op,expr in [('OP_get_loc8','var_buf[*pc++]')]+[(f'OP_get_loc{i}',f'var_buf[{i}]') for i in range(4)]+[(f'OP_get_arg{i}',f'arg_buf[{i}]') for i in range(4)]:
        old=f'CASE({op}): *sp++ = js_dup({expr}); BREAK;'
        new=f'CASE({op}): *sp++ = js_load_local_or_field({expr}, &pc); BREAK;'
        assert source.count(old)==1,old
        source=source.replace(old,new)
    for op,nextop,buf in [('OP_get_loc','OP_put_loc','var_buf'),('OP_get_arg','OP_put_arg','arg_buf')]:
        start=source.index(f'        CASE({op}):')
        end=source.index(f'        CASE({nextop}):',start)
        body=source[start:end]
        old=f'js_dup({buf}[idx])'
        assert body.count(old)==1
        source=source[:start]+body.replace(old,f'js_load_local_or_field({buf}[idx], &pc)')+source[end:]
    return source
