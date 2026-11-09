#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JSValue code;
static uint8_t *canonical;
static size_t canonical_len;
static int callbacks;
static void check(int ok, const char *why) {
    if (!ok) {
        fprintf(stderr, "%s\n", why);
        exit(1);
    }
}
static JSValue checkpoint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len;
    uint8_t *bytes = JS_WriteObject(ctx, &len, code, JS_WRITE_OBJ_BYTECODE);
    check(bytes && len == canonical_len && !memcmp(bytes, canonical, len), "active-frame bytecode changed");
    js_free(ctx, bytes);
    JS_RunGC(JS_GetRuntime(ctx));
    callbacks++;
    if (argc && JS_ToBool(ctx, argv[0])) {
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, proto, "changed", JS_NewInt32(ctx, 123));
        /* Public API accepts built-in class IDs; OBJECT is the first class. */
        JS_SetClassProto(ctx, 1, proto);
    }
    return JS_NewInt32(ctx, callbacks);
}
static void result_ok(JSContext *ctx, JSValue result) {
    if (JS_IsException(result)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "exception: %s\n", s ? s : "?");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        exit(2);
    }
    JS_FreeValue(ctx, result);
}
int main(int argc, char **argv) {
    const char *src =
        "function descend(n) { return n ? make(n-1,false) : leaf(); } function leaf(){ return {x:1,y:2}; } "
        "function make(n,change) { return {a: checkpoint(change), b: descend(n), c: checkpoint(false)}; }"
        "globalThis.make=make;";
    const char *calls =
        "let oldProto=Object.getPrototypeOf(make(0,false));"
        "let outer=make(3,true);"
        "if(Object.getPrototypeOf(outer)!==oldProto) throw Error('creation prototype changed');"
        "if(Object.getPrototypeOf(outer.b).changed!==123) throw Error('recursive new prototype');"
        "if(Object.keys(outer).join(',')!=='a,b,c'||outer.b.b.b.b.x!==1) throw Error('values');"
        "let now=make(0,false); if(Object.getPrototypeOf(now).changed!==123) throw Error('cached prototype');"
        "delete now.b; now.d=9; Object.defineProperty(now,'a',{get(){return 7}});"
        "if(now.a!==7||make(0,false).b.y!==2) throw Error('mutation');";
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    check(rt && ctx, "runtime");
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "checkpoint", JS_NewCFunction(ctx, checkpoint, "checkpoint", 1));
    JS_FreeValue(ctx, global);
    code = JS_Eval(ctx, src, strlen(src), "native-object-layout.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    check(!JS_IsException(code), "compile");
    canonical = JS_WriteObject(ctx, &canonical_len, code, JS_WRITE_OBJ_BYTECODE);
    check(canonical != NULL, "serialize");
    result_ok(ctx, JS_EvalFunction(ctx, JS_DupValue(ctx, code)));
    result_ok(ctx, JS_Eval(ctx, calls, strlen(calls), "native-calls.js", JS_EVAL_TYPE_GLOBAL));
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "wb");
        check(f && fwrite(canonical, 1, canonical_len, f) == canonical_len, "write peer bytes");
        fclose(f);
    }
    if (argc >= 3) {
        FILE *f = fopen(argv[2], "rb");
        check(f && !fseek(f, 0, SEEK_END), "open peer bytes");
        long len = ftell(f);
        check(len > 0 && !fseek(f, 0, SEEK_SET), "peer size");
        uint8_t *bytes = malloc((size_t) len);
        check(bytes && fread(bytes, 1, (size_t) len, f) == (size_t) len, "read peer");
        fclose(f);
        JSValue peer = JS_ReadObject(ctx, bytes, (size_t) len, JS_READ_OBJ_BYTECODE);
        free(bytes);
        check(!JS_IsException(peer), "deserialize peer");
        result_ok(ctx, JS_EvalFunction(ctx, peer));
        result_ok(ctx, JS_Eval(ctx, "make(2,false)", 13, "peer-calls.js", JS_EVAL_TYPE_GLOBAL));
    }
    JSContext *other = JS_NewContext(rt);
    check(other != NULL, "second realm");
    JSValue other_proto = JS_NewObject(other);
    JS_SetPropertyStr(other, other_proto, "changed", JS_NewInt32(other, 999));
    JS_SetClassProto(other, 1, other_proto);
    JSValue own_global = JS_GetGlobalObject(ctx);
    JSValue maker = JS_GetPropertyStr(ctx, own_global, "make");
    JSValue args[] = { JS_NewInt32(ctx, 0), JS_FALSE };
    JSValue cross = JS_Call(other, maker, JS_UNDEFINED, 2, args);
    check(!JS_IsException(cross), "cross-realm call");
    JSValue cross_proto = JS_GetPrototype(other, cross);
    JSValue changed = JS_GetPropertyStr(other, cross_proto, "changed");
    int32_t marker;
    check(JS_ToInt32(other, &marker, changed) == 0 && marker == 123, "bytecode realm prototype");
    JS_FreeValue(other, changed);
    JS_FreeValue(other, cross_proto);
    JS_FreeValue(other, cross);
    JS_FreeValue(ctx, maker);
    JS_FreeValue(ctx, own_global);
    JS_FreeContext(other);
    JS_FreeValue(ctx, code);
    js_free(ctx, canonical);
    JS_RunGC(rt);
    JS_FreeContext(ctx);
    JS_RunGC(rt);
    JS_FreeRuntime(rt);
    printf("object native GC/prototype/active serialization passed (%d callbacks)\n", callbacks);
    return 0;
}
