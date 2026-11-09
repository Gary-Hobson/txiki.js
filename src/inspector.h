#ifndef TJS_INSPECTOR_H
#define TJS_INSPECTOR_H

#include <quickjs.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TJSInspector TJSInspector;

TJSInspector *tjs__inspector_start(JSContext *ctx, uv_loop_t *loop, int port, bool wait);
void tjs__inspector_stop(TJSInspector *inspector);

#ifdef __cplusplus
}
#endif

#endif
