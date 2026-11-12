import assert from 'tjs:assert';

function lastProduct(start, limit) {
    var result = 0;

    for (var i = start; i < limit; i++) {
        result = i * 4095;
    }

    return result;
}

function checksum(start, limit, left, right) {
    var result = 0;

    for (var i = start; i < limit; i++) {
        result += (i * 7) ^ (left + right);
    }

    return result;
}

function expectedChecksum(start, limit, offset) {
    let result = 0;

    for (let i = start; i < limit; i++) {
        result += (i * 7) ^ offset;
    }

    return result;
}

function exercise() {
    for (let i = 0; i < 512; i++) {
        const start = (i & 31) - 16;
        const limit = start + (i & 7);

        assert.eq(lastProduct(start, limit), start < limit ? (limit - 1) * 4095 : 0);
        assert.eq(checksum(start, limit, i, -15), expectedChecksum(start, limit, i - 15));
    }

    assert.eq(lastProduct(0, 32780), 32779 * 4095);

    for (let i = 0; i < 128; i++) {
        assert.eq(lastProduct(i, i + 2), (i + 1) * 4095);
    }

    // Advancing to a wide product after the last iteration must not exit.
    assert.eq(lastProduct(524416, 524417), 2147483520);

    // The same wide product is observed in the duplicated body here.
    assert.eq(lastProduct(524416, 524418), 2147487615);
    assert.eq(checksum(-5, 12345, 4, 7), expectedChecksum(-5, 12345, 11));
    console.log('induction recurrences match');
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
        const recurrences = optimized.stderr.match(/\bcfg_inductions_reduced=(\d+)\b/);

        assert.ok(/\bcfg_inductions_reduced=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(recurrences && Number(recurrences[1]) >= 2, optimized.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bprecise_exits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bprecise_fuel_exits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
    }
}
