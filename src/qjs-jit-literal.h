/* Literal operands retain their interpreter-frame ownership across each
   helper. The consuming QuickJS APIs release their inputs on failure too. */
static int qjs_boxed_literal(QJSBoxedFrame *f, uint32_t operand, uint32_t pc)
{
    JSContext *ctx = f->ctx;
    JSRuntime *rt = ctx->rt;
    JSValue *sp = f->sp;
    int opcode = f->b->byte_code_buf[pc];
    JSValue value;
    int ret;

    qjs_boxed_safepoint(f);
    switch (opcode) {
    case OP_object:
        value = JS_NewObject(ctx);
        if (unlikely(JS_IsException(value)))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = value;
        rt->jit_boxed_literal_objects++;
        break;
    case OP_object_layout:
        if (unlikely(!f->b->object_layout))
            goto resume;
        value = js_object_layout_new(ctx, f->b, pc);
        if (unlikely(JS_IsException(value)))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = value;
        rt->jit_boxed_literal_layouts++;
        break;
    case OP_init_object_field: {
        if (unlikely(!f->b->object_layout || !JS_IsObject(sp[-2])))
            goto resume;
        JSObject *obj = JS_VALUE_GET_OBJ(sp[-2]);
        if (unlikely(operand >= (uint32_t)obj->shape->prop_count ||
                     get_shape_prop(obj->shape)[operand].flags != JS_PROP_C_W_E ||
                     !JS_IsUndefined(obj->prop[operand].u.value)))
            goto resume;
        obj->prop[operand].u.value = sp[-1];
        f->sp--;
        rt->jit_boxed_literal_layout_fields++;
        break;
    }
    case OP_array_from:
        value = JS_NewArrayFrom(ctx, operand, sp - operand);
        f->sp -= operand;
        if (unlikely(JS_IsException(value)))
            return QJS_BOXED_EXCEPTION;
        *f->sp++ = value;
        rt->jit_boxed_literal_arrays++;
        break;
    case OP_define_field:
        ret = JS_DefinePropertyValue(ctx, sp[-2], operand, sp[-1], JS_PROP_C_W_E | JS_PROP_THROW);
        f->sp--;
        if (unlikely(ret < 0))
            return QJS_BOXED_EXCEPTION;
        rt->jit_boxed_literal_fields++;
        break;
    case OP_define_array_el:
        ret = JS_DefinePropertyValueValue(ctx, sp[-3], js_dup(sp[-2]), sp[-1],
                                          JS_PROP_C_W_E | JS_PROP_THROW);
        f->sp--;
        if (unlikely(ret < 0))
            return QJS_BOXED_EXCEPTION;
        rt->jit_boxed_literal_fields++;
        break;
    case OP_set_proto:
        if ((JS_IsObject(sp[-1]) || JS_IsNull(sp[-1])) &&
            JS_SetPrototypeInternal(ctx, sp[-2], sp[-1], true) < 0)
            return QJS_BOXED_EXCEPTION;
        JS_FreeValue(ctx, *--f->sp);
        break;
    case OP_set_home_object:
        js_method_set_home_object(ctx, sp[-1], sp[-2]);
        break;
    case OP_set_name:
        if (JS_DefineObjectName(ctx, sp[-1], operand, JS_PROP_CONFIGURABLE) < 0)
            return QJS_BOXED_EXCEPTION;
        break;
    case OP_set_name_computed:
        if (JS_DefineObjectNameComputed(ctx, sp[-1], sp[-2], JS_PROP_CONFIGURABLE) < 0)
            return QJS_BOXED_EXCEPTION;
        break;
    case OP_append:
        if (js_append_enumerate(ctx, sp))
            return QJS_BOXED_EXCEPTION;
        JS_FreeValue(ctx, *--f->sp);
        break;
    case OP_copy_data_properties:
        if (JS_CopyDataProperties(ctx, sp[-1 - (int)(operand & 3)],
                                  sp[-1 - (int)((operand >> 2) & 7)],
                                  sp[-1 - (int)((operand >> 5) & 7)], 0))
            return QJS_BOXED_EXCEPTION;
        break;
    default:
        abort();
    }
    return QJS_BOXED_CONTINUE;

resume:
    f->pc = pc;
    return QJS_BOXED_RESUME;
}
