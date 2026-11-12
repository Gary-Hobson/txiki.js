import assert from 'tjs:assert';

function reduction(n, seed) {
    var result = seed;

    for (var i = 0; i < n; i++) {
        if ((i & 1) === 0) {
            result += i * 3;
        } else {
            result -= i;
        }
    }

    return result;
}

function product(left, right) {
    return left * right;
}

function quotient(left, right) {
    return left / right;
}

function remainder(left, right) {
    return left % right;
}

function sampleAfterWarmup(operation, left, right, warmValue = 23) {
    for (let i = 0; i < 64; i++) {
        operation(warmValue, 1);
    }

    return operation(left, right);
}

function sumAfterLoop(n, seed, tail) {
    var result = seed;

    for (var i = 0; i < n; i++) {
        result += i;
    }

    return result + tail;
}

function unsigned(value, shift) {
    return value >>> shift;
}

function lexicalLoop(n, seed) {
    let result = seed;

    for (let i = 0; i < n; i++) {
        const step = i * 3;

        result += step;
    }

    return result;
}

function coldLoop(n, seed) {
    var result = seed;

    for (var i = 0; i < n; i++) {
        result += i;
    }

    return result;
}

function coldLoopOverflow(n, seed) {
    var result = seed;

    for (var i = 0; i < n; i++) {
        result += i;
    }

    return result;
}

function exerciseOSR() {
    const result = coldLoop(512, 17);
    const overflow = coldLoopOverflow(512, 2147400000);

    assert.eq(result, 130833);
    assert.eq(overflow, 2147530816);
    console.log(result, overflow);
}

function exerciseConstants() {
    const values = [ -2147483648, -2147483647, -128, -1, 0, 1, 127, 2147483647 ];

    for (const divisor of [ 1, -1, 2, -8, 128, -65536, 1073741824, -2147483648 ]) {
        const divide = new Function('value', `return value / (${divisor});`);
        const modulo = new Function('value', `return value % (${divisor});`);

        for (let i = 0; i < 256; i++) {
            divide(divisor);
            modulo(1);
        }

        for (const value of values) {
            const actualQuotient = sampleAfterWarmup(divide, value, divisor, divisor);
            const actualRemainder = sampleAfterWarmup(modulo, value, divisor, 1);

            assert.ok(Object.is(actualQuotient, value / divisor), `${value} / ${divisor}`);
            assert.ok(Object.is(actualRemainder, value % divisor), `${value} % ${divisor}`);
        }
    }

    for (const multiplier of [ 0, 1, -1, 2, -8, 3, -5, 1073741824 ]) {
        const multiply = new Function('value', `return value * (${multiplier});`);

        for (let i = 0; i < 256; i++) {
            multiply(1);
        }

        for (const value of values) {
            const actual = sampleAfterWarmup(multiply, value, multiplier, 1);

            assert.ok(Object.is(actual, value * multiplier), `${value} * ${multiplier}`);
        }
    }

    const offset = new Function('value', 'return value - -4095;');
    const compare = new Function('value', 'return value < -4095 ? 7 : 11;');

    for (let i = 0; i < 256; i++) {
        assert.eq(offset(i), i + 4095);
        assert.eq(compare(i), 11);
    }

    assert.eq(offset(2147483647), 2147487742);
    assert.eq(compare(-4095), 11);
    assert.eq(compare(-2147483648), 7);
}

function exercise() {
    for (let i = 0; i < 2048; i++) {
        reduction(32, -8);
        product(11, -3);
        quotient(-24, 3);
        remainder(-23, 5);
        sumAfterLoop(12, 0, 1);
        unsigned(42, 1);
        lexicalLoop(32, -5);
    }

    const marker = new Error('coercion failed');
    let conversions = 0;
    const throwing = {
        valueOf() {
            conversions++;
            throw marker;
        },
    };
    let exception;

    try {
        sumAfterLoop(12, 0, throwing);
    } catch (error) {
        exception = error;
    }

    const addOverflow = sumAfterLoop(4, 2147483646, 0);
    const multiplyOverflow = product(-2147483648, -1);

    for (let i = 0; i < 64; i++) {
        sumAfterLoop(12, 0, 1);
        product(11, -3);
    }

    const result = {
        zeroIterations: reduction(-3, -8),
        signedLoop: reduction(32, -8),
        longLoop: reduction(20000, -8),
        fuelLimitedLoop: coldLoop(40000, 1),
        addOverflow,
        multiplyOverflow,
        multiplyNegativeZero: Object.is(product(-5, 0), -0),
        doubleProduct: product(2.5, -3),
        exactQuotient: quotient(24, -3),
        signedRemainder: remainder(-23, 5),
        fractionalQuotient: quotient(-7, 2),
        quotientNegativeZero: Object.is(sampleAfterWarmup(quotient, 0, -4), -0),
        remainderNegativeZero: Object.is(remainder(-6, 3), -0),
        quotientByZero: sampleAfterWarmup(quotient, 4, 0) === Infinity,
        remainderByZero: Number.isNaN(sampleAfterWarmup(remainder, 4, 0)),
        quotientOverflow: sampleAfterWarmup(quotient, -2147483648, -1),
        remainderOverflow: Object.is(sampleAfterWarmup(remainder, -2147483648, -1), -0),
        unsignedOverflow: unsigned(-1, 32),
        unsignedMaskedShift: unsigned(-8, 34),
        stringConcatenation: sumAfterLoop(4, 0, 'x'),
        propagatedException: exception === marker,
        conversions,
        postExitLoop: reduction(5, 0),
        lexicalLoop: lexicalLoop(33, -5),
    };

    assert.eq(result, {
        zeroIterations: -8,
        signedLoop: 456,
        longLoop: 199969992,
        fuelLimitedLoop: 799980001,
        addOverflow: 2147483652,
        multiplyOverflow: 2147483648,
        multiplyNegativeZero: true,
        doubleProduct: -7.5,
        exactQuotient: -8,
        signedRemainder: -3,
        fractionalQuotient: -3.5,
        quotientNegativeZero: true,
        remainderNegativeZero: true,
        quotientByZero: true,
        remainderByZero: true,
        quotientOverflow: 2147483648,
        remainderOverflow: true,
        unsignedOverflow: 4294967295,
        unsignedMaskedShift: 1073741822,
        stringConcatenation: '6x',
        propagatedException: true,
        conversions: 1,
        postExitLoop: 14,
        lexicalLoop: 1579,
    });
    exerciseConstants();
    console.log(JSON.stringify(result));
}

async function run(mode, task = 'child') {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, task ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: { ...tjs.env, TJS_QUICKJS_JIT: mode, TJS_QUICKJS_JIT_STATS: '1' },
    });
    const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

    try {
        const [ status, stdout, stderr ] = await Promise.all([ proc.wait(), proc.stdout.text(), proc.stderr.text() ]);

        assert.eq(status.term_signal, null, `JIT mode ${mode} terminated: ${stderr}`);
        assert.eq(status.exit_status, 0, `JIT mode ${mode} failed: ${stderr}`);

        return { stdout, stderr };
    } finally {
        clearTimeout(watchdog);
    }
}

function assertCounter(stderr, name, predicate) {
    const match = stderr.match(new RegExp(`\\b${name}=(\\d+)\\b`));

    assert.ok(match && predicate(Number(match[1])), `unexpected ${name} counter: ${stderr}`);
}

if (tjs.args[3] === 'child') {
    exercise();
} else if (tjs.args[3] === 'osr-child') {
    exerciseOSR();
} else {
    const interpreter = await run('0');
    const baseline = await run('baseline');
    const optimized = await run('1');
    const osrInterpreter = await run('0', 'osr-child');
    const osrOptimized = await run('1', 'osr-child');

    assert.eq(baseline.stdout, interpreter.stdout, 'Baseline JIT matches the interpreter');
    assert.eq(optimized.stdout, interpreter.stdout, 'Optimizing JIT matches the interpreter');
    assert.eq(osrOptimized.stdout, osrInterpreter.stdout, 'OSR matches the interpreter on the first call');

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assertCounter(baseline.stderr, 'baseline_hits', hits => hits > 0);
        assertCounter(baseline.stderr, 'optimized_hits', hits => hits === 0);
        assertCounter(optimized.stderr, 'baseline_hits', hits => hits > 0);
        assertCounter(optimized.stderr, 'optimized_hits', hits => hits > 0);
        assertCounter(optimized.stderr, 'guard_exits', hits => hits > 0);
        assertCounter(optimized.stderr, 'precise_exits', hits => hits > 0);
        assertCounter(optimized.stderr, 'fuel_exits', hits => hits > 0);
        assertCounter(optimized.stderr, 'precise_fuel_exits', hits => hits > 0);
        assertCounter(osrOptimized.stderr, 'osr_hits', hits => hits > 0);
        assertCounter(osrOptimized.stderr, 'osr_exits', exits => exits > 0);
        assertCounter(osrOptimized.stderr, 'precise_exits', exits => exits > 0);
    }
}
