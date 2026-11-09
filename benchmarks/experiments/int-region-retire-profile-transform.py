"""Diagnostic-only identities and canonical instructions of first-failure sites.
No scoring. Does not count executions after the site restores its canonical entry.
"""
import importlib.util
from pathlib import Path


def transform(source):
    path = Path(__file__).with_name('int-region-transform.py')
    spec = importlib.util.spec_from_file_location('int_region_retire_base', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = module.transform(source)
    anchor = '                    memcpy(b->byte_code_buf + pos, site->original, 5);\n'
    assert source.count(anchor) == 1
    log = r'''                    {
                        char fn[256], file[256];
                        uint32_t j;
                        const JSIntRegionInput *inputs = (const void *)((const uint8_t *)plan + site->payload);
                        const JSIntRegionNode *nodes = (const void *)(inputs + site->inputs);
                        fprintf(stderr, "INT_REGION_RETIRE name=%s file=%s line=%d pc=%u end=%u insns=%u\n",
                            JS_AtomGetStr(ctx, fn, sizeof(fn), b->func_name),
                            JS_AtomGetStr(ctx, file, sizeof(file), b->filename),
                            b->line_num, pos, site->end, site->insns);
                        for (j = 0; j < site->inputs; j++) {
                            const JSIntRegionInput *in = &inputs[j];
                            JSValue value = in->kind == 1 ? var_buf[in->value] :
                                            in->kind == 2 ? arg_buf[in->value] : js_int32(in->value);
                            fprintf(stderr, "IR_INPUT ref=%u kind=%u source=%d tag=%d int=%d\n",
                                j, in->kind, in->value, (int)JS_VALUE_GET_TAG(value),
                                JS_VALUE_GET_TAG(value) == JS_TAG_INT ? JS_VALUE_GET_INT(value) : 0);
                        }
                        for (j = 0; j < site->nodes; j++)
                            fprintf(stderr, "IR_NODE ref=%u op=%u left=%u right=%u\n",
                                    IR_NODE_BASE + j, nodes[j].op, nodes[j].left, nodes[j].right);
                        fprintf(stderr, "IR_BYTES ");
                        for (j = pos; j < site->end; j++)
                            fprintf(stderr, "%02x", j < pos + 5 ? site->original[j - pos] : b->byte_code_buf[j]);
                        fputc('\n', stderr);
                    }
'''
    return '#define QJS_INT_REGION_DIAGNOSTICS 1\n' + source.replace(anchor, log + anchor)
