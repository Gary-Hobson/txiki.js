<p align="center">
    <img width="240" src="https://raw.githubusercontent.com/saghul/txiki.js/master/website/static/img/logo-heartonly.png" />
</p>

# txiki.js — The tiny JavaScript runtime

> **txiki** /ˈtʃiki/ (Basque): small, tiny.

*txiki.js* is a small and powerful JavaScript runtime. It targets state-of-the-art ECMAScript
and aims to be [WinterTC] compliant.

It's built on the shoulders of giants: it uses [QuickJS-ng] as its JavaScript engine and [libuv] as the platform layer.

## Quick start

```bash
# Get the code
git clone --recursive https://github.com/saghul/txiki.js --shallow-submodules && cd txiki.js
# Compile it!
make
# Run the REPL
./build/tjs
```

See [Building](https://txikijs.org/docs/building) for detailed instructions including Windows support.

### Experimental QuickJS JIT

On AArch64 macOS and Linux, configure CMake with `-DBUILD_WITH_QUICKJS_JIT=ON`.
Set `TJS_QUICKJS_JIT=1` to enable both native tiers or
`TJS_QUICKJS_JIT=baseline` to enable only the first tier. The JIT compiles
eligible integer and floating-point functions and can enter machine code from
a running loop. Integer guard exits and scalar loop-budget exits can restore the
interpreter's program counter, locals, and operand stack at supported sites.
Set `TJS_QUICKJS_JIT_STATS=1` to report tier and fallback counts on exit.
Both the build option and runtime execution default to off.

Set `TJS_QUICKJS_JIT_FEEDBACK=1` to sample numeric operands, branch decisions,
call targets, and array element accesses, and to enable own-property inline caches.
Use `TJS_QUICKJS_JIT_FEEDBACK=dump` for detailed per-site diagnostics.
With feedback enabled, a stable call site can enter a ready scalar callee
directly. Set `TJS_QUICKJS_JIT_BOXED=1` alongside JIT mode to enable the
experimental boxed Baseline for supported dynamic bytecodes.
Set `TJS_QUICKJS_JIT_DATA=0` to disable guarded TypedArray reads, writes,
and reductions, fixed-buffer DataView reads and writes, dense-array reads,
and primitive string length and `charCodeAt` operations.
JIT mode also reuses small-integer strings when appending them to a nonempty
flat string. Long concatenations continue to use QuickJS's rope representation.
Set `TJS_QUICKJS_JIT_SHADOW=1` to compare successful pure scalar native calls
with interpreter results, including NaN and signed zero. Shadow mode uses
function-entry compilation and runs effectful bytecodes through the interpreter.

Each runtime budgets 8 MiB of executable code, including a 2 MiB partition
for boxed and coroutine code. Under pressure the cache scans a bounded number
of entries and can reclaim cold mappings in each tier. Active mappings stay
pinned across helper calls, and reclaimed code can compile again after warming
up. Compilation admits up to 12 attempts or 5 ms of accumulated work per 512
eligible dispatches. Deferred compilation can retry on a later dispatch.

```bash
cmake -S . -B build -DBUILD_WITH_QUICKJS_JIT=ON
cmake --build build --target tjs
for test in tests/test-quickjs-jit*.js; do
    TJS_QUICKJS_JIT_REQUIRED=1 ./build/tjs run "$test" || break
done
TJS_QUICKJS_JIT=1 ./build/tjs run benchmarks/experiments/quickjs-jit-numeric.js
TJS_QUICKJS_JIT=1 ./build/tjs run benchmarks/experiments/quickjs-jit-numeric.js constants
```

## Features

- Web Platform APIs: `fetch`, `WebSocket`, `Console`, `setTimeout`, `Crypto`, Web Workers, and more
- TCP, UDP, and Unix sockets
- HTTP server with WebSocket support
- File I/O, child processes, signal handling
- Standard library: `tjs:sqlite`, `tjs:ffi`, `tjs:path`, `tjs:hashing`, and more
- Standalone executables via `tjs compile`

## Documentation

Full documentation is available at **[txikijs.org](https://txikijs.org)**.

## Supported platforms

* GNU/Linux
* macOS
* Windows
* Other Unixes (please test!)

<br />

<footer>
<p align="center" style="font-size: smaller;">
Built with ❤️ by saghul and these awesome <a href="https://github.com/saghul/txiki.js/graphs/contributors" target="_blank">contributors</a>.
</footer>

[QuickJS-ng]: https://github.com/quickjs-ng/quickjs
[libuv]: https://libuv.org/
[WinterTC]: https://wintertc.org/
