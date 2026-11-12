import assert from 'tjs:assert';

function values(input) {
    return [ 0n, -17n, 2147483647n, -2147483648n, 2147483648n, input + 37n ];
}

function capture(first, second) {
    const getters = [];

    for (let i = 0; i < 2; i++) {
        let value = i ? second : first;

        getters.push(replacement => {
            const previous = value;

            if (replacement) {
                value = replacement;
            }

            return previous;
        });
    }

    return getters;
}

function exercise() {
    for (let i = 0; i < 128; i++) {
        assert.eq(values(BigInt(i)), [ 0n, -17n, 2147483647n, -2147483648n, 2147483648n, BigInt(i + 37) ]);

        const first = { i };
        const second = { i: -i };
        const [ left, right ] = capture(first, second);

        assert.is(left(), first);
        assert.is(right(), second);
    }

    const first = { value: 1 };
    const second = { value: 2 };
    const replacement = { value: 3 };
    const [ left, right ] = capture(first, second);

    tjs.engine.gc.run();
    assert.is(left(replacement), first);
    tjs.engine.gc.run();
    assert.is(left(), replacement);
    assert.is(right(), second);
    assert.throws(() => values(1), TypeError);
    console.log('boxed short BigInt and lexical close semantics verified');
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_SHADOW: '0',
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

if (tjs.args.includes('child')) {
    exercise();
} else {
    const control = await run('0');
    const boxed = await run('baseline');

    assert.eq(boxed.stdout, control.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        for (const counter of [ 'boxed_short_bigints', 'boxed_loc_closes' ]) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
        }
    }
}
