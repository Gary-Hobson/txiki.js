import assert from 'tjs:assert';

function copyAtJoin(left, right, choose) {
    let value = choose ? left : right;
    const alias = value;

    value = alias;

    return value;
}

function subtractAlias(left, right, choose) {
    let value = choose ? left : right;
    const alias = value;

    return value - alias;
}

function copyDistinctPhis(left, right, choose) {
    let first;
    let second;

    if (choose) {
        first = left;
        second = right;
    } else {
        first = right;
        second = left;
    }

    if (first < 0) {
        return first;
    }

    first = second;

    return first;
}

function copyInLoop(initial, next, iterations) {
    let value = initial;

    for (let i = 0; i < iterations; i++) {
        let candidate = i < 1 ? next : value;
        const alias = candidate;

        candidate = alias;
        value = candidate;
    }

    return value;
}

function exercise() {
    assert.ok(Object.is(copyInLoop(0.5, -0, 512), -0));

    let checksum = 0;

    for (let i = 0; i < 480; i++) {
        const choose = i & 1;

        checksum += copyAtJoin(1.25, 2.5, choose);
        assert.ok(Object.is(subtractAlias(1.25, 2, choose), 0));
        assert.eq(copyDistinctPhis(1.25, 2, choose), choose ? 2 : 1.25);
        assert.ok(Object.is(copyInLoop(0.5, -0, 4), -0));
    }

    assert.ok(Object.is(copyAtJoin(-0, 0, 1), -0));
    assert.ok(Object.is(copyAtJoin(-0, 0, 0), 0));
    assert.ok(Number.isNaN(copyAtJoin(NaN, Infinity, 1)));
    assert.eq(copyAtJoin(NaN, Infinity, 0), Infinity);
    assert.ok(Number.isNaN(subtractAlias(Infinity, 1.25, 1)));
    assert.ok(Number.isNaN(subtractAlias(NaN, 1.25, 1)));
    assert.ok(Object.is(subtractAlias(-0, 1.25, 1), 0));
    assert.ok(Object.is(copyDistinctPhis(-0, 0, 0), -0));
    assert.eq(copyDistinctPhis(-3.25, 2, 1), -3.25);
    console.log(checksum);
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: { ...tjs.env, TJS_QUICKJS_JIT: mode, TJS_QUICKJS_JIT_STATS: '1' },
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
    const baseline = await run('baseline');
    const optimized = await run('1');

    assert.eq(baseline.stdout, interpreter.stdout);
    assert.eq(optimized.stdout, interpreter.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bssa_f64_phis=0 ssa_f64_stores_elided=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(/\bf64_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bssa_f64_phis=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bssa_f64_stores_elided=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
    }
}
