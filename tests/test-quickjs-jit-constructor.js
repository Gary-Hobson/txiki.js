import assert from 'tjs:assert';

function Candidate(value) {
    return value + 7;
}

function OnlyConstructed(value) {
    return value + 31;
}

function FuelConstructor(iterations, seed) {
    let state = 1.5;
    let sum = seed;

    for (let i = 0; i < iterations; i++) {
        sum += state;
        state = (state + 0.25) / 1.25;
    }

    return sum;
}

function exerciseNewOnly() {
    let last;

    for (let i = 0; i < 320; i++) {
        last = new OnlyConstructed(i);
    }

    assert.eq(Object.getPrototypeOf(last), OnlyConstructed.prototype);
    assert.eq(Object.getPrototypeOf(new OnlyConstructed(2147483647)), OnlyConstructed.prototype);
}

function exerciseFuelExit() {
    for (let i = 0; i < 64; i++) {
        new FuelConstructor(8, 0.5);
    }

    let prototypeReads = 0;
    const prototype = {};
    const newTarget = new Proxy(function () {}, {
        get() {
            prototypeReads++;

            return prototype;
        },
    });
    const instance = Reflect.construct(FuelConstructor, [ 33024, 0.5 ], newTarget);

    assert.eq(Object.getPrototypeOf(instance), prototype);
    assert.eq(prototypeReads, 1);
}

function exercise() {
    for (let i = 0; i < 192; i++) {
        assert.eq(Candidate(i), i + 7);
    }

    for (let i = 0; i < 64; i++) {
        assert.eq(Object.getPrototypeOf(new Candidate(i)), Candidate.prototype);
    }

    const inherited = { marker: 11 };
    let prototypeReads = 0;
    const newTarget = new Proxy(function () {}, {
        get(target, key, receiver) {
            if (key === 'prototype') {
                prototypeReads++;

                return inherited;
            }

            return Reflect.get(target, key, receiver);
        },
    });

    assert.eq(Object.getPrototypeOf(Reflect.construct(Candidate, [ 8 ], newTarget)), inherited);
    assert.eq(prototypeReads, 1);

    // Overflow can exit the scalar body, but the existing receiver is reused.
    assert.eq(Object.getPrototypeOf(Reflect.construct(Candidate, [ 2147483647 ], newTarget)), inherited);
    assert.eq(prototypeReads, 2);

    let conversions = 0;
    const value = {
        valueOf() {
            conversions++;

            return 5;
        },
    };

    assert.eq(Object.getPrototypeOf(Reflect.construct(Candidate, [ value ], newTarget)), inherited);
    assert.eq(conversions, 1);
    assert.eq(prototypeReads, 3);

    const marker = new Error('prototype lookup failed');
    const throwingNewTarget = new Proxy(function () {}, {
        get() {
            throw marker;
        },
    });

    try {
        Reflect.construct(Candidate, [ value ], throwingNewTarget);
        assert.fail('Expected prototype lookup to throw');
    } catch (error) {
        assert.eq(error, marker);
    }

    assert.eq(conversions, 1);

    Candidate.prototype = 123;
    assert.eq(Object.getPrototypeOf(new Candidate(4)), Object.prototype);
    assert.eq(Object.getPrototypeOf(new Candidate()), Object.prototype);
    Candidate.prototype = {};

    const explicit = { explicit: true };

    function ReturnsObject() {
        return explicit;
    }

    class Derived extends Candidate {
        constructor(value) {
            super(value);
            this.value = value;
        }
    }

    assert.eq(new ReturnsObject(), explicit);
    const instance = new Derived(42);

    assert.eq(instance.value, 42);
    assert.eq(Object.getPrototypeOf(instance), Derived.prototype);
    assert.throws(() => Reflect.construct(() => 1, []), TypeError);
}

if (tjs.args[3] === 'child') {
    exercise();
} else if (tjs.args[3] === 'new-only') {
    exerciseNewOnly();
    exerciseFuelExit();
} else {
    const configurations = [
        [ 'child', '0', '0' ],
        [ 'child', 'baseline', '0' ],
        [ 'child', '1', '0' ],
        [ 'child', '1', '1' ],
        [ 'new-only', '0', '0' ],
        [ 'new-only', 'baseline', '0' ],
        [ 'new-only', '1', '0' ],
        [ 'new-only', '1', '1' ],
    ];

    for (const [ scenario, mode, shadow ] of configurations) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, scenario ], {
            stderr: 'pipe',
            env: {
                ...tjs.env,
                TJS_QUICKJS_JIT: mode,
                TJS_QUICKJS_JIT_SHADOW: shadow,
                TJS_QUICKJS_JIT_STATS: '1',
            },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            if (mode !== '0' && tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
                const match = /\bconstructor_hits=(\d+) constructor_optimized_hits=(\d+)\b/.exec(stderr);

                assert.ok(match, 'Missing native constructor statistics');
                const hits = Number(match[1]);
                const optimized = Number(match[2]);

                if (shadow === '1') {
                    assert.eq(hits, 0);
                    assert.eq(optimized, 0);
                } else if (scenario === 'new-only') {
                    assert.ok(hits >= 250, match[0]);
                    assert.ok(mode === '1' ? optimized >= 128 : optimized === 0, match[0]);
                    assert.ok(/\bprecise_fuel_exits=[1-9]\d*\b/.test(stderr), stderr);
                } else {
                    assert.ok(hits >= 64, match[0]);
                }
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
