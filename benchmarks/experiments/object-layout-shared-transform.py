"""Single revision: reuse a matching final shape in the existing global table.

No threshold, helper-inlining, or new-shape capacity policy changes. An existing
matching shape naturally carries the capacity selected by its original creator.
"""
import importlib.util
from pathlib import Path

def transform(source):
    spec = importlib.util.spec_from_file_location('object_layout_base', Path(__file__).with_name('object-layout-transform.py'))
    base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
    s = base.transform(source)
    s = base.once(s, 'plans, sites, fields, bytes, shapes, objects, initialized;',
                  'plans, sites, fields, bytes, shapes, objects, initialized, shared_shapes;')
    s = base.once(s, '        hash_size = JS_PROP_INITIAL_HASH_SIZE;\n        while (hash_size < site->count)', r'''
        hash = shape_initial_hash(proto);
        for (i = 0; i < site->count; i++) {
            JSAtom atom = fields[site->first_field + i].atom;
            hash = shape_hash(shape_hash(hash, atom), JS_PROP_C_W_E);
        }
        for (sh = ctx->rt->shape_hash[get_shape_hash(hash, ctx->rt->shape_hash_bits)];
             sh; sh = sh->shape_hash_next) {
            JSShapeProperty *props;
            if (sh->hash != hash || sh->proto != proto ||
                sh->prop_count != site->count || sh->deleted_prop_count)
                continue;
            props = get_shape_prop(sh);
            for (i = 0; i < site->count; i++) {
                if (props[i].atom != fields[site->first_field + i].atom ||
                    props[i].flags != JS_PROP_C_W_E)
                    break;
            }
            if (i == site->count) {
                site->shape = js_dup_shape(sh);
                OBJECT_LAYOUT_STAT(ctx->rt, shared_shapes, 1);
                goto have_shape;
            }
        }
        hash_size = JS_PROP_INITIAL_HASH_SIZE;
        while (hash_size < site->count)''')
    s = base.once(s, '        hash = shape_initial_hash(proto);\n        for (i = 0; i < site->count; i++) {\n            JSAtom atom = fields[site->first_field + i].atom;\n            /* Exact capacity:',
        '        for (i = 0; i < site->count; i++) {\n            JSAtom atom = fields[site->first_field + i].atom;\n            /* Exact capacity:')
    s = base.once(s, '            (void)ret;\n            hash = shape_hash(shape_hash(hash, atom), JS_PROP_C_W_E);', '            (void)ret;')
    s = base.once(s, '    obj = JS_NewObjectFromShape(ctx, js_dup_shape(sh), JS_CLASS_OBJECT, NULL);',
        ' have_shape:\n    obj = JS_NewObjectFromShape(ctx, js_dup_shape(sh), JS_CLASS_OBJECT, NULL);')
    s = base.once(s, '    fprintf(stderr, "OBJECT_LAYOUT plans=',
        '    fprintf(stderr, "OBJECT_LAYOUT_SHARED hits=%llu\\n", (unsigned long long)rt->object_layout_stats.shared_shapes);\n'
        '    fprintf(stderr, "OBJECT_LAYOUT plans=')
    return s
