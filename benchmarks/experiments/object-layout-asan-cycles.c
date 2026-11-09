#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int ok, const char *why) {
    if (!ok) {
        fprintf(stderr, "%s\n", why);
        exit(1);
    }
}
int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    check(rt && ctx, "runtime");
    JSValue original = JS_GetClassProto(ctx, 1);
    JSMemoryUsage before, after;
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &before);
    const char *src = "(function make(){return {a:1,b:2,c:3}})";
    for (int i = 0; i < 50; i++) {
        JSValue proto = JS_NewObject(ctx);
        JSValue maker = JS_Eval(ctx, src, strlen(src), "cycle-layout.js", JS_EVAL_TYPE_GLOBAL);
        check(!JS_IsException(proto) && !JS_IsException(maker), "cycle setup");
        check(JS_SetPropertyStr(ctx, proto, "maker", JS_DupValue(ctx, maker)) >= 0, "proto maker");
        JS_SetClassProto(ctx, 1, JS_DupValue(ctx, proto));
        JSValue value = JS_Call(ctx, maker, JS_UNDEFINED, 0, NULL);
        check(!JS_IsException(value), "create template");
        JS_FreeValue(ctx, value);
        JS_SetClassProto(ctx, 1, JS_DupValue(ctx, original));
        JS_FreeValue(ctx, maker);
        JS_FreeValue(ctx, proto);
        JS_RunGC(rt);
        JS_ComputeMemoryUsage(rt, &after);
        check(after.js_func_count == before.js_func_count, "bytecode cycle not collected");
        check(after.shape_count == before.shape_count, "template shape cycle not collected");
        check(after.obj_count == before.obj_count, "prototype/function cycle not collected");
    }
    JS_FreeValue(ctx, original);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts("50 shape-prototype-function-bytecode cycles collected exactly");
    return 0;
}
