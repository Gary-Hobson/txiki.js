"""Cost control only: retain plans/handler/layout, optionally leave canonical entries.

Input is the unchanged producer-region candidate source. The process environment
QJS_PRODUCER_REGION_CANONICAL=1 selects canonical execution. With it absent, the
normal producer entry is installed. No wrapper or per-call environment lookup.
"""


def transform(source):
    old = '''    b->producer_regions = plan;
    for (slot = 0; slot < count; slot++) {
        uint8_t *entry = b->byte_code_buf + plan->sites[slot].pc;
        entry[0] = OP_producer_region;
        put_u16(entry + 1, slot);
    }
'''
    new = '''    b->producer_regions = plan;
    /* Experimental runtime gate: both modes allocate and fill the same plan.
       Read only once per prepared plan; normal call handlers see no new flag. */
    if (getenv("QJS_PRODUCER_REGION_CANONICAL") == NULL) {
        for (slot = 0; slot < count; slot++) {
            uint8_t *entry = b->byte_code_buf + plan->sites[slot].pc;
            entry[0] = OP_producer_region;
            put_u16(entry + 1, slot);
        }
    }
'''
    assert source.count(old) == 1, source.count(old)
    assert 'QJS_PRODUCER_REGION_CANONICAL' not in source
    return source.replace(old, new, 1)
