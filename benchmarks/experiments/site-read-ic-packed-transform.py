"""Layout-only design revision of the site IC; no broader semantics/coverage.

Reuses the old is_hashed byte plus its alignment space for a 31-bit identity.
The runtime stops assigning identities at 2**31-1; it never wraps. This file
does not build/run anything. Benchmark only after the framework-cost gate.
"""
import importlib.util
from pathlib import Path


def transform(source):
    spec = importlib.util.spec_from_file_location(
        'site_read_ic_base', Path(__file__).with_name('site-read-ic-transform.py'))
    base = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(base)
    result = base.transform(source)
    result = base.once(result, '    uint64_t read_ic_next_layout_id;\n',
                       '    uint32_t read_ic_next_layout_id;\n')
    result = base.once(result, '    uint8_t is_hashed;\n',
                       '    uint32_t is_hashed : 1;\n'
                       '    uint32_t read_ic_layout_id : 31;\n')
    result = base.once(result, '    uint64_t read_ic_layout_id;\n', '')
    result = base.once(result, '    uint64_t layout[2];\n',
                       '    uint32_t layout[2];\n')
    result = base.once(result, '    JSShape *sh = p->shape;\n    uint64_t id;\n',
                       '    JSShape *sh = p->shape;\n    uint32_t id;\n')
    result = base.once(result,
                       'if (rt->read_ic_next_layout_id == UINT64_MAX)',
                       'if (rt->read_ic_next_layout_id == UINT32_C(0x7fffffff))')
    return result
