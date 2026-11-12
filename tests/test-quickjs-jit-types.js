import assert from 'tjs:assert';

function classify(value) {
    return [
        typeof value,
        typeof value === 'undefined',
        typeof value === 'function',
        value === null,
        value === undefined,
    ];
}

function nullable(record) {
    return record?.value ?? 5;
}

function has(key, record) {
    return key in record;
}

function instance(value, constructor) {
    return value instanceof constructor;
}

function remove(record, key) {
    return delete record[key];
}

function insert(record, value) {
    return record.value = value;
}

function postIncrement(record) {
    return record.value++;
}

function postDecrement(record) {
    return record.value--;
}

class Private {
    #value;

    has(value) {
        return #value in value;
    }
}

function exercise() {
    const branded = new Private();
    const inherited = Object.create({ inherited: true });

    for (let i = 0; i < 128; i++) {
        assert.eq(classify(null), [ 'object', false, false, true, false ]);
        assert.eq(classify(undefined), [ 'undefined', true, false, false, true ]);
        assert.eq(classify(exercise), [ 'function', false, true, false, false ]);
        assert.eq(classify(Symbol()), [ 'symbol', false, false, false, false ]);
        assert.eq(classify(2n), [ 'bigint', false, false, false, false ]);
        assert.eq(nullable(null), 5);
        assert.eq(nullable({ value: 0 }), 0);
        assert.eq(has('inherited', inherited), true);
        assert.eq(has('missing', inherited), false);
        assert.eq(instance(branded, Private), true);
        assert.eq(instance(inherited, Private), false);
        assert.eq(branded.has(branded), true);
        assert.eq(branded.has(inherited), false);

        const record = {};
        const value = { i };

        assert.is(insert(record, value), value);
        assert.is(record.value, value);
        assert.eq(remove(record, 'value'), true);
        assert.eq(has('value', record), false);

        record.value = i;
        assert.eq(postIncrement(record), i);
        assert.eq(record.value, i + 1);
        assert.eq(postDecrement(record), i + 1);
        assert.eq(record.value, i);
    }

    const marker = new Error('operator trap');
    const actions = [];
    const key = {
        [Symbol.toPrimitive]() {
            actions.push('key');
            tjs.engine.gc.run();

            return 'present';
        },
    };
    const proxy = new Proxy({ present: true }, {
        has(target, property) {
            actions.push('has');

            return Reflect.has(target, property);
        },
        deleteProperty() {
            actions.push('delete');
            throw marker;
        },
    });
    const constructor = {
        [Symbol.hasInstance](value) {
            actions.push('instance');
            tjs.engine.gc.run();

            return value === proxy;
        },
    };

    assert.eq(has(key, proxy), true);
    assert.throws(() => has(key, 0), TypeError);
    assert.eq(instance(proxy, constructor), true);
    assert.throws(() => instance(proxy, null), TypeError);
    assert.throws(() => remove(proxy, key), marker);
    assert.throws(() => branded.has(3), TypeError);
    assert.throws(() => remove(Object.freeze({ value: 1 }), 'value'), TypeError);
    assert.eq(actions.join(','), 'key,has,instance,key,delete');

    const record = { value: 2147483647 };

    assert.eq(postIncrement(record), 2147483647);
    assert.eq(record.value, 2147483648);
    record.value = -2147483648;
    assert.eq(postDecrement(record), -2147483648);
    assert.eq(record.value, -2147483649);
    record.value = 2n;
    assert.eq(postIncrement(record), 2n);
    assert.eq(record.value, 3n);
    record.value = {
        valueOf() {
            actions.push('valueOf');
            tjs.engine.gc.run();
            throw marker;
        },
    };
    assert.throws(() => postDecrement(record), marker);
    assert.eq(actions.join(','), 'key,has,instance,key,delete,valueOf');
    console.log('stack and operator semantics verified');
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_SHADOW: '0',
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
        const counters = [
            'boxed_native_shuffles',
            'boxed_stack_inserts',
            'boxed_type_checks',
            'boxed_membership',
            'boxed_deletes',
            'boxed_exceptions',
        ];

        for (const counter of counters) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
        }
    }
}
