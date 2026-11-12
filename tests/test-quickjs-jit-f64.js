import assert from 'tjs:assert';

function dense(iterations, seed) {
    let x = seed;
    let sum = 0.125;

    for (let i = 0; i < iterations; i++) {
        x = x * 1.125 + 0.05;
        sum = (sum + x) / 1.25;
    }

    return sum;
}

function fuelLoop(iterations, seed) {
    let state = 1.5;
    let sum = seed;

    for (let i = 0; i < iterations; i++) {
        sum += state;
        state = (state + 0.25) / 1.25;
    }

    return sum;
}

function quotient(left, right) {
    return left / right;
}

function divideByNegativePower(value) {
    return value / -8;
}

function divideBySmallPower(value) {
    return value / 0.125;
}

function remainder(left, right) {
    return left % right;
}

function exponentiate(left, right) {
    return left ** right;
}

function mathLoop(iterations, seed) {
    let result = seed;

    for (let i = 0; i < iterations; i++) {
        result = result + (0.75 + (result % 2.75) ** 2);
    }

    return result;
}

function mixedBits(value, shift) {
    return 0.5 + (~value + (value & shift) + (value ^ shift) + (value | shift) +
                  (value << shift) + (value >> shift) + (value >>> shift));
}

function overflow(left, right) {
    return left * right + left;
}

function negate(value) {
    return -value;
}

function truthiness(value) {
    if (value) {
        return 1.25;
    }

    return -0;
}

function comparisons(left, right) {
    let result = 0.5;

    if (left < right) {
        result += 1;
    }

    if (left <= right) {
        result += 2;
    }

    if (left > right) {
        result += 4;
    }

    if (left >= right) {
        result += 8;
    }

    if (left === right) {
        result += 16;
    }

    if (left !== right) {
        result += 32;
    }

    return result;
}

function exercise() {
    // The first call crosses the interpreter's backedge threshold.
    const osr = dense(200, 0.625);
    const osrFuelExit = fuelLoop(33024, 0.5);

    for (let i = 0; i < 384; i++) {
        dense(12, 0.625);
        fuelLoop(8, 0.5);
        quotient(7, 2);
        divideByNegativePower(7.5);
        divideBySmallPower(7.5);
        remainder(-7.5, 2);
        exponentiate(2.25, 0.5);
        mathLoop(8, 1.5);
        mixedBits(-0.25, 1.5);
        overflow(2097152, 2097152);
        negate(0.5);
        truthiness(0.25);
        comparisons(0.25, 1);
    }

    for (const input of [ 0, -0, 7.5, -7.5, Number.MIN_VALUE, -Number.MIN_VALUE, Number.MAX_VALUE,
        Infinity, -Infinity, NaN ]) {
        assert.ok(Object.is(divideByNegativePower(input), input / -8));
        assert.ok(Object.is(divideBySmallPower(input), input / 0.125));
    }

    const value = {
        osr,
        osrFuelExit,
        entryFuelExit: fuelLoop(33024, 0.5),
        dense: dense(16, 1.625),
        fractional: quotient(7, 2),
        mixed: quotient(7.5, 2),
        remainder: remainder(-7.5, 2),
        infiniteRemainder: remainder(-5.5, Infinity),
        invalidRemainder: Number.isNaN(remainder(Infinity, 2)),
        remainderNegativeZero: Object.is(remainder(-4, 2), -0),
        sqrt: exponentiate(2.25, 0.5),
        exponentZero: exponentiate(NaN, 0),
        infiniteExponent: Number.isNaN(exponentiate(-1, Infinity)),
        powerNegativeZero: exponentiate(-0, -3) === -Infinity,
        mathLoop: mathLoop(24, 1.5),
        mixedBits: [
            mixedBits(-1.5, 0),
            mixedBits(4294967297.75, 31.5),
            mixedBits(-4294967297.75, -31.5),
            mixedBits(NaN, Infinity),
            mixedBits(-0, -0),
            mixedBits(Number.MIN_VALUE, 0),
        ],
        overflow: overflow(2097152, 2097152),
        infinity: quotient(1, 0) === Infinity,
        invalid: Number.isNaN(quotient(0, 0)),
        negatedZero: Object.is(negate(0), -0),
        underflow: Object.is(quotient(-Number.MIN_VALUE, 2), -0),
        negativeZero: Object.is(quotient(0, -0.5), -0),
        truthyInfinity: truthiness(Infinity),
        falseyNaN: Object.is(truthiness(NaN), -0),
        falseyZero: Object.is(truthiness(-0), -0),
        unordered: comparisons(NaN, 1),
        unorderedRhs: comparisons(1, NaN),
        less: comparisons(0.5, 1),
        greater: comparisons(1, 0.5),
        equal: comparisons(-0, 0),
        // Execute the fallback after an admitted function is hot.
        coerced: quotient({ valueOf: () => 9 }, 2),
    };

    assert.eq(value.fractional, 3.5);
    assert.eq(value.mixed, 3.75);
    assert.eq(value.osrFuelExit, value.entryFuelExit);
    assert.ok(Number.isFinite(value.osrFuelExit));
    assert.eq(value.remainder, -1.5);
    assert.eq(value.infiniteRemainder, -5.5);
    assert.ok(value.invalidRemainder && value.remainderNegativeZero);
    assert.eq(value.sqrt, 1.5);
    assert.eq(value.exponentZero, 1);
    assert.ok(value.infiniteExponent && value.powerNegativeZero);
    assert.eq(value.overflow, 4398048608256);
    assert.ok(value.infinity && value.invalid && value.negatedZero &&
              value.underflow && value.negativeZero);
    assert.eq(value.truthyInfinity, 1.25);
    assert.ok(value.falseyNaN && value.falseyZero);
    assert.eq(value.unordered, 32.5);
    assert.eq(value.unorderedRhs, 32.5);
    assert.eq(value.less, 35.5);
    assert.eq(value.greater, 44.5);
    assert.eq(value.equal, 26.5);
    assert.eq(value.coerced, 4.5);
    console.log(JSON.stringify(value));
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

        assert.eq(status.term_signal, null, `Float64 mode ${mode} terminated: ${stderr}`);
        assert.eq(status.exit_status, 0, `Float64 mode ${mode} failed: ${stderr}`);

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
        assert.ok(/\bf64_hits=[1-9]\d*\b/.test(baseline.stderr), baseline.stderr);
        assert.ok(/\bf64_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bf64_promotions=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\boptimized_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bosr_hits=[1-9]\d*\b/.test(optimized.stderr), optimized.stderr);
        assert.ok(/\bprecise_fuel_exits=([2-9]|[1-9]\d+)\b/.test(optimized.stderr), optimized.stderr);
    }
}
