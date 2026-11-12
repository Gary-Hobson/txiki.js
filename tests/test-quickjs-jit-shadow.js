import assert from 'tjs:assert';

function integer(a, b) {
    return (a + b * 2) ^ 5;
}

function floating(a, b) {
    return a / b + a % b;
}

if (tjs.args[3] === 'shadow-child') {
    for (let i = 0; i < 512; i++) {
        assert.eq(integer(i, 3), (i + 6) ^ 5);
        assert.eq(floating(i + 0.5, 2), (i + 0.5) / 2 + (i + 0.5) % 2);
    }

    assert.ok(Object.is(floating(-0, 2), -0));
    assert.ok(Number.isNaN(floating(Infinity, 2)));

    let conversions = 0;
    const value = {
        valueOf() {
            conversions++;

            return 9;
        },
    };

    assert.eq(integer(value, 3), 10);
    assert.eq(conversions, 1);
} else {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'shadow-child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: '1',
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_FEEDBACK: '1',
            TJS_QUICKJS_JIT_SHADOW: '1',
            TJS_QUICKJS_JIT_STATS: '1',
        },
    });
    const [ status, stdout, stderr ] = await Promise.all([ proc.wait(), proc.stdout.text(), proc.stderr.text() ]);

    assert.eq(status.term_signal, null, stderr);
    assert.eq(status.exit_status, 0, stdout + stderr);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bshadow_checks=[1-9]\d*\b/.test(stderr), stderr);
        assert.ok(/\bf64_hits=[1-9]\d*\b/.test(stderr), stderr);
        assert.ok(/\bboxed_entries=0\b/.test(stderr), stderr);
    }
}
