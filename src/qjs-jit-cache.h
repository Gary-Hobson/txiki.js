/* Included by qjs-jit.h. The owning QuickJS runtime serializes cache access. */
enum {
    QJS_JIT_CACHE_EPOCH = 512,
    QJS_JIT_CACHE_COLD_AGE = 4 * QJS_JIT_CACHE_EPOCH,
    QJS_JIT_CACHE_COMPILE_LIMIT = 12,
    QJS_JIT_CACHE_COMPILE_NS = 5 * 1000 * 1000,
    QJS_JIT_CACHE_SCAN_LIMIT = 8,
    QJS_JIT_CACHE_COMPILE_RETRY = 32,
    QJS_JIT_CACHE_BOXED_LIMIT = QJS_JIT_CODE_LIMIT / 4,
};

static void qjs_jit_cache_tick(JSRuntime *rt)
{
    rt->jit_cache_tick++;
}

static uint64_t qjs_jit_cache_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static bool qjs_jit_cache_begin_compile(JSRuntime *rt, const JSFunctionBytecode *b,
                                        uint64_t *started)
{
    uint64_t epoch = rt->jit_cache_tick / QJS_JIT_CACHE_EPOCH;
    if (epoch != rt->jit_cache_epoch) {
        rt->jit_cache_epoch = epoch;
        rt->jit_cache_attempts = 0;
        rt->jit_cache_compile_ns = 0;
    }
    if (b->jit_cache_active || rt->jit_cache_attempts >= QJS_JIT_CACHE_COMPILE_LIMIT ||
        rt->jit_cache_compile_ns >= QJS_JIT_CACHE_COMPILE_NS) {
        return false;
    }
    rt->jit_cache_attempts++;
    *started = qjs_jit_cache_now();
    return true;
}

static void qjs_jit_cache_end_compile(JSRuntime *rt, uint64_t started)
{
    uint64_t finished = qjs_jit_cache_now();
    if (started && finished >= started)
        rt->jit_cache_compile_ns += finished - started;
}

static void qjs_jit_cache_unlink(JSRuntime *rt, JSFunctionBytecode *b)
{
    JSFunctionBytecode *previous = b->jit_cache_prev;
    JSFunctionBytecode *next = b->jit_cache_next;
    if (previous)
        previous->jit_cache_next = next;
    else
        rt->jit_cache_oldest = next;
    if (next)
        next->jit_cache_prev = previous;
    else
        rt->jit_cache_newest = previous;
    b->jit_cache_prev = NULL;
    b->jit_cache_next = NULL;
}

static void qjs_jit_cache_link(JSRuntime *rt, JSFunctionBytecode *b)
{
    b->jit_cache_used = rt->jit_cache_tick;
    b->jit_cache_prev = rt->jit_cache_newest;
    if (rt->jit_cache_newest)
        rt->jit_cache_newest->jit_cache_next = b;
    else
        rt->jit_cache_oldest = b;
    rt->jit_cache_newest = b;
}

static void qjs_jit_cache_remove(JSRuntime *rt, JSFunctionBytecode *b)
{
    if (!b->jit_code)
        return;
    assert(!b->jit_cache_active);
    qjs_jit_cache_unlink(rt, b);
    munmap(b->jit_code, b->jit_code_size);
    rt->jit_code_bytes -= b->jit_code_size;
    rt->jit_cache_scalar_bytes -= b->jit_code_size;
    rt->malloc_state.malloc_size -= b->jit_code_size;
    rt->malloc_state.malloc_count--;
    b->jit_code = NULL;
    b->jit_code_size = 0;
    b->jit_osr_pc = 0;
    b->jit_osr_word = 0;
    b->jit_osr_locals = 0;
}

static bool qjs_jit_cache_has_space(const JSRuntime *rt, size_t size)
{
    const JSMallocState *ms = &rt->malloc_state;
    assert(rt->jit_code_bytes <= QJS_JIT_CODE_LIMIT);
    return size <= QJS_JIT_CODE_LIMIT - rt->jit_code_bytes &&
           (!ms->malloc_limit || (ms->malloc_size < ms->malloc_limit &&
                                 size < ms->malloc_limit - ms->malloc_size));
}

static bool qjs_jit_data_cache_evict(JSRuntime *rt);
static bool qjs_boxed_cache_reclaim(JSRuntime *rt, size_t size, bool boxed_request);

static bool qjs_jit_cache_reserve(JSRuntime *rt, const JSFunctionBytecode *requester, size_t size)
{
    if (qjs_jit_cache_has_space(rt, size))
        return true;
    if (size > QJS_JIT_CODE_LIMIT ||
        (rt->malloc_state.malloc_limit && size >= rt->malloc_state.malloc_limit))
        return false;
    if (qjs_jit_data_cache_evict(rt) && qjs_jit_cache_has_space(rt, size))
        return true;

    JSFunctionBytecode *candidate = rt->jit_cache_oldest;
    for (unsigned i = 0; i < QJS_JIT_CACHE_SCAN_LIMIT && candidate; i++) {
        JSFunctionBytecode *next = candidate->jit_cache_next;
        if (rt->jit_cache_tick - candidate->jit_cache_used < QJS_JIT_CACHE_COLD_AGE)
            break;
        if (candidate != requester && !candidate->jit_cache_active) {
            qjs_jit_cache_remove(rt, candidate);
            candidate->jit_hot_calls = 0;
            candidate->jit_hot_backedges = 0;
            candidate->jit_baseline_hits = 0;
            candidate->jit_optimization_attempted = false;
            candidate->jit_optimized = false;
            candidate->jit_cache_was_evicted = true;
            rt->jit_cache_evictions++;
            if (qjs_jit_cache_has_space(rt, size))
                return true;
        }
        candidate = next;
    }
    return qjs_boxed_cache_reclaim(rt, size, false);
}

static bool qjs_jit_cache_install(JSRuntime *rt, JSFunctionBytecode *b, const uint32_t *words,
                                  size_t bytes, QJSJitOSR osr)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || page_size > QJS_JIT_CODE_LIMIT)
        return false;
    size_t size = (bytes + page_size - 1) / page_size * page_size;
    if (!qjs_jit_cache_reserve(rt, b, size))
        return false;

    void *code = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED)
        return false;
    memcpy(code, words, bytes);
    __builtin___clear_cache(code, (char *)code + bytes);
    if (mprotect(code, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, size);
        return false;
    }

    qjs_jit_cache_remove(rt, b);
    b->jit_code = code;
    b->jit_code_size = size;
    b->jit_osr_pc = osr.pc;
    b->jit_osr_word = osr.word_offset;
    b->jit_osr_locals = osr.locals_mask;
    qjs_jit_cache_link(rt, b);
    rt->jit_code_bytes += size;
    rt->jit_cache_scalar_bytes += size;
    rt->malloc_state.malloc_size += size;
    rt->malloc_state.malloc_count++;
    if (b->jit_cache_was_evicted) {
        b->jit_cache_was_evicted = false;
        rt->jit_cache_reinstalls++;
    }
    return true;
}

static void qjs_jit_cache_enter(JSRuntime *rt, JSFunctionBytecode *b)
{
    b->jit_cache_active++;
    /* Batch list writes when two hot functions alternate at the same site. */
    if (rt->jit_cache_tick - b->jit_cache_used >= QJS_JIT_CACHE_EPOCH) {
        qjs_jit_cache_unlink(rt, b);
        qjs_jit_cache_link(rt, b);
    }
}

static void qjs_jit_cache_leave(JSFunctionBytecode *b)
{
    assert(b->jit_cache_active);
    b->jit_cache_active--;
}
