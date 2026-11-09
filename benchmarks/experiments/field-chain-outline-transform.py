"""Keep the existing one-field inline path and outline longer borrowed chains."""


def transform(source):
    marker = '/* Borrow the local receiver while executing an adjacent plain own-field\n'
    start = source.index(marker)
    end = source.index('\n}\n', start) + 3
    old = source[start:end]
    needle = '''            *ppc = pc + 5;
            return js_dup(pr->u.value);'''
    assert old.count(needle) == 1
    tail = '''/* Only longer chains enter this helper. Keep the loop out of the many
   inline local/argument load handlers in the main interpreter. */
static no_inline JSValue js_load_field_tail(JSValueConst value, uint8_t **ppc)
{
    uint8_t *pc = *ppc;
    JSProperty *pr;
    JSShapeProperty *prs;

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
    new = old.replace(needle, '''            *ppc = pc + 5;
            if (pc[5] == OP_get_field &&
                JS_VALUE_GET_TAG(pr->u.value) == JS_TAG_OBJECT)
                return js_load_field_tail(pr->u.value, ppc);
            return js_dup(pr->u.value);''')
    return source[:start] + tail + new + source[end:]
