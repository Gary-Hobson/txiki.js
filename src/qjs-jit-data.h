/* Included by quickjs.c after the scalar JIT and bytecode table. */
#if defined(__linux__)
#include <sys/auxv.h>
#endif

enum {
    QJS_JIT_DATA_UNKNOWN,
    QJS_JIT_DATA_UNSUPPORTED,
    QJS_JIT_DATA_INDEX,
    QJS_JIT_DATA_STORE,
    QJS_JIT_DATA_STORE_RESULT,
    QJS_JIT_DATA_LENGTH,
    QJS_JIT_DATA_CHAR_CODE,
    QJS_JIT_DATA_VIEW_I32,
    QJS_JIT_DATA_VIEW_U32,
    QJS_JIT_DATA_VIEW_F64,
    QJS_JIT_DATA_VIEW_SET_I32,
    QJS_JIT_DATA_VIEW_SET_U32,
    QJS_JIT_DATA_VIEW_SET_F64,
    QJS_JIT_DATA_SUM,
    QJS_JIT_DATA_SUM_WRAP,
    QJS_JIT_DATA_MAX_ARGS = 4,
    QJS_JIT_DATA_MAX_COUNT = 1 << 21,
    QJS_JIT_DATA_NO_ARG = UINT8_MAX,
};

_Static_assert(sizeof(JSValue) == 8 || sizeof(JSValue) == 16, "Unexpected JSValue size");
_Static_assert(offsetof(JSStringRope, len) == 0, "Unexpected rope length offset");
_Static_assert(QJS_JIT_BASELINE_THRESHOLD <= UINT8_MAX, "Data JIT hot counter is too small");

typedef struct {
    const uint8_t *code;
    unsigned len, pos;
} QJSJitDataCursor;

typedef struct {
    uint8_t kind, view_arg, bound_arg, value_arg, endian_arg;
} QJSJitDataPlan;

typedef struct {
    void *code;
    size_t code_size;
    const void *sum_i32, *sum_f64, *load_i32, *load_f64;
    const void *store_i32, *store_f64;
    const void *load_value, *string_length;
    const void *char_code8, *char_code16;
    const void *dataview_load32, *dataview_load64, *dataview_store32, *dataview_store64;
} QJSJitDataMapping;

typedef struct QJSJitData {
    QJSJitDataMapping mapping;
    uint64_t last_used, retry_at;
    unsigned active, warm_calls;
    unsigned evictions, reinstalls;
    unsigned load_hits, store_hits, sum_hits, simd_hits, f64_hits;
    unsigned array_hits, string_hits, char_code_hits;
    unsigned dataview_load_hits, dataview_store_hits;
    bool simd;
} QJSJitData;

static bool qjs_jit_data_byte(QJSJitDataCursor *c, uint8_t expected)
{
    if (c->pos == c->len || c->code[c->pos] != expected)
        return false;
    c->pos++;
    return true;
}

static bool qjs_jit_data_local(QJSJitDataCursor *c, uint8_t op, uint8_t slot)
{
    return qjs_jit_data_byte(c, op) && qjs_jit_data_byte(c, slot) &&
           qjs_jit_data_byte(c, 0);
}

static int qjs_jit_data_arg(QJSJitDataCursor *c, unsigned argc)
{
    if (c->pos == c->len)
        return -1;
    unsigned op = c->code[c->pos];
    if (op < OP_get_arg0 || op > OP_get_arg3 || op - OP_get_arg0 >= argc)
        return -1;
    c->pos++;
    return op - OP_get_arg0;
}

static int qjs_jit_data_jump(QJSJitDataCursor *c, uint8_t op)
{
    if (!qjs_jit_data_byte(c, op) || c->pos == c->len)
        return -1;
    int target = (int)c->pos + (int8_t)c->code[c->pos];
    c->pos++;
    return target;
}

static bool qjs_jit_data_is_dataview(unsigned kind)
{
    return kind >= QJS_JIT_DATA_VIEW_I32 && kind <= QJS_JIT_DATA_VIEW_SET_F64;
}

static bool qjs_jit_data_dataview_is_store(unsigned kind)
{
    return kind >= QJS_JIT_DATA_VIEW_SET_I32 && kind <= QJS_JIT_DATA_VIEW_SET_F64;
}

static unsigned qjs_jit_data_method_kind(JSRuntime *rt, JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom) || atom >= (uint32_t)rt->atom_size)
        return QJS_JIT_DATA_UNSUPPORTED;
    JSAtomStruct *p = rt->atom_array[atom];
    if (!p || atom_is_free(p) || p->atom_type != JS_ATOM_TYPE_STRING || p->is_wide_char)
        return QJS_JIT_DATA_UNSUPPORTED;
    static const struct {
        const char *name;
        uint8_t len, kind;
    } methods[] = {
        { "charCodeAt", sizeof("charCodeAt") - 1, QJS_JIT_DATA_CHAR_CODE },
        { "getInt32", sizeof("getInt32") - 1, QJS_JIT_DATA_VIEW_I32 },
        { "getUint32", sizeof("getUint32") - 1, QJS_JIT_DATA_VIEW_U32 },
        { "getFloat64", sizeof("getFloat64") - 1, QJS_JIT_DATA_VIEW_F64 },
        { "setInt32", sizeof("setInt32") - 1, QJS_JIT_DATA_VIEW_SET_I32 },
        { "setUint32", sizeof("setUint32") - 1, QJS_JIT_DATA_VIEW_SET_U32 },
        { "setFloat64", sizeof("setFloat64") - 1, QJS_JIT_DATA_VIEW_SET_F64 },
    };
    for (unsigned i = 0; i < countof(methods); i++) {
        if (p->len == methods[i].len && !memcmp(str8(p), methods[i].name, p->len))
            return methods[i].kind;
    }
    return QJS_JIT_DATA_UNSUPPORTED;
}

/* Match the bytecode itself, including both branch destinations. The two
   locals are exclusively the accumulator and the zero-based index. */
static bool qjs_jit_data_plan(const JSFunctionBytecode *b, QJSJitDataPlan *plan)
{
    if (b->func_kind != JS_FUNC_NORMAL || b->closure_var_count || b->var_ref_count ||
        b->arg_count > QJS_JIT_DATA_MAX_ARGS || b->byte_code_len <= 0 ||
        b->byte_code_len > 96)
        return false;

    QJSJitDataCursor c = { b->byte_code_buf, b->byte_code_len, 0 };
    if (b->var_count == 0) {
        int view = qjs_jit_data_arg(&c, b->arg_count);
        if (view < 0)
            return false;
        if (qjs_jit_data_byte(&c, OP_get_length)) {
            if (!qjs_jit_data_byte(&c, OP_return) || c.pos != c.len)
                return false;
            *plan = (QJSJitDataPlan){ QJS_JIT_DATA_LENGTH, view,
                                     QJS_JIT_DATA_NO_ARG, QJS_JIT_DATA_NO_ARG };
            return true;
        }
        if (qjs_jit_data_byte(&c, OP_get_field2)) {
            if (c.len - c.pos < 4)
                return false;
            unsigned kind = qjs_jit_data_method_kind(b->realm->rt, get_u32(c.code + c.pos));
            if (kind == QJS_JIT_DATA_UNSUPPORTED)
                return false;
            c.pos += 4;
            int index = qjs_jit_data_arg(&c, b->arg_count);
            bool store = qjs_jit_data_dataview_is_store(kind);
            int value = store ? qjs_jit_data_arg(&c, b->arg_count) : -1;
            int endian = qjs_jit_data_is_dataview(kind) ? qjs_jit_data_arg(&c, b->arg_count) : -1;
            bool tail = qjs_jit_data_byte(&c, OP_tail_call_method);
            if (index < 0 || (store && value < 0) ||
                (!tail && (!store || !qjs_jit_data_byte(&c, OP_call_method))) ||
                !qjs_jit_data_byte(&c, 1 + store + (endian >= 0)) ||
                !qjs_jit_data_byte(&c, 0) ||
                (!tail && !qjs_jit_data_byte(&c, OP_return_undef)) ||
                c.pos != c.len)
                return false;
            *plan = (QJSJitDataPlan){ kind, view, index,
                                     store ? value : QJS_JIT_DATA_NO_ARG,
                                     endian < 0 ? QJS_JIT_DATA_NO_ARG : endian };
            return true;
        }
        int index = qjs_jit_data_arg(&c, b->arg_count);
        if (index < 0)
            return false;
        if (qjs_jit_data_byte(&c, OP_get_array_el)) {
            if (!qjs_jit_data_byte(&c, OP_return) || c.pos != c.len)
                return false;
            *plan = (QJSJitDataPlan){ QJS_JIT_DATA_INDEX, view, index, QJS_JIT_DATA_NO_ARG };
            return true;
        }
        int value = qjs_jit_data_arg(&c, b->arg_count);
        bool returns_value = qjs_jit_data_byte(&c, OP_insert3);
        if (value < 0 || !qjs_jit_data_byte(&c, OP_put_array_el))
            return false;
        if (returns_value) {
            if (!qjs_jit_data_byte(&c, OP_return))
                return false;
        } else if (!qjs_jit_data_byte(&c, OP_return_undef)) {
            if (qjs_jit_data_arg(&c, b->arg_count) != value ||
                !qjs_jit_data_byte(&c, OP_return))
                return false;
            returns_value = true;
        }
        if (c.pos != c.len)
            return false;
        *plan = (QJSJitDataPlan){ returns_value ? QJS_JIT_DATA_STORE_RESULT : QJS_JIT_DATA_STORE,
                                 view, index, value };
        return true;
    }
    if (b->var_count != 2)
        return false;

    bool lexical = c.code[0] == OP_set_loc_uninitialized;
    if (lexical && !qjs_jit_data_local(&c, OP_set_loc_uninitialized, 0))
        return false;
    int seed = QJS_JIT_DATA_NO_ARG;
    if (!qjs_jit_data_byte(&c, OP_push_0)) {
        seed = qjs_jit_data_arg(&c, b->arg_count);
        if (seed < 0)
            return false;
    }
    if (!qjs_jit_data_byte(&c, OP_put_loc0) ||
        (lexical && !qjs_jit_data_local(&c, OP_set_loc_uninitialized, 1)) ||
        !qjs_jit_data_byte(&c, OP_push_0) || !qjs_jit_data_byte(&c, OP_put_loc1))
        return false;

    int loop = c.pos;
    if (lexical ? !qjs_jit_data_local(&c, OP_get_loc_check, 1)
                : !qjs_jit_data_byte(&c, OP_get_loc1))
        return false;
    int bound = qjs_jit_data_arg(&c, b->arg_count);
    bool use_length = qjs_jit_data_byte(&c, OP_get_length);
    if (bound < 0 || !qjs_jit_data_byte(&c, OP_lt))
        return false;
    int exit = qjs_jit_data_jump(&c, OP_if_false8);
    if (exit < 0 || (lexical ? !qjs_jit_data_local(&c, OP_get_loc_check, 0)
                             : !qjs_jit_data_byte(&c, OP_get_loc0)))
        return false;
    int view = qjs_jit_data_arg(&c, b->arg_count);
    if (view < 0 || (use_length && bound != view) ||
        (lexical ? !qjs_jit_data_local(&c, OP_get_loc_check, 1)
                 : !qjs_jit_data_byte(&c, OP_get_loc1)) ||
        !qjs_jit_data_byte(&c, OP_get_array_el) || !qjs_jit_data_byte(&c, OP_add))
        return false;
    bool wrap = qjs_jit_data_byte(&c, OP_push_0);
    if (wrap && !qjs_jit_data_byte(&c, OP_or))
        return false;
    if (lexical) {
        if (!qjs_jit_data_byte(&c, OP_dup) ||
            !qjs_jit_data_local(&c, OP_put_loc_check, 0) ||
            !qjs_jit_data_byte(&c, OP_drop) ||
            !qjs_jit_data_local(&c, OP_get_loc_check, 1) ||
            !qjs_jit_data_byte(&c, OP_post_inc) ||
            !qjs_jit_data_local(&c, OP_put_loc_check, 1) ||
            !qjs_jit_data_byte(&c, OP_drop))
            return false;
    } else if (!qjs_jit_data_byte(&c, OP_put_loc0) ||
               !qjs_jit_data_byte(&c, OP_inc_loc) || !qjs_jit_data_byte(&c, 1)) {
        return false;
    }
    if (qjs_jit_data_jump(&c, OP_goto8) != loop || c.pos != (unsigned)exit ||
        (lexical ? !qjs_jit_data_local(&c, OP_get_loc_check, 0)
                 : !qjs_jit_data_byte(&c, OP_get_loc0)) ||
        !qjs_jit_data_byte(&c, OP_return) || c.pos != c.len)
        return false;
    *plan = (QJSJitDataPlan){ wrap ? QJS_JIT_DATA_SUM_WRAP : QJS_JIT_DATA_SUM,
                             view, use_length ? QJS_JIT_DATA_NO_ARG : bound, seed };
    return true;
}

static const uint32_t qjs_jit_data_i32_scalar[] = {
    0xaa1f03e2, /* mov x2, xzr */
    0x340000a1, /* cbz w1, done */
    0xb8804403, /* loop: ldrsw x3, [x0], #4 */
    0x8b030042, /* add x2, x2, x3 */
    0x71000421, /* subs w1, w1, #1 */
    0x54ffffa1, /* b.ne loop */
    0xaa0203e0, /* done: mov x0, x2 */
    0xd65f03c0, /* ret */
};

static const uint32_t qjs_jit_data_i32_vector[] = {
    0x6f00e400, /* movi v0.2d, #0 */
    0x6f00e401, /* movi v1.2d, #0 */
    0x7100103f, /* cmp w1, #4 */
    0x540000e3, /* b.lo tail */
    0x3cc10402, /* loop: ldr q2, [x0], #16 */
    0x0ea21000, /* saddw v0.2d, v0.2d, v2.2s */
    0x4ea21021, /* saddw2 v1.2d, v1.2d, v2.4s */
    0x51001021, /* sub w1, w1, #4 */
    0x7100103f, /* cmp w1, #4 */
    0x54ffff62, /* b.hs loop */
    0x4ee18400, /* tail: add v0.2d, v0.2d, v1.2d */
    0x5ef1b800, /* addp d0, v0.2d */
    0x9e660002, /* fmov x2, d0 */
    0x340000a1, /* cbz w1, done */
    0xb8804403, /* remainder: ldrsw x3, [x0], #4 */
    0x8b030042, /* add x2, x2, x3 */
    0x71000421, /* subs w1, w1, #1 */
    0x54ffffa1, /* b.ne remainder */
    0xaa0203e0, /* done: mov x0, x2 */
    0xd65f03c0, /* ret */
};

/* Preserve JavaScript's left-to-right FP rounding and signed zero. */
static const uint32_t qjs_jit_data_f64_sum[] = {
    0x340000a1, /* cbz w1, done */
    0xfc408401, /* loop: ldr d1, [x0], #8 */
    0x1e612800, /* fadd d0, d0, d1 */
    0x71000421, /* subs w1, w1, #1 */
    0x54ffffa1, /* b.ne loop */
    0xd65f03c0, /* done: ret */
};
static const uint32_t qjs_jit_data_i32_load[] = {
    0xb8615800, /* ldr w0, [x0, w1, uxtw #2] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_f64_load[] = {
    0xfc615800, /* ldr d0, [x0, w1, uxtw #3] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_i32_store[] = {
    0xb8215802, /* str w2, [x0, w1, uxtw #2] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_f64_store[] = {
    0xfc215800, /* str d0, [x0, w1, uxtw #3] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_value16[] = {
    0x8b215000, /* add x0, x0, w1, uxtw #4 */
    0xa9401003, /* ldp x3, x4, [x0] */
    0xa9001043, /* stp x3, x4, [x2] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_value8[] = {
    0xf8615803, /* ldr x3, [x0, w1, uxtw #3] */
    0xf9000043, /* str x3, [x2] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_string_length[] = {
    0xb9400000, /* ldr w0, [x0] */
    0x12007800, /* and w0, w0, #0x7fffffff */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_char_code8[] = {
    0x38614800, /* ldrb w0, [x0, w1, uxtw] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_char_code16[] = {
    0x78615800, /* ldrh w0, [x0, w1, uxtw #1] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_dataview32[] = {
    0xb9400000, /* ldr w0, [x0] */
    0x34000041, /* cbz w1, done */
    0x5ac00800, /* rev w0, w0 */
    0xd65f03c0, /* done: ret */
};
static const uint32_t qjs_jit_data_dataview64[] = {
    0xf9400000, /* ldr x0, [x0] */
    0x34000041, /* cbz w1, done */
    0xdac00c00, /* rev x0, x0 */
    0xd65f03c0, /* done: ret */
};
static const uint32_t qjs_jit_data_dataview_store32[] = {
    0x34000042, /* cbz w2, store */
    0x5ac00821, /* rev w1, w1 */
    0xb9000001, /* store: str w1, [x0] */
    0xd65f03c0, /* ret */
};
static const uint32_t qjs_jit_data_dataview_store64[] = {
    0x34000042, /* cbz w2, store */
    0xdac00c21, /* rev x1, x1 */
    0xf9000001, /* store: str x1, [x0] */
    0xd65f03c0, /* ret */
};

static const void *qjs_jit_data_emit(uint8_t **cursor, const void *source, size_t size)
{
    uint8_t *entry = *cursor;
    memcpy(entry, source, size);
    *cursor += size;
    return entry;
}

static bool qjs_jit_data_compile(JSRuntime *rt, const JSFunctionBytecode *b)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || page > QJS_JIT_CODE_LIMIT)
        return false;
    size_t size = page;
    bool reinstalled = rt->jit_data != NULL;
    QJSJitData *data = reinstalled ? rt->jit_data : js_mallocz_rt(rt, sizeof(*data));
    if (!data)
        return false;
    assert(!data->mapping.code);
    JSMallocState *ms = &rt->malloc_state;
    if (!qjs_jit_cache_reserve(rt, b, size))
        goto fail;

    uint8_t *code = mmap(NULL, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED)
        goto fail;
    uint8_t *cursor = code;
    QJSJitDataMapping mapping = { .code = code, .code_size = size };
    bool simd = rt->jit_optimize;
#if defined(__linux__)
    /* Linux AArch64 HWCAP_ASIMD is bit 1. */
    simd = simd && (getauxval(AT_HWCAP) & (1UL << 1));
#endif
    if (simd) {
        mapping.sum_i32 = qjs_jit_data_emit(&cursor, qjs_jit_data_i32_vector,
                                           sizeof(qjs_jit_data_i32_vector));
    } else {
        mapping.sum_i32 = qjs_jit_data_emit(&cursor, qjs_jit_data_i32_scalar,
                                           sizeof(qjs_jit_data_i32_scalar));
    }
    mapping.sum_f64 = qjs_jit_data_emit(&cursor, qjs_jit_data_f64_sum,
                                       sizeof(qjs_jit_data_f64_sum));
    mapping.load_i32 = qjs_jit_data_emit(&cursor, qjs_jit_data_i32_load,
                                      sizeof(qjs_jit_data_i32_load));
    mapping.load_f64 = qjs_jit_data_emit(&cursor, qjs_jit_data_f64_load,
                                      sizeof(qjs_jit_data_f64_load));
    mapping.store_i32 = qjs_jit_data_emit(&cursor, qjs_jit_data_i32_store,
                                       sizeof(qjs_jit_data_i32_store));
    mapping.store_f64 = qjs_jit_data_emit(&cursor, qjs_jit_data_f64_store,
                                       sizeof(qjs_jit_data_f64_store));
    if (sizeof(JSValue) == 16) {
        mapping.load_value = qjs_jit_data_emit(&cursor, qjs_jit_data_value16,
                                           sizeof(qjs_jit_data_value16));
    } else {
        mapping.load_value = qjs_jit_data_emit(&cursor, qjs_jit_data_value8,
                                           sizeof(qjs_jit_data_value8));
    }
    /* Check the compiler's implementation-defined string bitfield layout
       before exposing the native entry. */
    JSString string = { .len = 0x1234567, .is_wide_char = 1 };
    uint32_t first_word;
    memcpy(&first_word, &string, sizeof(first_word));
    if (first_word == 0x81234567) {
        mapping.string_length = qjs_jit_data_emit(&cursor, qjs_jit_data_string_length,
                                                  sizeof(qjs_jit_data_string_length));
    }
    mapping.char_code8 = qjs_jit_data_emit(&cursor, qjs_jit_data_char_code8,
                                         sizeof(qjs_jit_data_char_code8));
    mapping.char_code16 = qjs_jit_data_emit(&cursor, qjs_jit_data_char_code16,
                                          sizeof(qjs_jit_data_char_code16));
    mapping.dataview_load32 = qjs_jit_data_emit(&cursor, qjs_jit_data_dataview32,
                                               sizeof(qjs_jit_data_dataview32));
    mapping.dataview_load64 = qjs_jit_data_emit(&cursor, qjs_jit_data_dataview64,
                                               sizeof(qjs_jit_data_dataview64));
    mapping.dataview_store32 = qjs_jit_data_emit(&cursor, qjs_jit_data_dataview_store32,
                                                sizeof(qjs_jit_data_dataview_store32));
    mapping.dataview_store64 = qjs_jit_data_emit(&cursor, qjs_jit_data_dataview_store64,
                                                sizeof(qjs_jit_data_dataview_store64));
    __builtin___clear_cache((char *)code, (char *)cursor);
    if (mprotect(code, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, size);
        goto fail;
    }
    data->mapping = mapping;
    data->simd = simd;
    data->last_used = rt->jit_cache_tick;
    data->reinstalls += reinstalled;
    rt->jit_data = data;
    rt->jit_code_bytes += size;
    ms->malloc_size += size;
    ms->malloc_count++;
    return true;

fail:
    if (!reinstalled)
        js_free_rt(rt, data);
    return false;
}

static void qjs_jit_data_drop_mapping(JSRuntime *rt, QJSJitData *data)
{
    assert(!data->active);
    QJSJitDataMapping *mapping = &data->mapping;
    munmap(mapping->code, mapping->code_size);
    rt->jit_code_bytes -= mapping->code_size;
    rt->malloc_state.malloc_size -= mapping->code_size;
    rt->malloc_state.malloc_count--;
    *mapping = (QJSJitDataMapping){0};
}

static bool qjs_jit_data_cache_evict(JSRuntime *rt)
{
    QJSJitData *data = rt->jit_data;
    if (!data || !data->mapping.code || data->active ||
        rt->jit_cache_tick - data->last_used < QJS_JIT_CACHE_COLD_AGE)
        return false;

    qjs_jit_data_drop_mapping(rt, data);
    data->warm_calls = 0;
    data->evictions++;
    return true;
}

static void qjs_jit_data_free(JSRuntime *rt)
{
    QJSJitData *data = rt->jit_data;
    if (!data)
        return;
    if (rt->jit_stats) {
        fprintf(stderr, "QuickJS JIT data: typed_load_hits=%u typed_store_hits=%u typed_sum_hits=%u "
                        "typed_simd_enabled=%u typed_simd_hits=%u typed_f64_hits=%u array_load_hits=%u "
                        "string_length_hits=%u string_char_code_hits=%u dataview_load_hits=%u "
                        "dataview_store_hits=%u data_cache_evictions=%u data_cache_reinstalls=%u\n",
                data->load_hits, data->store_hits, data->sum_hits, (unsigned)data->simd,
                data->simd_hits, data->f64_hits, data->array_hits, data->string_hits,
                data->char_code_hits, data->dataview_load_hits, data->dataview_store_hits,
                data->evictions, data->reinstalls);
    }
    if (data->mapping.code)
        qjs_jit_data_drop_mapping(rt, data);
    js_free_rt(rt, data);
    rt->jit_data = NULL;
}

static bool qjs_jit_data_intrinsic_length(JSObject *view)
{
    /* A typed array's indexed exotic handler does not intercept "length".
       A later exotic prototype can, and a user getter can resize or detach. */
    JSObject *p = view;
    for (unsigned depth = 0; p && depth < 8; depth++, p = p->shape->proto) {
        if (p != view && p->is_exotic)
            return false;
        JSProperty *pr;
        JSShapeProperty *prs = find_own_property(&pr, p, JS_ATOM_length);
        if (!prs)
            continue;
        if ((prs->flags & JS_PROP_TMASK) != JS_PROP_GETSET)
            return false;
        JSObject *getter = pr->u.getset.getter;
        return getter && getter->class_id == JS_CLASS_C_FUNCTION &&
               getter->u.cfunc.cproto == JS_CFUNC_getter &&
               getter->u.cfunc.c_function.getter == js_typed_array_get_length;
    }
    return false;
}

static JSObject *qjs_jit_data_method(const JSFunctionBytecode *b, JSObject *p)
{
    /* The matched bytecode has a one-byte receiver load followed by
       get_field2. Inspect descriptors without invoking exotic lookups. */
    JSAtom atom = get_u32(b->byte_code_buf + 2);
    for (unsigned depth = 0; p && depth < 8; depth++, p = p->shape->proto) {
        JSProperty *pr;
        JSShapeProperty *prs = find_own_property(&pr, p, atom);
        if (prs) {
            if ((prs->flags & JS_PROP_TMASK) || !JS_IsObject(pr->u.value))
                return NULL;
            JSObject *method = JS_VALUE_GET_OBJ(pr->u.value);
            return method->class_id == JS_CLASS_C_FUNCTION ? method : NULL;
        }
        if (p->is_exotic)
            return NULL;
    }
    return NULL;
}

static bool qjs_jit_data_intrinsic_char_code(const JSFunctionBytecode *b)
{
    JSValueConst proto = b->realm->class_proto[JS_CLASS_STRING];
    if (!JS_IsObject(proto))
        return false;
    JSObject *method = qjs_jit_data_method(b, JS_VALUE_GET_OBJ(proto));
    return method && method->u.cfunc.cproto == JS_CFUNC_generic &&
           method->u.cfunc.c_function.generic == js_string_charCodeAt;
}

static JSValue js_dataview_getValue(JSContext *ctx, JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id);
static JSValue js_dataview_setValue(JSContext *ctx, JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id);

static uint8_t *qjs_jit_data_dataview_address(const JSFunctionBytecode *b, JSObject *view,
                                             int cls, uint32_t offset, uint32_t width, bool store)
{
    JSObject *method = qjs_jit_data_method(b, view);
    if (!method || method->u.cfunc.cproto != JS_CFUNC_generic_magic ||
        method->u.cfunc.c_function.generic_magic != (store ? js_dataview_setValue : js_dataview_getValue) ||
        method->u.cfunc.magic != cls)
        return NULL;

    JSTypedArray *ta = view->u.typed_array;
    if (!ta)
        return NULL;
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    if (abuf->shared || abuf->detached || abuf->max_byte_length >= 0 || !abuf->data ||
        abuf->byte_length < 0 || (store && abuf->immutable))
        return NULL;

    uint32_t buffer_length = abuf->byte_length;
    if (ta->offset > buffer_length || ta->length > buffer_length - ta->offset ||
        offset > ta->length || width > ta->length - offset)
        return NULL;
    return abuf->data + ta->offset + offset;
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static bool qjs_jit_data_execute_dataview(QJSJitData *data, const JSFunctionBytecode *b,
                                           QJSJitDataPlan plan, JSObject *view,
                                           const JSValue *args, JSValue *result)
{
    bool little_endian = false;
    if (plan.endian_arg != QJS_JIT_DATA_NO_ARG) {
        JSValue endian = args[plan.endian_arg];
        if (JS_VALUE_GET_TAG(endian) != JS_TAG_BOOL)
            return false;
        little_endian = JS_VALUE_GET_BOOL(endian);
    }
    bool store = qjs_jit_data_dataview_is_store(plan.kind);
    bool is_f64 = plan.kind == QJS_JIT_DATA_VIEW_F64 || plan.kind == QJS_JIT_DATA_VIEW_SET_F64;
    bool is_i32 = plan.kind == QJS_JIT_DATA_VIEW_I32 || plan.kind == QJS_JIT_DATA_VIEW_SET_I32;
    int cls = is_f64 ? JS_CLASS_FLOAT64_ARRAY : is_i32 ? JS_CLASS_INT32_ARRAY : JS_CLASS_UINT32_ARRAY;
    uint32_t offset = JS_VALUE_GET_INT(args[plan.bound_arg]);
    uint8_t *ptr = qjs_jit_data_dataview_address(b, view, cls, offset, is_f64 ? 8 : 4, store);
    if (!ptr)
        return false;
    uint32_t swap = little_endian ^ !is_be();
    if (store) {
        /* Primitive Number conversion cannot run JavaScript after the address guard. */
        JSValue value = args[plan.value_arg];
        if (is_f64) {
            JSFloat64Union bits;
            if (!js_arith_to_float64(value, &bits.d))
                return false;
            void (*write)(uint8_t *, uint64_t, uint32_t) = (void *)data->mapping.dataview_store64;
            write(ptr, bits.u64, swap);
        } else {
            uint32_t bits;
            uint32_t tag = JS_VALUE_GET_TAG(value);
            if (tag == JS_TAG_INT)
                bits = JS_VALUE_GET_INT(value);
            else if (!JS_TAG_IS_FLOAT64(tag) || JS_ToUint32(b->realm, &bits, value))
                return false;
            void (*write)(uint8_t *, uint32_t, uint32_t) = (void *)data->mapping.dataview_store32;
            write(ptr, bits, swap);
        }
        *result = JS_UNDEFINED;
        data->dataview_store_hits++;
    } else {
        if (is_f64) {
            uint64_t (*load)(const uint8_t *, uint32_t) = (void *)data->mapping.dataview_load64;
            JSFloat64Union bits = { .u64 = load(ptr, swap) };
            *result = js_float64(bits.d);
        } else {
            uint32_t (*load)(const uint8_t *, uint32_t) = (void *)data->mapping.dataview_load32;
            uint32_t bits = load(ptr, swap);
            *result = is_i32 ? js_int32(bits) : js_uint32(bits);
        }
        data->dataview_load_hits++;
    }
    return true;
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static bool qjs_jit_data_execute(QJSJitData *data, const JSFunctionBytecode *b,
                                 QJSJitDataPlan plan, JSValue value,
                                 const JSValue *args, JSValue *result)
{
    if (plan.kind == QJS_JIT_DATA_LENGTH) {
        if (!data->mapping.string_length)
            return false;
        uint32_t (*length)(const void *) = (void *)data->mapping.string_length;
        *result = js_int32(length(JS_VALUE_GET_PTR(value)));
        data->string_hits++;
        return true;
    }
    if (plan.kind == QJS_JIT_DATA_CHAR_CODE) {
        JSValue index = args[plan.bound_arg];
        if (JS_VALUE_GET_TAG(index) != JS_TAG_INT || !qjs_jit_data_intrinsic_char_code(b))
            return false;
        JSString *str = JS_VALUE_GET_STRING(value);
        uint32_t offset = JS_VALUE_GET_INT(index);
        if (offset >= str->len) {
            *result = JS_NAN;
            return true;
        }
        const void *entry = str->is_wide_char ? data->mapping.char_code16 : data->mapping.char_code8;
        uint32_t (*load)(const void *, uint32_t) = (void *)entry;
        *result = js_int32(load(strv(str), offset));
        data->char_code_hits++;
        return true;
    }
    JSObject *view = JS_VALUE_GET_OBJ(value);
    if (plan.bound_arg != QJS_JIT_DATA_NO_ARG &&
        JS_VALUE_GET_TAG(args[plan.bound_arg]) != JS_TAG_INT)
        return false;
    if (qjs_jit_data_is_dataview(plan.kind))
        return qjs_jit_data_execute_dataview(data, b, plan, view, args, result);
    if (view->class_id == JS_CLASS_ARRAY) {
        uint32_t index = JS_VALUE_GET_INT(args[plan.bound_arg]);
        if (!view->fast_array || index >= view->u.array.count)
            return false;
        void (*load)(const JSValue *, uint32_t, JSValue *) = (void *)data->mapping.load_value;
        JSValue element;
        load(view->u.array.u.values, index, &element);
        *result = js_dup(element);
        data->array_hits++;
        return true;
    }
    JSTypedArray *ta = view->u.typed_array;
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    if (!view->fast_array || abuf->shared || abuf->detached || abuf->max_byte_length >= 0)
        return false;

    bool f64 = view->class_id == JS_CLASS_FLOAT64_ARRAY;
    uint32_t length = view->u.array.count;
    void *elements = view->u.array.u.ptr;
    if (plan.kind == QJS_JIT_DATA_INDEX) {
        int32_t index = JS_VALUE_GET_INT(args[plan.bound_arg]);
        if ((uint32_t)index >= length)
            return false;
        if (f64) {
            double (*load)(const void *, uint32_t) = (void *)data->mapping.load_f64;
            *result = js_float64(load(elements, index));
            data->f64_hits++;
        } else {
            int32_t (*load)(const void *, uint32_t) = (void *)data->mapping.load_i32;
            *result = js_int32(load(elements, index));
        }
        data->load_hits++;
        return true;
    }
    if (plan.kind == QJS_JIT_DATA_STORE || plan.kind == QJS_JIT_DATA_STORE_RESULT) {
        uint32_t index = JS_VALUE_GET_INT(args[plan.bound_arg]);
        if (index >= length || abuf->immutable)
            return false;
        JSValue stored = args[plan.value_arg];
        /* Conversions that could execute user code must happen in the
           interpreter. After the native write there are no more guards. */
        if (f64) {
            double unboxed;
            if (!js_arith_to_float64(stored, &unboxed))
                return false;
            void (*store)(void *, uint32_t, double) = (void *)data->mapping.store_f64;
            store(elements, index, unboxed);
            data->f64_hits++;
        } else {
            if (JS_VALUE_GET_TAG(stored) != JS_TAG_INT)
                return false;
            void (*store)(void *, uint32_t, int32_t) = (void *)data->mapping.store_i32;
            store(elements, index, JS_VALUE_GET_INT(stored));
        }
        *result = plan.kind == QJS_JIT_DATA_STORE_RESULT ? stored : JS_UNDEFINED;
        data->store_hits++;
        return true;
    }

    int32_t count;
    if (plan.bound_arg == QJS_JIT_DATA_NO_ARG) {
        if (!qjs_jit_data_intrinsic_length(view))
            return false;
        count = length;
    } else {
        count = JS_VALUE_GET_INT(args[plan.bound_arg]);
    }
    if (count < 0 || (uint32_t)count > length || count > QJS_JIT_DATA_MAX_COUNT)
        return false;
    JSValue seed = plan.value_arg == QJS_JIT_DATA_NO_ARG ? js_int32(0) : args[plan.value_arg];

    if (f64) {
        double unboxed;
        if (plan.kind == QJS_JIT_DATA_SUM_WRAP || !js_arith_to_float64(seed, &unboxed))
            return false;
        double (*sum)(const void *, uint32_t, double) = (void *)data->mapping.sum_f64;
        *result = js_number(sum(elements, count, unboxed));
        data->f64_hits++;
    } else {
        if (JS_VALUE_GET_TAG(seed) != JS_TAG_INT)
            return false;
        /* For at most 2^21 int32 elements plus an int32 seed, every partial
           sum is exact in binary64. For "| 0", addition is modulo 2^32. */
        int64_t (*sum)(const void *, uint32_t) = (void *)data->mapping.sum_i32;
        int64_t total = sum(elements, count) + JS_VALUE_GET_INT(seed);
        if (plan.kind == QJS_JIT_DATA_SUM_WRAP) {
            uint32_t low = (uint32_t)total;
            int32_t wrapped;
            memcpy(&wrapped, &low, sizeof(wrapped));
            *result = js_int32(wrapped);
        } else {
            *result = js_number((double)total);
        }
        if (data->simd && count >= 4)
            data->simd_hits++;
    }
    data->sum_hits++;
    return true;
}

static bool qjs_jit_data_call(JSRuntime *rt, JSFunctionBytecode *b, const JSValue *args,
                              JSValue *result)
{
    if (rt->jit_data_disabled || b->jit_data.kind == QJS_JIT_DATA_UNSUPPORTED ||
        !b->arg_count || b->arg_count > QJS_JIT_DATA_MAX_ARGS ||
        (b->var_count != 0 && b->var_count != 2))
        return false;
    if (b->jit_data.kind == QJS_JIT_DATA_UNKNOWN) {
        QJSJitDataPlan plan;
        if (!qjs_jit_data_plan(b, &plan)) {
            b->jit_data.kind = QJS_JIT_DATA_UNSUPPORTED;
            return false;
        }
        b->jit_data.kind = plan.kind;
        b->jit_data.view_arg = plan.view_arg;
        b->jit_data.bound_arg = plan.bound_arg;
        b->jit_data.value_arg = plan.value_arg;
        b->jit_data.endian_arg = plan.endian_arg;
    }
    JSValue value = args[b->jit_data.view_arg];
    if (b->jit_data.kind == QJS_JIT_DATA_LENGTH) {
        int tag = JS_VALUE_GET_TAG(value);
        if (tag != JS_TAG_STRING && tag != JS_TAG_STRING_ROPE)
            return false;
    } else if (b->jit_data.kind == QJS_JIT_DATA_CHAR_CODE) {
        if (JS_VALUE_GET_TAG(value) != JS_TAG_STRING)
            return false;
    } else {
        if (!JS_IsObject(value))
            return false;
        JSClassID cls = JS_VALUE_GET_OBJ(value)->class_id;
        if (qjs_jit_data_is_dataview(b->jit_data.kind)) {
            if (cls != JS_CLASS_DATAVIEW)
                return false;
        } else if (cls != JS_CLASS_INT32_ARRAY && cls != JS_CLASS_FLOAT64_ARRAY &&
                   !(cls == JS_CLASS_ARRAY && b->jit_data.kind == QJS_JIT_DATA_INDEX)) {
            return false;
        }
    }
    if (b->jit_data.calls < QJS_JIT_BASELINE_THRESHOLD &&
        ++b->jit_data.calls < QJS_JIT_BASELINE_THRESHOLD)
        return false;

    QJSJitData *data = rt->jit_data;
    if (!data || !data->mapping.code) {
        if (data && (rt->jit_cache_tick < data->retry_at ||
                     (data->warm_calls < QJS_JIT_BASELINE_THRESHOLD &&
                      ++data->warm_calls < QJS_JIT_BASELINE_THRESHOLD)))
            return false;
        uint64_t started;
        bool installed = false;
        if (qjs_jit_cache_begin_compile(rt, b, &started)) {
            installed = qjs_jit_data_compile(rt, b);
            qjs_jit_cache_end_compile(rt, started);
        }
        if (!installed) {
            if (data)
                data->retry_at = rt->jit_cache_tick + QJS_JIT_CACHE_COMPILE_RETRY;
            b->jit_data.calls = 0;
            return false;
        }
        data = rt->jit_data;
    }
    QJSJitDataPlan plan = { b->jit_data.kind, b->jit_data.view_arg,
                           b->jit_data.bound_arg, b->jit_data.value_arg, b->jit_data.endian_arg };
    /* Keep every entry pointer valid from its guard through native return. */
    data->active++;
    bool handled = qjs_jit_data_execute(data, b, plan, value, args, result);
    if (handled)
        data->last_used = rt->jit_cache_tick;
    data->active--;
    return handled;
}
