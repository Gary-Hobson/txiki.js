#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int polls;
static int count_poll(JSContext *ctx, void *opaque) {
    (void) ctx;
    (void) opaque;
    polls++;
    return 0;
}
static int abort_poll(JSContext *ctx, void *opaque) {
    (void) ctx;
    (void) opaque;
    return 1;
}
static void check(int ok, const char *why) {
    if (!ok) {
        fprintf(stderr, "%s\n", why);
        exit(1);
    }
}
static JSValue prototype(JSContext *ctx, const char *name) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, name);
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return proto;
}
static void clear_exception(JSContext *ctx) {
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *callee = JS_NewContext(rt), *caller = JS_NewContext(rt);
    check(rt && callee && caller, "contexts");
    JSValue proto = prototype(callee, "String");
    JSValue method = JS_GetPropertyStr(callee, proto, "charCodeAt");
    JS_FreeValue(callee, proto);
    JSValue str = JS_NewString(caller, "abcdef");
    JSValue index = JS_NewInt32(caller, 2);
    JS_SetInterruptHandler(rt, count_poll, NULL);
    for (int i = 0; i < 50000; i++) {
        JSValue value = JS_Call(caller, method, str, 1, &index);
        int32_t code = -1;
        check(!JS_IsException(value) && JS_ToInt32(caller, &code, value) == 0 && code == 99,
              "cross-realm pure numeric call");
        JS_FreeValue(caller, value);
    }
    check(polls == 5, "original 10000-call interrupt cadence");
    JS_SetInterruptHandler(rt, abort_poll, NULL);
    int interrupted = 0;
    for (int i = 0; i < 10001; i++) {
        JSValue value = JS_Call(caller, method, str, 1, &index);
        if (JS_IsException(value)) {
            clear_exception(caller);
            interrupted = 1;
            break;
        }
        JS_FreeValue(caller, value);
    }
    check(interrupted, "interrupt can stop repeated intrinsic calls");
    JS_SetInterruptHandler(rt, NULL, NULL);

    JS_SetMaxStackSize(rt, 1);
    JSValue value = JS_Call(caller, method, str, 1, &index);
    check(JS_IsException(value), "original native stack check retained");
    clear_exception(caller);
    JS_SetMaxStackSize(rt, 512 * 1024);

    // A non-int argument must enter the original callee realm and C frame.
    const char *symbol_source = "Symbol('index')";
    JSValue symbol = JS_Eval(caller, symbol_source, strlen(symbol_source), "intrinsic-native.js", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(symbol), "symbol setup");
    value = JS_Call(caller, method, str, 1, &symbol);
    check(JS_IsException(value), "symbol conversion throws");
    JSValue error = JS_GetException(caller);
    JSValue actual = JS_GetPrototype(caller, error);
    JSValue expected = prototype(callee, "TypeError");
    check(JS_IsStrictEqual(caller, actual, expected), "fallback preserves callee error realm");
    JS_FreeValue(caller, actual);
    JS_FreeValue(callee, expected);
    JS_FreeValue(caller, error);
    JS_FreeValue(caller, symbol);

    value = JS_CallConstructor(caller, method, 1, &index);
    check(JS_IsException(value), "builtin remains non-constructor");
    clear_exception(caller);
    JS_FreeValue(caller, str);
    JS_FreeValue(callee, method);
    JS_RunGC(rt);  // Emits actual fast-path count in diagnostic builds.
    JS_FreeContext(caller);
    JS_FreeContext(callee);
    JS_FreeRuntime(rt);
    printf("string intrinsic native checks passed polls=%d\n", polls);
    return 0;
}
