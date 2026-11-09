"""Diagnostic-only dry runs at three identified retired readInt entries.

Canonical execution continues unchanged. The dry run returns before any local
commit, does not increment production success/failure counters, and is keyed
through the current frame's live b->int_regions plan rather than cached PCs.
"""
import importlib.util
from pathlib import Path


def once(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:120])
    return s.replace(old, new, 1)


def transform(source):
    p = Path(__file__).with_name('int-region-retire-profile-transform.py')
    sp = importlib.util.spec_from_file_location('ir_retired_frequency_base', p)
    m = importlib.util.module_from_spec(sp); sp.loader.exec_module(m)
    source = m.transform(source)
    source = once(source, '    uint32_t count;\n    JSIntRegionSite sites[];\n',
                  '    uint32_t count;\n    uint32_t watch_retired; /* diagnostic only */\n    JSIntRegionSite sites[];\n')
    source = once(source, '             covered_insns, saved_dispatches, dispatches; } int_region_stats;\n',
                  '             covered_insns, saved_dispatches, dispatches; } int_region_stats;\n'
                  '    struct { uint64_t executions, eligible, saved; } int_region_retired_watch[3];\n')
    source = once(source, '                              JSValue *args, JSValue *result)\n',
                  '                              JSValue *args, JSValue *result, bool dry_run)\n')
    source = once(source, 'js_int_region_run(rt, plan, site, var_buf, arg_buf, &result)',
                  'js_int_region_run(rt, plan, site, var_buf, arg_buf, &result, false)')
    for field in ('write_guard_fail','type_guard_fail','arithmetic_fail'):
        old = f'INT_REGION_STAT(rt, {field}, 1);'
        source = once(source, old, f'if (!dry_run) {old}')
    source = once(source, '    /* No callbacks, allocation, refcounted inputs or interrupt polls occurred.\n',
                  '    if (dry_run) return true; /* before every commit and production statistic */\n'
                  '    /* No callbacks, allocation, refcounted inputs or interrupt polls occurred.\n')
    source = once(source, '                    site->retired = 1;\n',
                  '                    site->retired = 1;\n'
                  '                    if (b->line_num == 66419 && pos == 153 && site->insns == 7) site->retired = 2;\n'
                  '                    if (b->line_num == 121996 && pos == 474 && site->insns == 6) site->retired = 3;\n'
                  '                    if (b->line_num == 175290 && pos == 458 && site->insns == 6) site->retired = 4;\n'
                  '                    if (site->retired >= 2) plan->watch_retired = 1;\n')
    helper = r'''
static inline void js_ir_watch_retired(JSRuntime *rt, JSFunctionBytecode *b,
                                       const uint8_t *pc, JSValue *locals, JSValue *args)
{
    JSIntRegionPlan *plan = b->int_regions;
    uint32_t i, pos;
    if (!plan || !plan->watch_retired) return;
    pos = pc - b->byte_code_buf;
    for (i = 0; i < plan->count; i++) {
        JSIntRegionSite *site = &plan->sites[i];
        if (site->retired >= 2 && site->retired <= 4 && site->pc == pos) {
            unsigned int index = site->retired - 2;
            JSValue unused;
            rt->int_region_retired_watch[index].executions++;
            if (js_int_region_run(rt, plan, site, locals, args, &unused, true)) {
                rt->int_region_retired_watch[index].eligible++;
                rt->int_region_retired_watch[index].saved += site->insns - 1;
            }
            break;
        }
    }
}
'''
    source = once(source, 'static JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n',
                  helper + '\nstatic JSValue js_object_layout_new(JSContext *ctx, JSFunctionBytecode *b, uint32_t pc)\n')
    source = once(source, 'switch ((INT_REGION_STAT(rt, dispatches, 1), opcode = *pc++))',
                  'switch ((js_ir_watch_retired(rt, b, pc, var_buf, arg_buf), INT_REGION_STAT(rt, dispatches, 1), opcode = *pc++))')
    source = once(source, '__extension__ ({ INT_REGION_STAT(rt, dispatches, 1); goto *dispatch_table[opcode = *pc++]; });',
                  '__extension__ ({ js_ir_watch_retired(rt, b, pc, var_buf, arg_buf); INT_REGION_STAT(rt, dispatches, 1); goto *dispatch_table[opcode = *pc++]; });')
    source = once(source, '    IR_PRINT(dispatches); fputc(\'\\n\', stderr);\n',
                  '    IR_PRINT(dispatches); fputc(\'\\n\', stderr);\n'
                  '    for (int ir_i = 0; ir_i < 3; ir_i++)\n'
                  '        fprintf(stderr, "IR_RETIRED_FREQUENCY site=%d executions=%" PRIu64 " eligible=%" PRIu64 " saved=%" PRIu64 "\\n",\n'
                  '                ir_i, rt->int_region_retired_watch[ir_i].executions,\n'
                  '                rt->int_region_retired_watch[ir_i].eligible, rt->int_region_retired_watch[ir_i].saved);\n')
    return source
