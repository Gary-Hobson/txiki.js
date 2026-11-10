"""Runtime-cached prepare bypass; both measured modes keep canonical entries.

Input is the archived runtime-gate candidate, whose private handler remains in
the binary. QJS_PRODUCER_REGION_CANONICAL=1 is required in both experiment modes.
QJS_PRODUCER_PREPARE_SKIP=1 skips all preparation; absent means prepared canonical.
"""


def once(source, old, new):
    assert source.count(old) == 1, (source.count(old), old[:100])
    return source.replace(old, new, 1)


def transform(source):
    assert 'QJS_PRODUCER_REGION_CANONICAL' in source
    assert 'producer_prepare_skip' not in source
    source = once(source, '    JSRuntimeFinalizerState *finalizers;\n',
                  '    JSRuntimeFinalizerState *finalizers;\n'
                  '    bool producer_prepare_skip; /* experimental, cached once per runtime */\n')
    source = once(source, '    rt->mf = *mf;\n',
                  '    rt->producer_prepare_skip = getenv("QJS_PRODUCER_PREPARE_SKIP") != NULL;\n'
                  '    rt->mf = *mf;\n')
    source = once(source, 'static void js_producer_region_prepare(JSRuntime *rt, JSFunctionBytecode *b)\n{\n',
                  'static void js_producer_region_prepare(JSRuntime *rt, JSFunctionBytecode *b)\n{\n'
                  '    PRODUCER_REGION_STAT(rt, prepare_calls, 1);\n'
                  '    if (rt->producer_prepare_skip) {\n'
                  '        PRODUCER_REGION_STAT(rt, prepare_skips, 1);\n'
                  '        return;\n'
                  '    }\n')
    source = once(source, 'struct { uint64_t scanned, plans, sites, bytes, live_bytes,',
                  'struct { uint64_t prepare_calls, prepare_skips, scanned, plans, sites, bytes, live_bytes,')
    source = once(source, '    PR_PRINT(scanned);\n',
                  '    PR_PRINT(prepare_calls);\n    PR_PRINT(prepare_skips);\n    PR_PRINT(scanned);\n')
    return source
