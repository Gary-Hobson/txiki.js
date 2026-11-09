"""Fuse comparison results into the following canonical conditional branch."""


def transform(source):
    anchor = ('/* argv[] is modified if (flags & JS_CALL_FLAG_COPY_ARGV) = 0. */\n'
              'static JSValue JS_CallInternal(')
    helper = '''/* Comparisons produce canonical booleans. Consume the adjacent branch
   without another dispatch; preserve its interrupt poll at the caller. */
static inline bool js_take_comparison_branch(bool value, uint8_t **ppc)
{
#if defined(ENABLE_DUMPS) || defined(JS_DEBUG_TRACE_DEBUGGER_STMT)
    return false;
#else
    uint8_t *pc = *ppc;
    unsigned int op = *pc;
    if (op - OP_if_false8 <= 1) {
        *ppc = pc + 1 + (value == (op - OP_if_false8)
                        ? (int8_t)pc[1] : 1);
        return true;
    }
    if (op - OP_if_false <= 1) {
        *ppc = pc + 1 + (value == (op - OP_if_false)
                        ? (int32_t)get_u32(pc + 1) : 4);
        return true;
    }
    return false;
#endif
}

'''
    assert source.count(anchor) == 1
    source = source.replace(anchor, helper + anchor)
    begin = source.index('#define OP_CMP(')
    end = source.index('            OP_CMP(OP_lt', begin)
    macro = source[begin:end]
    assert macro.count('            BREAK') == 1
    additions = [
        '                if (js_take_comparison_branch(JS_VALUE_GET_INT(sp[-1]), &pc)) {',
        '                    sp--;',
        '                    if (unlikely(js_poll_interrupts(ctx)))',
        '                        goto exception;',
        '                }',
    ]
    fused = ''.join(line + ' \\\n' for line in additions)
    macro = macro.replace('            BREAK', fused + '            BREAK')
    return source[:begin] + macro + source[end:]
