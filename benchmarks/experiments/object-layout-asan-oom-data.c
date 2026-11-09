#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define usable malloc_size
#else
#include <malloc.h>
#define usable malloc_usable_size
#endif
struct State {
    size_t calls, fail_at;
    int injected;
};
static int fail(struct State *s) {
    if (s->fail_at && ++s->calls == s->fail_at) {
        s->injected = 1;
        return 1;
    }
    return 0;
}
static void *alloc(void *o, size_t n) {
    return fail(o) ? NULL : malloc(n);
}
static void *zero(void *o, size_t n, size_t m) {
    return fail(o) ? NULL : calloc(n, m);
}
static void release(void *o, void *p) {
    (void) o;
    free(p);
}
static void *resize(void *o, void *p, size_t n) {
    return fail(o) ? NULL : realloc(p, n);
}
static size_t available(const void *p) {
    return usable(p);
}
static const JSMallocFunctions mf = { zero, alloc, release, resize, available };
static const char *src =
    "globalThis.effects=[]; function leaf(n){effects.push(n); return {index:n,payload:String(n).repeat(256)}}"
    "function make(n){return {a:leaf(1),b:leaf(2),c:leaf(3),d:leaf(4),e:leaf(5),f:leaf(6),g:leaf(7),h:leaf(8),"
    "i:leaf(9),j:leaf(10),k:leaf(11),l:leaf(12),m:leaf(13),n:leaf(14),o:leaf(15),p:leaf(16),"
    "q:leaf(17),r:leaf(18),s:leaf(19),t:leaf(20),u:leaf(21),v:leaf(22),w:leaf(23),x:leaf(24),"
    "y:leaf(25),z:leaf(26),aa:leaf(27),ab:leaf(28),ac:leaf(29),ad:leaf(30),ae:leaf(31),af:leaf(32)}}"
    ";";
int main(void) {
    unsigned exceptions = 0, injections = 0;
    for (size_t index = 1; index <= 180; index++) {
        fprintf(stderr, "OOM_POINT %zu\n", index);
        struct State state = { 0 };
        JSRuntime *rt = JS_NewRuntime2(&mf, &state);
        JSContext *ctx = JS_NewContext(rt);
        if (!rt || !ctx) {
            return 1;
        }
        JSValue code = JS_Eval(ctx, src, strlen(src), "oom-layout.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(code)) {
            return 2;
        }
        JSValue setup = JS_EvalFunction(ctx, code);
        if (JS_IsException(setup)) {
            return 2;
        }
        JS_FreeValue(ctx, setup);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue maker = JS_GetPropertyStr(ctx, global, "make");
        JS_FreeValue(ctx, global);
        state.fail_at = index;
        JSValue out = JS_Call(ctx, maker, JS_UNDEFINED, 0, NULL);
        state.fail_at = 0;
        JS_FreeValue(ctx, maker);
        injections += state.injected;
        if (JS_IsException(out)) {
            exceptions++;
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        } else {
            JS_FreeValue(ctx, out);
        }
        JS_RunGC(rt);
        const char *recovery =
            "if(({a:1,b:2,c:3}).c!==3)throw Error('recovery'); if(Array.isArray(globalThis.effects))for(let "
            "i=1;i<effects.length;i++)if(effects[i]!==effects[i-1]+1)throw Error('side effects');";
        out = JS_Eval(ctx, recovery, strlen(recovery), "recovery.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(out)) {
            fprintf(stderr, "recovery failed at %zu\n", index);
            return 3;
        }
        JS_FreeValue(ctx, out);
        JS_FreeContext(ctx);
        JS_RunGC(rt);
        JS_FreeRuntime(rt);
    }
    printf("Data-initializer OOM injection passed: points=180 injected=%u exceptions=%u\n", injections, exceptions);
    return 0;
}
