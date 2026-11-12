import assert from 'tjs:assert';

function sum(iterable) {
    let total = 0;

    for (const value of iterable) {
        total += value;
    }

    return total;
}

function first(iterable) {
    let result;

    for (const value of iterable) {
        result = value;
        break;
    }

    return result;
}

function keys(object) {
    let result = '';

    for (const key in object) {
        result += key;
    }

    return result;
}

function storeFirst(iterable, record) {
    [ record.value ] = iterable;

    return record.value;
}

function throwFromBody(iterable, error) {
    for (const _value of iterable) {
        throw error;
    }
}

function observeError(action) {
    try {
        action();
    } catch (error) {
        return error;
    }

    throw new Error('expected an exception');
}

function exercise() {
    const obj = Object.create({ c: true });

    obj.a = true;
    obj.b = true;

    for (let i = 0; i < 192; i++) {
        assert.eq(sum([ 1, 2, 3 ]), 6);
        assert.eq(first([ 4, 5 ]), 4);
        assert.eq(keys(obj), 'abc');
        assert.eq(storeFirst([ i ], {}), i);
        assert.eq(observeError(() => throwFromBody([ 1 ], i)), i);
    }

    const bodyError = new Error('loop body');
    const closeError = new Error('iterator return');
    const nextError = new Error('iterator next');
    let closes = 0;

    function iterable(close, nextError) {
        return {
            [Symbol.iterator]() {
                let pending = true;

                return {
                    next() {
                        if (nextError) {
                            throw nextError;
                        }

                        const done = !pending;

                        pending = false;

                        return { value: 41, done };
                    },
                    get return() {
                        closes++;

                        return close;
                    },
                };
            },
        };
    }

    assert.eq(sum(iterable(() => {
        throw closeError;
    })), 41);
    assert.eq(closes, 0, 'exhausted iterator does not access return');

    assert.eq(first(iterable(() => {
        tjs.engine.gc.run();

        return {};
    })), 41);
    assert.eq(closes, 1, 'break closes once');

    const closeNormally = () => {
        return {};
    };

    assert.eq(storeFirst(iterable(closeNormally), {}), 41);
    assert.eq(closes, 2, 'destructuring closes once');

    assert.ok(observeError(() => first(iterable(() => 3))) instanceof TypeError);
    assert.eq(closes, 3, 'non-object return does not close again');

    assert.eq(observeError(() => first(iterable(() => {
        throw closeError;
    }))), closeError);
    assert.eq(closes, 4, 'throwing return does not close again');

    assert.eq(observeError(() => throwFromBody(iterable(() => {
        throw closeError;
    }), bodyError)), bodyError);
    assert.eq(closes, 5, 'body exception is retained after iterator return throws');

    assert.eq(observeError(() => sum(iterable(closeNormally, nextError))), nextError);
    assert.eq(closes, 5, 'throwing next does not close');

    const proxy = new Proxy(obj, {
        ownKeys(target) {
            return Reflect.ownKeys(target).reverse();
        },
    });

    assert.eq(keys(proxy), 'bac');
    console.log('iterator semantics verified');
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_STATS: '1',
        },
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

if (tjs.args.includes('child')) {
    exercise();
} else {
    const control = await run('0');
    const boxed = await run('baseline');

    assert.eq(boxed.stdout, control.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        const counters = [ 'boxed_for_in_next', 'boxed_for_of_next', 'boxed_iterator_closes', 'boxed_exceptions' ];

        for (const counter of counters) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
        }
    }
}
