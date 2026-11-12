import assert from 'tjs:assert';

function redundantStore(left, right, choose) {
    let value = choose ? left : right;
    const alias = value;

    value = alias;

    return value;
}

function differenceOfAliases(left, right, choose) {
    let value = choose ? left : right;
    const alias = value;

    return value - alias;
}

function differenceOfDistinctPhis(left, right, choose) {
    let first;
    let second;

    if (choose) {
        first = left;
        second = right;
    } else {
        first = right;
        second = left;
    }

    return first - second;
}

function loopCarried(initial, limit) {
    let previous;
    let current = initial;
    let total = 0;

    for (let i = 0; i < limit; i++) {
        previous = current;
        current = current + 1;
        total = total + (current - previous);
    }

    return total;
}

function exercise() {
    assert.eq(loopCarried(-5, 512), 512);

    let checksum = 0;

    for (let i = 0; i < 480; i++) {
        const choose = i & 1;
        const left = i - 20;
        const right = 20 - i;

        checksum += redundantStore(left, right, choose);
        assert.ok(Object.is(differenceOfAliases(left, right, choose), 0));
        assert.eq(differenceOfDistinctPhis(left, right, choose), choose ? left - right : right - left);
        assert.eq(loopCarried(left, 4), 4);
    }

    assert.ok(Object.is(differenceOfAliases(-2147483648, 2147483647, 1), 0));
    assert.eq(differenceOfDistinctPhis(2147483647, -1, 1), 2147483648);
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
        assert.ok(/\bssa_phis=0 ssa_stores_elided=0 ssa_zero_folded=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bssa_phis=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bssa_stores_elided=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bssa_zero_folded=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
    }
}
