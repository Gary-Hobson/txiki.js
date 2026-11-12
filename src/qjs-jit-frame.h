/* Interpreter frame values at a guard, before the guarded opcode executes. */
enum {
    QJS_JIT_MAX_LOCALS = 32,
    QJS_JIT_MAX_STACK = 32,
    QJS_JIT_EXIT_PRECISE_GUARD = 3,
    QJS_JIT_EXIT_PRECISE_FUEL = 4,
};

typedef struct {
    double result;
    uint32_t pc;
    uint32_t depth;
    uint32_t initialized;
    uint32_t uninitialized;
    double locals[QJS_JIT_MAX_LOCALS];
    double stack[QJS_JIT_MAX_STACK];
} QJSJitF64Exit;

typedef struct {
    int32_t result;
    uint32_t pc;
    uint32_t depth;
    uint32_t initialized;
    uint32_t uninitialized;
    int32_t locals[QJS_JIT_MAX_LOCALS];
    int32_t stack[QJS_JIT_MAX_STACK];
    QJSJitF64Exit *f64_exit;
} QJSJitExit;
