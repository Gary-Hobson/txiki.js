import assert from 'tjs:assert';

function arithmetic(left, right) {
    return left * 2 + right;
}

function threeArguments(first, second, third) {
    return first + second + third;
}

function directCallSite(target, left, right) {
    return target(left, right);
}

function directMethodSite(receiver, left, right) {
    return receiver.method(left, right);
}

function missingArgumentSite(target) {
    return target(1, 2);
}

function floatLoop(limit, seed) {
    let sum = seed;

    for (let i = 0; i < limit; i++) {
        sum += 0.125;
    }

    return sum;
}

function floatCallSite(target, limit, sum) {
    return target(limit, sum);
}

function constructorSite(target) {
    return new target(1, 2);
}

function evalSite(source) {
    const _captured = 13;

    return eval(source);
}

function exercise() {
    let sum = 0;
    const receiver = { method: arithmetic };

    for (let i = 0; i < 1024; i++) {
        sum += directCallSite(arithmetic, i, 3);
        sum += directMethodSite(receiver, i, 4);
        assert.eq(threeArguments(i, 2, 3), i + 5);
    }

    assert.eq(sum, 2102272);

    let conversions = 0;
    const value = {
        valueOf() {
            conversions++;

            return 5;
        },
    };

    assert.eq(directCallSite(arithmetic, value, 2), 12);
    assert.eq(conversions, 1);
    assert.eq(directCallSite(arithmetic, 2147483647, 2), 4294967296);

    for (let i = 0; i < 256; i++) {
        assert.ok(Number.isNaN(missingArgumentSite(threeArguments)));
        assert.eq(floatCallSite(floatLoop, 4, 0.5), 1);
        assert.ok(constructorSite(arithmetic) instanceof arithmetic);
        assert.eq(evalSite('_captured + 1'), 14);
    }

    // A direct Float64 call can exhaust native fuel after recording a snapshot.
    assert.eq(floatCallSite(floatLoop, 40000, 0.5), 5000.5);

    let getterCalls = 0;

    Object.defineProperty(receiver, 'method', {
        configurable: true,
        get() {
            getterCalls++;

            return arithmetic;
        },
    });
    assert.eq(directMethodSite(receiver, 6, 4), 16);
    assert.eq(getterCalls, 1);

    let sideEffects = 0;

    const replacement = function(left, right) {
        sideEffects++;

        return this === receiver ? left - right : left + right;
    };

    assert.eq(directCallSite(replacement, 11, 2), 13);
    Object.defineProperty(receiver, 'method', { value: replacement });
    assert.eq(directMethodSite(receiver, 11, 2), 9);
    assert.eq(sideEffects, 2);

    const marker = new Error('callee throws');

    try {
        directCallSite(() => {
            throw marker;
        }, 1, 2);
        assert.fail('Expected the replacement callee to throw');
    } catch (error) {
        assert.eq(error, marker);
    }
}

function callSite(stderr, name) {
    const match = stderr.match(new RegExp(`^QuickJS feedback name=${name} pc=\\d+ call (.+)$`, 'm'));

    assert.ok(match, `Missing ${name} feedback`);

    return Object.fromEntries(match[1].split(' ').map(field => field.split('=')));
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    const configurations = [ [ '0', '0' ], [ 'baseline', '0' ], [ '1', '0' ] ];

    if (tjs.env.TJS_QUICKJS_JIT_BOXED === '1' || tjs.env.TJS_QUICKJS_JIT_BOXED_REQUIRED === '1') {
        configurations.push([ '1', '1' ]);
    }

    for (const [ mode, boxed ] of configurations) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: { ...tjs.env, TJS_QUICKJS_JIT: mode, TJS_QUICKJS_JIT_BOXED: boxed,
                TJS_QUICKJS_JIT_SHADOW: '0', TJS_QUICKJS_JIT_FEEDBACK: 'dump' },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1' || stderr.includes('QuickJS feedback ')) {
                const call = callSite(stderr, 'directCallSite');
                const method = callSite(stderr, 'directMethodSite');
                const missing = callSite(stderr, 'missingArgumentSite');
                const float = callSite(stderr, 'floatCallSite');
                const constructor = callSite(stderr, 'constructorSite');

                assert.eq(missing.native_hits, '0');
                assert.eq(constructor.native_hits, '0');

                if (mode === '0') {
                    assert.eq(call.native_hits, '0');
                    assert.eq(method.native_hits, '0');
                    assert.eq(float.native_hits, '0');
                } else {
                    assert.eq(call.targets, '2');
                    assert.eq(method.targets, '2');
                    assert.ok(Number(call.native_hits) > 0, `Direct call hits: ${call.native_hits}`);
                    assert.ok(Number(method.native_hits) > 0, `Direct method hits: ${method.native_hits}`);
                    assert.ok(Number(float.native_hits) > 0, `Direct Float64 hits: ${float.native_hits}`);

                    if (boxed === '1') {
                        assert.ok(Number(call.boxed_hits) > 0, `Boxed call hits: ${call.boxed_hits}`);
                        assert.ok(Number(method.boxed_hits) > 0, `Boxed method hits: ${method.boxed_hits}`);
                        assert.ok(Number(float.boxed_hits) > 0, `Boxed Float64 hits: ${float.boxed_hits}`);
                    }
                }
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
