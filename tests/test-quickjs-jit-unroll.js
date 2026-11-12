import assert from 'tjs:assert';

function counted(n, k, seed) {
    var sum = seed;

    for (var i = 0; i < n; i++) {
        sum += i + k;
    }

    return sum;
}

function postTest(n, k) {
    var sum = 0;
    var i = 0;

    do {
        sum += i + k;
        i++;
    } while (i < n);

    return sum;
}

function exercise() {
    for (let i = 0; i < 512; i++) {
        const n = i & 7;

        assert.eq(counted(n, 3, 5), 5 + n * 3 + n * (n - 1) / 2);

        const count = Math.max(n, 1);

        assert.eq(postTest(n, -5), count * -5 + count * (count - 1) / 2);
    }

    // The first iteration stays in int32 range and the duplicate hits overflow.
    assert.eq(counted(2, 1, 2147483645), 2147483648);

    // Fuel expires on a latch. The interpreter must resume at the loop header.
    const n = 32780;

    assert.eq(postTest(n, 3), n * 3 + n * (n - 1) / 2);
    console.log('unrolled loops match');
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
        const loops = optimized.stderr.match(/\bcfg_loops_unrolled=(\d+)\b/);

        assert.ok(/\bcfg_loops_unrolled=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(loops && Number(loops[1]) >= 2, optimized.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bprecise_exits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bprecise_fuel_exits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
    }
}
