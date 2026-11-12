import assert from 'tjs:assert';

function equalJoin(input, selector) {
    var coefficient;

    if (selector < 0) {
        coefficient = 7;
    } else {
        coefficient = 7;
    }

    var adjustment = coefficient * 3;

    if (coefficient === 7) {
        return (input + 1) * adjustment;
    }

    return input - adjustment;
}

function conflictingJoin(input, selector) {
    var coefficient;

    if (selector < 0) {
        coefficient = 7;
    } else {
        coefficient = 8;
    }

    if (coefficient === 7) {
        return input + coefficient;
    }

    return input - coefficient;
}

function loop(n) {
    var total = 0;
    var step = 5;

    for (var i = 0; i < n; i++) {
        var mode = 2;

        if (mode < 4) {
            total += i + step;
        } else {
            total -= step;
        }
    }

    return total;
}

function negativeZero() {
    var dividend = 0;
    var divisor = -2;

    return dividend / divisor;
}

function mostlyTaken(input, selector) {
    if (selector < 0) {
        return input + 3;
    }

    return input * 7;
}

function mostlyTakenFloat(input, selector) {
    if (selector < 0) {
        return input + 3.25;
    }

    return input / -8;
}

function exercise() {
    assert.eq(loop(512), 133376);

    for (let i = 0; i < 512; i++) {
        const selector = (i & 1) ? -1 : 1;

        assert.eq(equalJoin(i, selector), (i + 1) * 21);
        assert.eq(conflictingJoin(i, selector), selector < 0 ? i + 7 : i - 8);
        assert.eq(loop(7), 56);
        assert.eq(mostlyTaken(i, 1), i * 7);
        assert.eq(mostlyTakenFloat(i + 0.5, 1), (i + 0.5) / -8);
    }

    for (let i = 0; i < 180; i++) {
        assert.ok(Object.is(negativeZero(), -0));
    }

    // On an arithmetic side exit the materialized slots must still reflect
    // the bytecode at the guard, including the folded coefficient.
    assert.eq(equalJoin(2147483647, -1), 45097156608);
    assert.eq(loop(32780), 537411710);
    assert.eq(mostlyTaken(5, -1), 8);
    assert.eq(mostlyTaken(2147483647, 1), 15032385529);
    assert.eq(mostlyTakenFloat(0.5, -Infinity), 3.75);
    assert.ok(Object.is(mostlyTakenFloat(0, NaN), -0));
    console.log('CFG results match');
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: { ...tjs.env, TJS_QUICKJS_JIT: mode, TJS_QUICKJS_JIT_STATS: '1', TJS_QUICKJS_JIT_FEEDBACK: '1' },
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
        assert.ok(/\bcfg_branches_folded=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(/\bcfg_ops_folded=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(/\bcfg_hot_layouts=0\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(/\bcfg_branches_folded=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bcfg_ops_folded=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        const hotLayouts = optimized.stderr.match(/\bcfg_hot_layouts=(\d+)\b/);

        assert.ok(hotLayouts && Number(hotLayouts[1]) >= 2, optimized.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
    }
}
