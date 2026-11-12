/* Included by quickjs.c after the own-property lookup helpers. */
enum {
    QJS_JIT_FEEDBACK_SMALL_SITES = 8,
    QJS_JIT_FEEDBACK_MEDIUM_SITES = 16,
    QJS_JIT_FEEDBACK_LARGE_SITES = 32,
    QJS_JIT_FEEDBACK_PROBES = 4,
    QJS_JIT_FEEDBACK_MAX_BYTECODE = 4096,
    QJS_JIT_FEEDBACK_LIMIT = 2 * 1024 * 1024,
    QJS_JIT_FEEDBACK_POLYMORPHISM = 2,
    QJS_JIT_FEEDBACK_INITIAL_EVENTS = 128,
    QJS_JIT_FEEDBACK_SAMPLE_WINDOW = 8,
    QJS_JIT_FEEDBACK_SAMPLE_SHIFT = 8,
    QJS_JIT_FEEDBACK_OTHER_TAGS = JS_TAG_FLOAT64 - JS_TAG_FIRST - 1,
    QJS_JIT_PROPERTY_WRITE_MISS = 2,
};

typedef enum {
    QJS_JIT_FEEDBACK_BINARY = 1,
    QJS_JIT_FEEDBACK_BRANCH,
    QJS_JIT_FEEDBACK_PROPERTY,
    QJS_JIT_FEEDBACK_CALL,
    QJS_JIT_FEEDBACK_PROPERTY_WRITE,
    QJS_JIT_FEEDBACK_ELEMENT_READ,
    QJS_JIT_FEEDBACK_ELEMENT_WRITE,
} QJSJitFeedbackKind;

enum {
    QJS_JIT_ELEMENT_DENSE = 1u << 0,
    QJS_JIT_ELEMENT_SPARSE = 1u << 1,
    QJS_JIT_ELEMENT_TYPED = 1u << 2,
    QJS_JIT_ELEMENT_PROXY = 1u << 3,
    QJS_JIT_ELEMENT_OBJECT = 1u << 4,
    QJS_JIT_ELEMENT_STRING = 1u << 5,
    QJS_JIT_ELEMENT_OTHER = 1u << 6,
};

typedef struct {
    uint32_t receivers, typed_kinds, index_tags, value_tags;
    uint32_t samples, boxed_samples, int_indices;
    uint32_t in_bounds, at_end, out_of_bounds, invalid_view, unknown;
    int32_t min_index, max_index;
} QJSJitElementFeedback;

typedef struct {
    double minimum, maximum;
    /* Saturate at UINT16_MAX for hot numeric tags and UINT8_MAX otherwise. */
    uint16_t ints, floats;
    uint8_t other_tags[QJS_JIT_FEEDBACK_OTHER_TAGS];
    bool has_range, negative_zero, nan;
} QJSJitOperandFeedback;

typedef struct {
    uint32_t pc;
    uint8_t kind;
    union {
        struct {
            QJSJitOperandFeedback operands[2];
            uint32_t samples;
        } binary;
        struct {
            uint32_t taken, fallthrough;
        } branch;
        struct {
            struct {
                uintptr_t shape;
                uint32_t slot;
            } entries[QJS_JIT_FEEDBACK_POLYMORPHISM];
            uint32_t hits, misses, boxed_hits;
            uint32_t transition_hits, transition_boxed_hits;
            uint8_t count;
            bool megamorphic;
        } property;
        struct {
            struct {
                uint32_t id, count;
            } targets[QJS_JIT_FEEDBACK_POLYMORPHISM];
            uint32_t other, native_hits, boxed_hits, inline_hits;
            uint8_t count;
            bool inline_valid;
            int16_t inline_delta;
        } call;
        QJSJitElementFeedback element;
    } u;
} QJSJitFeedbackSite;

typedef struct QJSJitFeedback {
    uint32_t events;
    uint32_t site_mask;
    QJSJitFeedbackSite sites[];
} QJSJitFeedback;

_Static_assert(JS_TAG_FLOAT64 - JS_TAG_FIRST < 32, "QuickJS tags must fit the feedback mask");

static void qjs_jit_feedback_increment(uint32_t *count)
{
    if (*count < UINT32_MAX)
        ++*count;
}

static size_t qjs_jit_feedback_size(unsigned capacity)
{
    return sizeof(QJSJitFeedback) + capacity * sizeof(QJSJitFeedbackSite);
}

static QJSJitFeedbackSite *qjs_jit_feedback_find(QJSJitFeedback *feedback, uint32_t pc,
                                                 QJSJitFeedbackKind kind, bool create)
{
    unsigned index = (pc * UINT32_C(2654435761)) >> 27;
    for (unsigned i = 0; i < QJS_JIT_FEEDBACK_PROBES; i++) {
        QJSJitFeedbackSite *site = &feedback->sites[(index + i) & feedback->site_mask];
        if (!site->kind) {
            if (!create)
                return NULL;
            site->pc = pc;
            site->kind = kind;
        }
        if (site->pc == pc && site->kind == kind)
            return site;
    }
    return NULL;
}

static inline const QJSJitFeedbackSite *qjs_jit_feedback_lookup(const JSFunctionBytecode *b,
                                                               uint32_t pc, QJSJitFeedbackKind kind)
{
    return b->jit_feedback ? qjs_jit_feedback_find(b->jit_feedback, pc, kind, false) : NULL;
}

static QJSJitFeedbackSite *qjs_jit_feedback_get(JSRuntime *rt, JSFunctionBytecode *b,
                                               const uint8_t *pc, QJSJitFeedbackKind kind)
{
    if (!b->jit_feedback) {
        if (b->byte_code_len > QJS_JIT_FEEDBACK_MAX_BYTECODE)
            return NULL;
        /* Sites can remain live across guest callbacks. Choose the table size
           at allocation and keep it fixed until the bytecode is released. */
        unsigned capacity = b->byte_code_len <= 8 ? QJS_JIT_FEEDBACK_SMALL_SITES :
                            b->byte_code_len <= 24 ? QJS_JIT_FEEDBACK_MEDIUM_SITES :
                                                    QJS_JIT_FEEDBACK_LARGE_SITES;
        size_t size = qjs_jit_feedback_size(capacity);
        if (rt->jit_feedback_bytes > QJS_JIT_FEEDBACK_LIMIT - size)
            return NULL;
        b->jit_feedback = js_mallocz_rt(rt, size);
        if (!b->jit_feedback)
            return NULL;
        b->jit_feedback->site_mask = capacity - 1;
        rt->jit_feedback_bytes += size;
    }
    return qjs_jit_feedback_find(b->jit_feedback, pc - b->byte_code_buf, kind, true);
}

static inline bool qjs_jit_feedback_sample(JSFunctionBytecode *b)
{
    QJSJitFeedback *feedback = b->jit_feedback;
    if (!feedback)
        return b->byte_code_len <= QJS_JIT_FEEDBACK_MAX_BYTECODE;

    /* Sample densely at first, then move a small window through repeated
       instruction sequences so a fixed loop period cannot pin the sample. */
    uint32_t event = feedback->events++;
    return event < QJS_JIT_FEEDBACK_INITIAL_EVENTS ||
           ((event ^ (event >> QJS_JIT_FEEDBACK_SAMPLE_SHIFT)) &
            ((1u << QJS_JIT_FEEDBACK_SAMPLE_SHIFT) - 1)) < QJS_JIT_FEEDBACK_SAMPLE_WINDOW;
}

static inline uint32_t qjs_jit_feedback_tag_mask(JSValueConst value)
{
    int tag = JS_VALUE_GET_NORM_TAG(value);
    assert(tag >= JS_TAG_FIRST && tag <= JS_TAG_FLOAT64);
    return UINT32_C(1) << (tag - JS_TAG_FIRST);
}

static no_inline QJSJitElementFeedback *qjs_jit_feedback_element(JSRuntime *rt, JSFunctionBytecode *b,
                                                                const uint8_t *pc, JSValueConst receiver,
                                                                JSValueConst index, bool write, bool boxed)
{
    if (!qjs_jit_feedback_sample(b))
        return NULL;
    QJSJitFeedbackSite *site = qjs_jit_feedback_get(rt, b, pc, write ? QJS_JIT_FEEDBACK_ELEMENT_WRITE :
                                                                    QJS_JIT_FEEDBACK_ELEMENT_READ);
    if (!site)
        return NULL;

    QJSJitElementFeedback *element = &site->u.element;
    qjs_jit_feedback_increment(&element->samples);
    if (boxed)
        qjs_jit_feedback_increment(&element->boxed_samples);
    element->index_tags |= qjs_jit_feedback_tag_mask(index);
    bool int_index = JS_VALUE_GET_TAG(index) == JS_TAG_INT;
    int32_t idx = int_index ? JS_VALUE_GET_INT(index) : 0;
    if (int_index) {
        if (!element->int_indices || idx < element->min_index)
            element->min_index = idx;
        if (!element->int_indices || idx > element->max_index)
            element->max_index = idx;
        qjs_jit_feedback_increment(&element->int_indices);
    }

    uint32_t *outcome = &element->unknown;
    if (JS_IsObject(receiver)) {
        JSObject *object = JS_VALUE_GET_OBJ(receiver);
        if (object->class_id == JS_CLASS_ARRAY) {
            element->receivers |= object->fast_array ? QJS_JIT_ELEMENT_DENSE : QJS_JIT_ELEMENT_SPARSE;
            /* A fast array's count describes its contiguous storage prefix.
               Holes and inherited properties beyond it use generic lookup. */
            if (object->fast_array && int_index) {
                uint32_t count = object->u.array.count;
                outcome = (uint32_t)idx < count ? &element->in_bounds :
                          (uint32_t)idx == count ? &element->at_end : &element->out_of_bounds;
            }
        } else if (is_typed_array(object->class_id)) {
            element->receivers |= QJS_JIT_ELEMENT_TYPED;
            element->typed_kinds |= UINT32_C(1) << (object->class_id - JS_CLASS_UINT8C_ARRAY);
            if (typed_array_is_oob(object)) {
                outcome = &element->invalid_view;
            } else if (int_index) {
                uint32_t count = object->u.array.count;
                outcome = (uint32_t)idx < count ? &element->in_bounds :
                          (uint32_t)idx == count ? &element->at_end : &element->out_of_bounds;
            }
        } else {
            element->receivers |= object->class_id == JS_CLASS_PROXY ? QJS_JIT_ELEMENT_PROXY : QJS_JIT_ELEMENT_OBJECT;
        }
    } else {
        element->receivers |= JS_IsString(receiver) ? QJS_JIT_ELEMENT_STRING : QJS_JIT_ELEMENT_OTHER;
    }
    qjs_jit_feedback_increment(outcome);
    return element;
}

static inline void qjs_jit_feedback_element_value(QJSJitElementFeedback *element, JSValueConst value)
{
    if (unlikely(element))
        element->value_tags |= qjs_jit_feedback_tag_mask(value);
}

static no_inline JSValue qjs_jit_feedback_get_element(JSContext *ctx, JSFunctionBytecode *b,
                                                      const uint8_t *pc, JSValueConst receiver,
                                                      JSValue index, bool boxed)
{
    QJSJitElementFeedback *element = qjs_jit_feedback_element(ctx->rt, b, pc, receiver, index, false, boxed);
    JSValue value = JS_GetPropertyValue(ctx, receiver, index);
    qjs_jit_feedback_element_value(element, value);
    return value;
}

static unsigned qjs_jit_feedback_other_tag_index(int tag)
{
    assert(tag >= JS_TAG_FIRST && tag < JS_TAG_FLOAT64 && tag != JS_TAG_INT);
    return tag - JS_TAG_FIRST - (tag > JS_TAG_INT);
}

static void qjs_jit_feedback_operand(QJSJitOperandFeedback *operand, JSValueConst value)
{
    int tag = JS_VALUE_GET_NORM_TAG(value);
    if (tag != JS_TAG_INT && tag != JS_TAG_FLOAT64) {
        uint8_t *count = &operand->other_tags[qjs_jit_feedback_other_tag_index(tag)];
        if (*count < UINT8_MAX)
            ++*count;
        return;
    }

    uint16_t *count = tag == JS_TAG_INT ? &operand->ints : &operand->floats;
    if (*count < UINT16_MAX)
        ++*count;
    double number = tag == JS_TAG_INT ? JS_VALUE_GET_INT(value) : JS_VALUE_GET_FLOAT64(value);
    if (isnan(number)) {
        operand->nan = true;
        return;
    }
    if (number == 0 && signbit(number))
        operand->negative_zero = true;
    if (!operand->has_range || number < operand->minimum)
        operand->minimum = number;
    if (!operand->has_range || number > operand->maximum)
        operand->maximum = number;
    operand->has_range = true;
}

static no_inline void qjs_jit_feedback_binary(JSRuntime *rt, JSFunctionBytecode *b,
                                               const uint8_t *pc, JSValueConst left,
                                               JSValueConst right)
{
    if (!qjs_jit_feedback_sample(b))
        return;
    QJSJitFeedbackSite *site = qjs_jit_feedback_get(rt, b, pc, QJS_JIT_FEEDBACK_BINARY);
    if (site) {
        qjs_jit_feedback_operand(&site->u.binary.operands[0], left);
        qjs_jit_feedback_operand(&site->u.binary.operands[1], right);
        qjs_jit_feedback_increment(&site->u.binary.samples);
    }
}

static no_inline void qjs_jit_feedback_branch(JSRuntime *rt, JSFunctionBytecode *b,
                                               const uint8_t *pc, bool taken)
{
    if (!qjs_jit_feedback_sample(b))
        return;
    QJSJitFeedbackSite *site = qjs_jit_feedback_get(rt, b, pc, QJS_JIT_FEEDBACK_BRANCH);
    if (site)
        qjs_jit_feedback_increment(taken ? &site->u.branch.taken : &site->u.branch.fallthrough);
}

static bool qjs_jit_feedback_inline_leaf(const JSFunctionBytecode *target, int16_t *delta)
{
    if (target->func_kind != JS_FUNC_NORMAL || target->arg_count != 1 ||
        target->var_count || target->var_ref_count || target->closure_var_count)
        return false;

    const uint8_t *pc = target->byte_code_buf;
    const uint8_t *end = pc + target->byte_code_len;
    if (end - pc < 4 || *pc++ != OP_get_arg0)
        return false;

    int opcode = *pc++;
    int64_t immediate;
    if (opcode >= OP_push_minus1 && opcode <= OP_push_7) {
        immediate = opcode - OP_push_0;
    } else if (opcode == OP_push_i8 && end - pc == 3) {
        immediate = (int8_t)*pc++;
    } else if (opcode == OP_push_i16 && end - pc == 4) {
        immediate = (int16_t)get_u16(pc);
        pc += 2;
    } else if (opcode == OP_push_i32 && end - pc == 6) {
        immediate = (int32_t)get_u32(pc);
        pc += 4;
    } else {
        return false;
    }
    if (end - pc != 2 || (pc[0] != OP_add && pc[0] != OP_sub) || pc[1] != OP_return)
        return false;
    if (pc[0] == OP_sub)
        immediate = -immediate;
    if (immediate < -4095 || immediate > 4095)
        return false;
    *delta = immediate;
    return true;
}

static void qjs_jit_feedback_record_call(JSRuntime *rt, QJSJitFeedbackSite *site,
                                         JSValueConst callee)
{
    if (JS_VALUE_GET_TAG(callee) == JS_TAG_OBJECT) {
        JSObject *object = JS_VALUE_GET_OBJ(callee);
        if (js_class_has_bytecode(object->class_id)) {
            JSFunctionBytecode *target = object->u.func.function_bytecode;
            if (!target->jit_feedback_target_id && rt->jit_feedback_target_id != UINT32_MAX)
                target->jit_feedback_target_id = ++rt->jit_feedback_target_id;
            uint32_t id = target->jit_feedback_target_id;
            if (id) {
                for (unsigned i = 0; i < site->u.call.count; i++) {
                    if (site->u.call.targets[i].id == id) {
                        qjs_jit_feedback_increment(&site->u.call.targets[i].count);
                        return;
                    }
                }
                if (site->u.call.count < QJS_JIT_FEEDBACK_POLYMORPHISM) {
                    unsigned i = site->u.call.count++;
                    site->u.call.targets[i].id = id;
                    site->u.call.targets[i].count = 1;
                    if (!i && !site->u.call.other && object->class_id == JS_CLASS_BYTECODE_FUNCTION)
                        site->u.call.inline_valid =
                            qjs_jit_feedback_inline_leaf(target, &site->u.call.inline_delta);
                    return;
                }
            }
        }
    }
    qjs_jit_feedback_increment(&site->u.call.other);
}

static no_inline void qjs_jit_feedback_call(JSRuntime *rt, JSFunctionBytecode *b,
                                             const uint8_t *pc, JSValueConst callee)
{
    if (!qjs_jit_feedback_sample(b))
        return;
    QJSJitFeedbackSite *site = qjs_jit_feedback_get(rt, b, pc, QJS_JIT_FEEDBACK_CALL);
    if (site)
        qjs_jit_feedback_record_call(rt, site, callee);
}

static bool qjs_jit_feedback_property_usable(const JSShapeProperty *descriptor, bool write)
{
    if (write) {
        return (descriptor->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE | JS_PROP_LENGTH)) ==
               JS_PROP_WRITABLE;
    }
    return !(descriptor->flags & JS_PROP_TMASK);
}

static no_inline JSShapeProperty *qjs_jit_feedback_property_lookup(JSRuntime *rt, JSFunctionBytecode *b,
                                                                   const uint8_t *pc, JSObject *object,
                                                                   JSAtom atom, JSProperty **property,
                                                                   QJSJitFeedbackSite **transition, bool boxed)
{
    bool write = transition != NULL;
    if (object->class_id != JS_CLASS_OBJECT || object->is_exotic)
        return write ? NULL : find_own_property_hint(rt, property, object, atom);
    QJSJitFeedbackSite *site = qjs_jit_feedback_get(rt, b, pc, write ? QJS_JIT_FEEDBACK_PROPERTY_WRITE :
                                                                  QJS_JIT_FEEDBACK_PROPERTY);
    if (!site || site->u.property.megamorphic)
        return write ? NULL : find_own_property_hint(rt, property, object, atom);

    JSShape *shape = object->shape;
    uintptr_t shape_id = (uintptr_t)shape;
    unsigned i;
    for (i = 0; i < site->u.property.count; i++) {
        if (site->u.property.entries[i].shape == shape_id) {
            uint32_t slot = site->u.property.entries[i].slot;
            /* The cached address is never dereferenced. Revalidate the current
               shape's slot even if the old address was recycled by GC. */
            if (slot < (uint32_t)shape->prop_count) {
                JSShapeProperty *descriptor = &get_shape_prop(shape)[slot];
                if (descriptor->atom == atom && qjs_jit_feedback_property_usable(descriptor, write)) {
                    qjs_jit_feedback_increment(&site->u.property.hits);
                    if (boxed)
                        qjs_jit_feedback_increment(&site->u.property.boxed_hits);
                    *property = &object->prop[slot];
                    return descriptor;
                }
            }
            break;
        }
    }

    qjs_jit_feedback_increment(&site->u.property.misses);
    JSShapeProperty *descriptor = find_own_property_hint(rt, property, object, atom);
    if (write && !descriptor && !shape->proto && object->extensible)
        *transition = site;
    if (descriptor && qjs_jit_feedback_property_usable(descriptor, write)) {
        if (i == QJS_JIT_FEEDBACK_POLYMORPHISM) {
            site->u.property.megamorphic = true;
        } else {
            if (i == site->u.property.count)
                site->u.property.count++;
            site->u.property.entries[i].shape = shape_id;
            site->u.property.entries[i].slot = *property - object->prop;
        }
    }
    return write ? NULL : descriptor;
}

static inline JSShapeProperty *qjs_jit_feedback_property(JSRuntime *rt, JSFunctionBytecode *b,
                                                        const uint8_t *pc, JSObject *object,
                                                        JSAtom atom, JSProperty **property)
{
    return qjs_jit_feedback_property_lookup(rt, b, pc, object, atom, property,
                                            NULL, false);
}

/* The fast setter consumes value even on allocation failure. On a miss the
   generic setter retains responsibility for consuming the caller's value. */
static inline int qjs_jit_feedback_put_property(JSContext *ctx, JSFunctionBytecode *b,
                                                 const uint8_t *pc, JSValue object,
                                                 JSAtom atom, JSValue value, bool boxed)
{
    if (!JS_IsObject(object))
        return QJS_JIT_PROPERTY_WRITE_MISS;
    JSObject *receiver = JS_VALUE_GET_OBJ(object);
    JSProperty *property;
    QJSJitFeedbackSite *transition = NULL;
    if (qjs_jit_feedback_property_lookup(ctx->rt, b, pc, receiver,
                                         atom, &property, &transition, boxed)) {
        set_value(ctx, &property->u.value, value);
        return true;
    }
    if (!transition)
        return QJS_JIT_PROPERTY_WRITE_MISS;

    /* The preceding miss proved absence on an ordinary extensible object
       without a prototype. Reuse QuickJS's shape transition and allocator. */
    property = add_property(ctx, receiver, atom, JS_PROP_C_W_E);
    if (!property) {
        JS_FreeValue(ctx, value);
        return -1;
    }
    property->u.value = value;
    qjs_jit_feedback_increment(&transition->u.property.transition_hits);
    if (boxed)
        qjs_jit_feedback_increment(&transition->u.property.transition_boxed_hits);
    return true;
}

static unsigned qjs_jit_feedback_operand_count(const QJSJitOperandFeedback *operand, int tag)
{
    if (tag == JS_TAG_INT)
        return operand->ints;
    if (tag == JS_TAG_FLOAT64)
        return operand->floats;
    return operand->other_tags[qjs_jit_feedback_other_tag_index(tag)];
}

static uint32_t qjs_jit_feedback_operand_tags(const QJSJitOperandFeedback *operand)
{
    uint32_t tags = 0;
    for (int tag = JS_TAG_FIRST; tag <= JS_TAG_FLOAT64; ++tag) {
        if (qjs_jit_feedback_operand_count(operand, tag))
            tags |= UINT32_C(1) << (tag - JS_TAG_FIRST);
    }
    return tags;
}

static void qjs_jit_feedback_print_histogram(const QJSJitOperandFeedback *operand)
{
    const char *separator = "";
    for (int tag = JS_TAG_FIRST; tag <= JS_TAG_FLOAT64; ++tag) {
        unsigned count = qjs_jit_feedback_operand_count(operand, tag);
        if (count) {
            fprintf(stderr, "%s%d:%u", separator, tag, count);
            separator = ",";
        }
    }
}

static void qjs_jit_feedback_free(JSRuntime *rt, JSFunctionBytecode *b)
{
    QJSJitFeedback *feedback = b->jit_feedback;
    if (!feedback)
        return;
    if (rt->jit_feedback_dump) {
        char atom_buf[128];
        const char *name = JS_AtomGetStrRT(rt, atom_buf, sizeof(atom_buf), b->func_name);
        for (unsigned i = 0; i <= feedback->site_mask; i++) {
            const QJSJitFeedbackSite *site = &feedback->sites[i];
            if (!site->kind)
                continue;
            fprintf(stderr, "QuickJS feedback name=%s pc=%u ", name, site->pc);
            switch (site->kind) {
            case QJS_JIT_FEEDBACK_BINARY: {
                const QJSJitOperandFeedback *lhs = &site->u.binary.operands[0];
                const QJSJitOperandFeedback *rhs = &site->u.binary.operands[1];
                fprintf(stderr, "binary samples=%u lhs_tags=%x rhs_tags=%x "
                        "lhs_range=%d,%.17g,%.17g rhs_range=%d,%.17g,%.17g "
                        "lhs_nan=%d rhs_nan=%d lhs_negative_zero=%d rhs_negative_zero=%d lhs_hist=",
                        site->u.binary.samples, qjs_jit_feedback_operand_tags(lhs),
                        qjs_jit_feedback_operand_tags(rhs),
                        lhs->has_range, lhs->minimum, lhs->maximum,
                        rhs->has_range, rhs->minimum, rhs->maximum,
                        lhs->nan, rhs->nan, lhs->negative_zero, rhs->negative_zero);
                qjs_jit_feedback_print_histogram(lhs);
                fputs(" rhs_hist=", stderr);
                qjs_jit_feedback_print_histogram(rhs);
                fputc('\n', stderr);
                break;
            }
            case QJS_JIT_FEEDBACK_BRANCH:
                fprintf(stderr, "branch taken=%u fallthrough=%u\n",
                        site->u.branch.taken, site->u.branch.fallthrough);
                break;
            case QJS_JIT_FEEDBACK_PROPERTY:
                fprintf(stderr, "property shapes=%u hits=%u misses=%u megamorphic=%d\n",
                        (unsigned)site->u.property.count, site->u.property.hits,
                        site->u.property.misses, site->u.property.megamorphic);
                break;
            case QJS_JIT_FEEDBACK_PROPERTY_WRITE:
                fprintf(stderr, "property_write shapes=%u hits=%u misses=%u megamorphic=%d boxed_hits=%u\n",
                        (unsigned)site->u.property.count, site->u.property.hits,
                        site->u.property.misses, site->u.property.megamorphic,
                        site->u.property.boxed_hits);
                if (site->u.property.transition_hits) {
                    fprintf(stderr, "QuickJS feedback name=%s pc=%u property_transition hits=%u boxed_hits=%u\n",
                            name, site->pc, site->u.property.transition_hits,
                            site->u.property.transition_boxed_hits);
                }
                break;
            case QJS_JIT_FEEDBACK_CALL:
                fprintf(stderr, "call targets=%u count0=%u count1=%u other=%u native_hits=%u "
                        "boxed_hits=%u inline_hits=%u\n",
                        (unsigned)site->u.call.count, site->u.call.targets[0].count,
                        site->u.call.targets[1].count, site->u.call.other, site->u.call.native_hits,
                        site->u.call.boxed_hits, site->u.call.inline_hits);
                break;
            case QJS_JIT_FEEDBACK_ELEMENT_READ:
            case QJS_JIT_FEEDBACK_ELEMENT_WRITE: {
                const QJSJitElementFeedback *element = &site->u.element;
                fprintf(stderr, "%s samples=%u boxed_samples=%u receivers=%x typed_kinds=%x "
                        "index_tags=%x value_tags=%x int_indices=%u index_range=%d,%d "
                        "in_bounds=%u at_end=%u out_of_bounds=%u invalid_view=%u unknown=%u\n",
                        site->kind == QJS_JIT_FEEDBACK_ELEMENT_READ ? "element_read" : "element_write",
                        element->samples, element->boxed_samples, element->receivers, element->typed_kinds,
                        element->index_tags, element->value_tags, element->int_indices,
                        element->min_index, element->max_index, element->in_bounds, element->at_end,
                        element->out_of_bounds, element->invalid_view, element->unknown);
                break;
            }
            }
        }
    }
    rt->jit_feedback_bytes -= qjs_jit_feedback_size(feedback->site_mask + 1);
    js_free_rt(rt, feedback);
    b->jit_feedback = NULL;
}
