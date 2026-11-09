#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static JSValue retained;
static uint8_t *expected;
static size_t expected_len;
static int captures, polls, abort_poll;
static int interrupt(JSContext *ctx, void *opaque) {
    (void) ctx;
    (void) opaque;
    polls++;
    return abort_poll && polls >= abort_poll;
}
static void fail(JSContext *ctx, const char *what) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    fprintf(stderr, "%s: %s\n", what, s ? s : "no exception");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    exit(1);
}
static JSValue capture(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    size_t len;
    uint8_t *bytes = JS_WriteObject(ctx, &len, retained, JS_WRITE_OBJ_BYTECODE);
    if (!bytes || len != expected_len || memcmp(bytes, expected, len)) {
        if (bytes) {
            js_free(ctx, bytes);
        }
        return JS_ThrowInternalError(ctx, "active region canonical mismatch");
    }
    js_free(ctx, bytes);
    captures++;
    return JS_UNDEFINED;
}
int main(void) {
    const char *source = "function active(x,y,n){if(n) return active(x,y,n-1);"
                         "var a=x+y;var b=a*3;a=b-x;b=a^y;capture();return b+((a<<1)&255);}"
                         "if(active(2,3,12)!==40)throw Error('active result');"
                         "function loop(n){var sum=0;for(var i=0;i<n;i++){var a=i+1;var b=a+2;sum=sum+b;}return sum;}";
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "capture", JS_NewCFunction(ctx, capture, "capture", 0));
    retained =
        JS_Eval(ctx, source, strlen(source), "int-region-native.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(retained)) {
        fail(ctx, "compile");
    }
    expected = JS_WriteObject(ctx, &expected_len, retained, JS_WRITE_OBJ_BYTECODE);
    if (!expected) {
        fail(ctx, "serialize before");
    }
    JSValue value = JS_EvalFunction(ctx, JS_DupValue(ctx, retained));
    if (JS_IsException(value) || captures != 1) {
        fail(ctx, "active frame");
    }
    JS_FreeValue(ctx, value);
    size_t after_len;
    uint8_t *after = JS_WriteObject(ctx, &after_len, retained, JS_WRITE_OBJ_BYTECODE);
    if (!after || after_len != expected_len || memcmp(after, expected, after_len)) {
        fail(ctx, "serialize after");
    }
    js_free(ctx, after);
    // Restored bytecode is separately planned and executes the same active check.
    JSValue loaded = JS_ReadObject(ctx, expected, expected_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(loaded)) {
        fail(ctx, "read");
    }
    value = JS_EvalFunction(ctx, loaded);
    if (JS_IsException(value) || captures != 2) {
        fail(ctx, "read execution");
    }
    JS_FreeValue(ctx, value);
    JSValue fn = JS_GetPropertyStr(ctx, global, "loop"), n = JS_NewInt32(ctx, 20000);
    JS_SetInterruptHandler(rt, interrupt, NULL);
    value = JS_Call(ctx, fn, JS_UNDEFINED, 1, &n);
    int32_t result;
    if (JS_IsException(value) || JS_ToInt32(ctx, &result, value) || result != 200050000) {
        fail(ctx, "loop");
    }
    JS_FreeValue(ctx, value);
    int normal_polls = polls;
    if (normal_polls < 3) {
        fail(ctx, "polls missing");
    }
    polls = 0;
    abort_poll = 2;
    value = JS_Call(ctx, fn, JS_UNDEFINED, 1, &n);
    if (!JS_IsException(value) || polls != 2) {
        fail(ctx, "interrupt abort");
    }
    value = JS_GetException(ctx);
    JS_FreeValue(ctx, value);
    JS_SetInterruptHandler(rt, NULL, NULL);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    js_free(ctx, expected);
    JS_FreeValue(ctx, retained);
    JS_RunGC(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("int region native checks passed normal_polls=%d abort_polls=%d\n", normal_polls, polls);
    return 0;
}
