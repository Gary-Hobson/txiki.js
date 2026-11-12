import assert from 'tjs:assert';

function directEval(source, initial) {
    let captured = initial;
    const result = eval(source);

    return [ result, captured ];
}

function spreadEval(args, initial) {
    let captured = initial;
    const result = eval(...args);

    return [ result, captured ];
}

function directEmptyEval() {
    return eval();
}

function applyMethod(receiver, args) {
    return receiver.method(...args);
}

function applyConstructor(target, args) {
    return new target(...args);
}

function Constructor(value) {
    this.value = value;
    this.target = new.target;
}

function caught(fn) {
    try {
        fn();
    } catch (error) {
        return error;
    }
}

function exercise() {
    const receiver = {
        value: 7,
        method(a, b) {
            return this.value + a * b;
        },
    };

    for (let i = 0; i < 100; i++) {
        assert.eq(directEval('captured += 3', i), [ i + 3, i + 3 ]);
        assert.eq(spreadEval([ 'captured *= 2', 'ignored' ], i), [ i * 2, i * 2 ]);
        assert.eq(directEmptyEval(), undefined);
        assert.eq(applyMethod(receiver, [ i, 2 ]), 7 + i * 2);

        const obj = applyConstructor(Constructor, [ i ]);

        assert.ok(obj instanceof Constructor);
        assert.eq(obj.value, i);
        assert.is(obj.target, Constructor);
    }

    const marker = new Error('call sentinel');
    const [ closure ] = directEval('x => x + captured', 7);

    assert.eq(closure(9), 16);
    assert.eq(spreadEval([], 12), [ undefined, 12 ]);
    assert.is(caught(() => directEval('throw captured', marker)), marker);
    assert.is(caught(() => spreadEval([ 'throw captured' ], marker)), marker);
    assert.is(caught(() => applyMethod({ method() {
        throw marker;
    } }, [])), marker);
    assert.ok(caught(() => applyConstructor(() => {}, [])) instanceof TypeError);

    const originalEval = globalThis.eval;

    globalThis.eval = (a, b) => [ a, b ];

    try {
        assert.eq(directEval('captured', 1), [ [ 'captured', undefined ], 1 ]);
        assert.eq(spreadEval([ 'a', 'b' ], 2), [ [ 'a', 'b' ], 2 ]);
    } finally {
        globalThis.eval = originalEval;
    }

    console.log('100 direct eval, spread eval, method and constructor calls');
}

async function run(jit) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: jit,
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
        assert.ok(/\bboxed_applies=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_evals=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_apply_evals=[1-9]\d*\b/.test(native.stderr), native.stderr);
    }
}
