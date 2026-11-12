import assert from 'tjs:assert';

function invariantSum(n, a, b) {
    var sum = 0;

    for (var i = 0; i < n; i++) {
        sum += i + (a + b);
    }

    return sum;
}

function invariantXor(n, a, b) {
    var sum = 0;

    for (var i = 0; i < n; i++) {
        sum += i + (a ^ b);
    }

    return sum;
}

function invariantFloat(n, a, b, initial) {
    var value = initial;

    for (var i = 0; i < n; i++) {
        value = a / b;
    }

    return value;
}

function invariantFloatWithConstant(n, a, initial) {
    var value = initial;

    for (var i = 0; i < n; i++) {
        value += a * 0.5;
    }

    return value;
}

function exercise() {
    for (let i = 0; i < 512; i++) {
        const count = i & 7;
        const induction = count * (count - 1) / 2;

        assert.eq(invariantSum(count, i, -13), induction + count * (i - 13));
        assert.eq(invariantXor(count, i, -13), induction + count * (i ^ -13));
        assert.eq(invariantFloat(count, i + 0.5, 2, 0.25), count ? (i + 0.5) / 2 : 0.25);
        assert.eq(invariantFloatWithConstant(count, i + 0.5, 0.25), 0.25 + count * ((i + 0.5) * 0.5));
    }

    // The hoisted guard must replay the entry when the loop would not run.
    assert.eq(invariantSum(0, 2147483647, 1), 0);

    for (let i = 0; i < 128; i++) {
        assert.eq(invariantSum(3, i, 7), 3 + 3 * (i + 7));
    }

    assert.eq(invariantSum(2, 2147483647, 1), 4294967297);
    assert.eq(invariantSum(2, 0.5, 1), 4);
    assert.eq(invariantSum(2, '2', 3), '0023123');
    assert.eq(invariantFloat(0, Infinity, 0, 0.25), 0.25);
    assert.eq(invariantFloat(3, 1, 0, 0.25), Infinity);
    assert.ok(Number.isNaN(invariantFloat(3, Infinity, Infinity, 0.25)));
    assert.ok(Object.is(invariantFloat(3, -0, 2, 0.25), -0));
    console.log('hoisted invariants match');
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
    const baseline = await run('baseline');
    const optimized = await run('1');

    assert.eq(optimized.stdout, baseline.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        const hoisted = optimized.stderr.match(/\bcfg_invariants_hoisted=(\d+)\b/);

        assert.ok(/\bcfg_invariants_hoisted=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(hoisted && Number(hoisted[1]) >= 4, optimized.stderr);
        assert.ok(/\bf64_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bguard_exits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bprecise_exits=0\b/.test(optimized.stderr), optimized.stderr);
    }
}
