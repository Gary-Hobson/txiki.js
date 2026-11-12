import assert from 'tjs:assert';

function copy(value, other) {
    const local = value;

    return [ local, other ];
}

function discard(factory) {
    factory();

    return true;
}

function argumentTypes(value) {
    return [ typeof value, typeof value === 'undefined', typeof value === 'function',
        // eslint-disable-next-line eqeqeq -- Exercise the dedicated nullish comparison opcode.
        value === undefined, value === null, value == null ];
}

function localType(value, keep) {
    let local;

    if (keep) {
        local = value;
    } else {
        local = null;
    }

    return typeof local;
}

function closureType(value) {
    var captured = value;

    return () => typeof captured;
}

function checkedType(early) {
    if (early) {
        return typeof local;
    }

    let local = {};

    return typeof local;
}

function exercise() {
    const token = { data: 'retained' };
    const symbol = Symbol('retained');
    const callable = Proxy.revocable(() => {}, {});

    callable.revoke();

    const values = [ token, symbol, 2n ** 110n, 'a'.repeat(600) + 'b'.repeat(600),
        NaN, -0, 5, undefined, null, true, callable.proxy ];
    const types = [ 'object', 'symbol', 'bigint', 'string', 'number', 'number', 'number',
        'undefined', 'object', 'boolean', 'function' ];
    const closureTypes = values.map(closureType);
    let made = 0;

    function fresh() {
        made++;

        return { index: made };
    }

    function reused() {
        return token;
    }

    for (let i = 0; i < 320; i++) {
        assert.ok(discard(fresh));
        assert.ok(discard(reused));
        assert.eq(checkedType(false), 'object');
        assert.eq(localType(token, false), 'object');

        for (let j = 0; j < values.length; j++) {
            const item = values[j];
            const [ left, right ] = copy(item, token);

            assert.ok(Object.is(left, item) && right === token);
            assert.eq(argumentTypes(item), [ types[j], item === undefined, types[j] === 'function',
                item === undefined, item === null, item === null || item === undefined ]);
            assert.eq(localType(item, true), types[j]);
            assert.eq(closureTypes[j](), types[j]);
        }
    }

    assert.throws(() => checkedType(true), ReferenceError);
    assert.eq(token.data, 'retained');
    assert.eq(made, 320);
    console.log(made);
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_STATS: '1',
        },
    });
    const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

    try {
        const [ status, stdout, stderr ] = await Promise.all([ proc.wait(), proc.stdout.text(), proc.stderr.text() ]);

        assert.eq(status.term_signal, null, stderr);
        assert.eq(status.exit_status, 0, stderr);

        return { stdout, stderr };
    } finally {
        clearTimeout(watchdog);
    }
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    const interpreter = await run('0');
    const native = await run('1');

    assert.eq(native.stdout, interpreter.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bboxed_native_increfs=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_native_decrefs=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_native_drop_fallbacks=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_rc_pairs_elided=[1-9]\d*\b/.test(native.stderr), native.stderr);
    }
}
