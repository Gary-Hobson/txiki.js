from pathlib import Path


def transform(src):
    old='typedef struct {\n    const uint8_t *cbuf;'
    assert src.count(old)==1
    src=src.replace(old,'''typedef struct {
    const uint8_t *search_anchor_pc;
    const uint8_t *cbuf;''')
    old='    re_flags = lre_get_flags(bc_buf);'
    assert src.count(old)==1
    src=src.replace(old,'''    s->search_anchor_pc = NULL;
    /* Recognize the existing unanchored search prelude. Only unconditional
       capture starts may precede the mandatory non-multiline anchor. */
    if (get_u32(bc_buf + RE_HEADER_BYTECODE_LEN) >= 14 &&
        bc_buf[RE_HEADER_LEN] == REOP_split_goto_first &&
        get_u32(bc_buf + RE_HEADER_LEN + 1) == 6 &&
        bc_buf[RE_HEADER_LEN + 5] == REOP_any &&
        bc_buf[RE_HEADER_LEN + 6] == REOP_goto &&
        (int32_t)get_u32(bc_buf + RE_HEADER_LEN + 7) == -11) {
        const uint8_t *head = bc_buf + RE_HEADER_LEN + 11;
        const uint8_t *end = bc_buf + RE_HEADER_LEN + get_u32(bc_buf + RE_HEADER_BYTECODE_LEN);
        while (end - head >= 3 && *head == REOP_save_start)
            head += 2;
        if (head < end && *head == REOP_line_start)
            s->search_anchor_pc = bc_buf + RE_HEADER_LEN;
    }
    re_flags = lre_get_flags(bc_buf);''')
    old='''                const uint8_t *pc1;

                val = get_u32(pc);'''
    assert src.count(old)==1
    src=src.replace(old,'''                const uint8_t *pc1;

                /* Keep the initial root frame so failure still restores all
                   captures. After that, ^ proves no later start can match. */
                if (pc - 1 == s->search_anchor_pc && cptr != s->cbuf)
                    goto no_match;
                val = get_u32(pc);''')
    return src

if __name__=='__main__':
    root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/regexp-analysis'
    out=root/'regexp-anchor';out.mkdir(exist_ok=True)
    (out/'libregexp.c').write_text(transform((root.parent/'integrated-object-layout/quickjs-patched/libregexp.c').read_text()))
