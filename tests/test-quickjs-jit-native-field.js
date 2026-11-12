import assert from 'tjs:assert';

function readBorrowed(receiver) {
    return receiver.value;
}

function readRetained(receiver) {
    return receiver.method(9);
}

function exercise() {
    const payload = { id: 11 };
    const object = { first: 0, value: payload };
    const receiver = {
        first: 1,
        method(value) {
            return value === 9 ? this : undefined;
        },
    };

    let observedPayload;
    let observedMethod;

    for (let i = 0; i < 256; ++i) {
        observedPayload = readBorrowed(object);
        observedMethod = readRetained(receiver);
    }

    assert.ok(observedPayload === payload);
    assert.ok(observedMethod === receiver);

    const held = readBorrowed(object);

    object.value = 23;
    assert.eq(readBorrowed(object), 23);
    object.value = { id: 13 };
    assert.eq(readBorrowed(object).id, 13);
    assert.eq(held.id, 11);

    let accessorCalls = 0;

    Object.defineProperty(object, 'value', {
        get() {
            accessorCalls++;

            return this.first + 29;
        },
        configurable: true,
    });
    assert.eq(readBorrowed(object), 29);
    assert.eq(accessorCalls, 1);

    const inherited = { value: 31 };

    delete object.value;
    Object.setPrototypeOf(object, inherited);
    assert.eq(readBorrowed(object), 31);

    let proxyReads = 0;
    const proxy = new Proxy(object, {
        get(target, key, self) {
            proxyReads++;

            return Reflect.get(target, key, self);
        },
    });

    assert.eq(readBorrowed(proxy), 31);
    assert.eq(proxyReads, 1);
    assert.eq(readBorrowed({ value: 37, first: 2 }), 37);

    Object.defineProperty(receiver, 'method', {
        get() {
            accessorCalls++;

            return function (value) {
                return this.first + 2 * value;
            };
        },
    });
    assert.eq(readRetained(receiver), 19);
    assert.eq(accessorCalls, 2);
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    const configurations = [
        [ '0', '0', '0' ],
        [ '1', '1', '0' ],
        [ '1', '1', '1' ],
    ];

    for (const [ mode, boxed, feedback ] of configurations) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: {
                ...tjs.env,
                TJS_QUICKJS_JIT: mode,
                TJS_QUICKJS_JIT_BOXED: boxed,
                TJS_QUICKJS_JIT_FEEDBACK: feedback,
                TJS_QUICKJS_JIT_SHADOW: '0',
                TJS_QUICKJS_JIT_STATS: '1',
            },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            const hasStats = tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1' || stderr.includes('QuickJS boxed field:');

            if (boxed === '1' && hasStats) {
                const match = /boxed_native_borrowed_fields=(\d+) boxed_native_retained_fields=(\d+)/.exec(stderr);

                assert.ok(match, stderr);
                const [ , borrowed, retained ] = match;

                assert.ok(feedback === '1' ? Number(borrowed) >= 200 : borrowed === '0', match[0]);
                assert.ok(feedback === '1' ? Number(retained) >= 200 : retained === '0', match[0]);
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
