import assert from 'tjs:assert';

function read(values, index) {
    return values[index];
}

function length(value) {
    return value.length;
}

function charCode(value, index) {
    return value.charCodeAt(index);
}

function viewInt(view, index) {
    return view.getInt32(index);
}

function viewIntEndian(view, index, littleEndian) {
    return view.getInt32(index, littleEndian);
}

function viewUint(view, index, littleEndian) {
    return view.getUint32(index, littleEndian);
}

function viewFloat(view, index, littleEndian) {
    return view.getFloat64(index, littleEndian);
}

function viewSetInt(view, index, value) {
    return view.setInt32(index, value);
}

function viewSetIntEndian(view, index, value, littleEndian) {
    return view.setInt32(index, value, littleEndian);
}

function viewSetUint(view, littleEndian, value, index) {
    return view.setUint32(index, value, littleEndian);
}

function viewSetFloat(view, index, value, littleEndian) {
    view.setFloat64(index, value, littleEndian);
}

function write(values, index, value) {
    values[index] = value;
}

function writeResult(values, index, value) {
    return values[index] = value;
}

function sumCount(values, count, seed) {
    let sum = seed;

    for (let i = 0; i < count; i++) {
        sum += values[i];
    }

    return sum;
}

function sumLength(values) {
    var sum = 0;

    for (var i = 0; i < values.length; i++) {
        sum += values[i];
    }

    return sum;
}

function sumWrap(unused, seed, values) {
    let sum = seed;

    for (let i = 0; i < values.length; i++) {
        sum = (sum + values[i]) | 0;
    }

    return sum;
}

function exercise() {
    const backing = Int32Array.of(100, 0x7fffffff, 0x7fffffff, -0x80000000, 6, -7, 5, -9, 8, 200);
    const ints = backing.subarray(1, 9);
    const floats = Float64Array.of(1e16, 1, -1e16, 0.125, -0, Infinity, -Infinity);
    const dense = [ { token: 'held' }, undefined ];
    const intTarget = new Int32Array(2);
    const floatTarget = new Float64Array(2);

    for (let i = 0; i < 96; i++) {
        assert.eq(read(ints, 1), 0x7fffffff);
        assert.ok(Object.is(read(floats, 4), -0));
        assert.ok(read(dense, 0) === dense[0]);
        assert.eq(read(dense, 1), undefined);
        assert.eq(length('ASCII'), 5);
        assert.eq(length('🐕a'), 3);
        assert.eq(charCode('ASCII', 0), 65);
        assert.eq(charCode('🐕a', 1), 0xdc15);
        assert.eq(write(intTarget, 0, -i), undefined);
        assert.eq(intTarget[0], -i || 0);
        assert.ok(Object.is(writeResult(floatTarget, 1, -0), -0));
        assert.ok(Object.is(floatTarget[1], -0));
        assert.eq(sumCount(ints, 6, 0x7fffffff), 4294967297);
        assert.eq(sumCount(floats, 5, 0), 0.125);
        assert.eq(sumLength(ints), 2147483649);
        assert.eq(sumWrap(0, 0x7fffffff, ints), 0);
    }

    // Cover scalar tails and a full vector with a nonzero view offset.
    assert.eq(Array.from({ length: 9 }, (_, count) => sumCount(ints, count, 1)), [
        1, 2147483648, 4294967295, 2147483647, 2147483653,
        2147483646, 2147483651, 2147483642, 2147483650,
    ]);
    assert.ok(Object.is(sumCount(floats, 0, -0), -0));
    assert.ok(Number.isNaN(sumCount(floats, 7, 0)));
    assert.eq(read(ints, -1), undefined);
    assert.eq(read(ints, ints.length), undefined);
    assert.ok(Number.isNaN(sumCount(ints, ints.length + 1, 0)));
    assert.eq(sumCount(ints, -1, 3), 3);

    let countReads = 0;
    const changingCount = { valueOf() {
        countReads++;

        return 2;
    } };

    assert.eq(sumCount(ints, changingCount, 0), 4294967294);
    assert.eq(countReads, 3);

    let coercions = 0;
    const toStore = { valueOf() {
        coercions++;

        return 0x100000003;
    } };

    assert.ok(writeResult(intTarget, 1, toStore) === toStore);
    assert.eq(intTarget[1], 3);
    assert.eq(coercions, 1);
    assert.eq(write(intTarget, 99, toStore), undefined);
    assert.eq(coercions, 2);
    assert.throws(() => write(intTarget, 0, 1n), TypeError);
    assert.eq(intTarget[0], -95);

    const immutable = new Int32Array(Int32Array.of(4).buffer.transferToImmutable());

    assert.eq(write(immutable, 0, 99), undefined);
    assert.eq(immutable[0], 4);

    let lengthReads = 0;

    Object.defineProperty(ints, 'length', { get() {
        lengthReads++;

        return 2;
    } });
    assert.eq(sumLength(ints), 4294967294);
    assert.eq(lengthReads, 3);
    assert.eq(sumWrap(0, 2, ints), 0);
    assert.eq(lengthReads, 6);

    const inherited = Int32Array.of(4, 5, 6);
    let proxyReads = 0;
    const proxy = new Proxy({}, {
        get(target, key, receiver) {
            if (key === 'length') {
                proxyReads++;

                return 1;
            }

            return Reflect.get(target, key, receiver);
        },
    });

    Object.setPrototypeOf(inherited, proxy);
    assert.eq(sumLength(inherited), 4);
    assert.eq(proxyReads, 2);

    const detached = Int32Array.of(9, 8);

    detached.buffer.transfer();
    assert.eq(read(detached, 0), undefined);
    assert.eq(sumLength(detached), 0);
    assert.ok(Number.isNaN(sumCount(detached, 1, 0)));

    const resizable = new ArrayBuffer(16, { maxByteLength: 32 });
    const fixed = new Int32Array(resizable, 4, 2);

    fixed[0] = 3;
    fixed[1] = 7;
    assert.eq(sumLength(fixed), 10);
    resizable.resize(4);
    assert.eq(read(fixed, 0), undefined);
    assert.eq(sumLength(fixed), 0);
    assert.ok(Number.isNaN(sumCount(fixed, 1, 0)));

    const shared = new Int32Array(new SharedArrayBuffer(8));

    shared[1] = 11;
    assert.eq(read(shared, 1), 11);
    assert.eq(sumLength(shared), 11);
    assert.eq(read(BigInt64Array.of(-7n), 0), -7n);
    assert.throws(() => sumCount(BigInt64Array.of(5n), 1, 0), TypeError);
    assert.eq(read('🐕', 0), '\ud83d');

    const owned = read(dense, 0);

    dense[0] = null;
    assert.eq(owned.token, 'held');

    const sparse = new Array(2);
    let elementReads = 0;

    sparse[1] = 3;
    assert.eq(read(sparse, 0), undefined);
    Object.setPrototypeOf(sparse, { 0: 'inherited' });
    assert.eq(read(sparse, 0), 'inherited');
    Object.defineProperty(dense, 0, { get() {
        elementReads++;

        return 5;
    } });
    assert.eq(read(dense, 0), 5);
    assert.eq(elementReads, 1);

    const concatenated = '🐕a'.repeat(2048) + 'x'.repeat(2048);

    assert.eq(length(concatenated), 8192);
    assert.eq(charCode(concatenated, 8191), 120);
    assert.ok(Number.isNaN(charCode('a', -1)));
    assert.ok(Number.isNaN(charCode('a', 1)));
    assert.eq(charCode('ab', 1.25), 98);
    assert.eq(charCode(new String('b'), 0), 98);
    assert.eq(length(''), 0);
    assert.eq(length(new String('boxed')), 5);
    assert.eq(length({ get length() {
        return 17;
    } }), 17);

    const descriptor = Object.getOwnPropertyDescriptor(String.prototype, 'charCodeAt');
    let methodReads = 0;
    let indexReads = 0;
    const index = { valueOf() {
        indexReads++;

        return 1;
    } };

    try {
        Object.defineProperty(String.prototype, 'charCodeAt', {
            ...descriptor,
            value() {
                return 999;
            },
        });
        assert.eq(charCode('a', 0), 999);
        Object.defineProperty(String.prototype, 'charCodeAt', {
            configurable: true,
            get() {
                methodReads++;

                return descriptor.value;
            },
        });
        assert.eq(charCode('ab', 0), 97);
        assert.eq(methodReads, 1);
        assert.eq(charCode('ab', index), 98);
        assert.eq(methodReads, 2);
        assert.eq(indexReads, 1);
    } finally {
        Object.defineProperty(String.prototype, 'charCodeAt', descriptor);
    }

    assert.eq(charCode('ab', 1), 98);

    const buffer = new ArrayBuffer(36);
    const view = new DataView(buffer, 1, 33);

    view.setUint32(1, 0x800100fe);
    view.setFloat64(9, -0, true);
    view.setFloat64(17, NaN);
    view.setFloat64(25, 12.25);

    for (let i = 0; i < 96; i++) {
        assert.eq(viewInt(view, 1), 0x800100fe | 0);
        assert.eq(viewIntEndian(view, 1, false), 0x800100fe | 0);
        assert.eq(viewIntEndian(view, 1, true), 0xfe000180 | 0);
        assert.eq(viewUint(view, 1, true), 0xfe000180);
        assert.ok(Object.is(viewFloat(view, 9, true), -0));
        assert.ok(Number.isNaN(viewFloat(view, 17, false)));
        assert.eq(viewFloat(view, 25, false), 12.25);
    }

    assert.throws(() => viewInt(view, -1), RangeError);
    assert.throws(() => viewFloat(view, 26, false), RangeError);
    assert.throws(() => viewInt(view, 1n), TypeError);
    assert.throws(() => viewInt({ getInt32: DataView.prototype.getInt32 }, 1), TypeError);
    assert.eq(viewInt(view, 1.75), 0x800100fe | 0);
    assert.eq(viewUint(view, 1, {}), 0xfe000180);

    view.getInt32 = DataView.prototype.getUint32;
    assert.eq(viewInt(view, 1), 0x800100fe);
    let viewMethodReads = 0;
    let viewIndexReads = 0;

    Object.defineProperty(view, 'getInt32', {
        configurable: true,
        get() {
            viewMethodReads++;

            return DataView.prototype.getInt32;
        },
    });
    assert.eq(viewInt(view, 1), 0x800100fe | 0);
    assert.eq(viewInt(view, { valueOf() {
        viewIndexReads++;

        return 1;
    } }), 0x800100fe | 0);
    assert.eq(viewMethodReads, 2);
    assert.eq(viewIndexReads, 1);
    delete view.getInt32;

    const proxyView = new DataView(buffer, 1, 33);
    let viewProxyReads = 0;

    Object.setPrototypeOf(proxyView, new Proxy(DataView.prototype, {
        get(target, key, receiver) {
            viewProxyReads++;

            return Reflect.get(target, key, receiver);
        },
    }));
    assert.eq(viewInt(proxyView, 1), 0x800100fe | 0);
    assert.eq(viewProxyReads, 1);
    assert.eq(viewInt(view, 1), 0x800100fe | 0);

    buffer.transfer();
    assert.throws(() => viewInt(view, 1), TypeError);

    const movingBuffer = new ArrayBuffer(16, { maxByteLength: 32 });
    const movingView = new DataView(movingBuffer);

    movingView.setInt32(4, 123);
    assert.eq(viewInt(movingView, 4), 123);
    movingBuffer.resize(4);
    assert.throws(() => viewInt(movingView, 4), RangeError);

    const sharedView = new DataView(new SharedArrayBuffer(8));

    sharedView.setInt32(2, -123);
    assert.eq(viewInt(sharedView, 2), -123);
}

function exerciseDataViewStores() {
    const buffer = new ArrayBuffer(36);
    const view = new DataView(buffer, 1, 33);

    for (let i = 0; i < 96; i++) {
        assert.eq(viewSetInt(view, 1, 0x100000000 + i), undefined);
        assert.eq(view.getInt32(1), i);
        assert.eq(viewSetIntEndian(view, 5, -i || 0, true), undefined);
        assert.eq(view.getInt32(5, true), -i || 0);
        assert.eq(viewSetUint(view, true, -i - 0.75, 9), undefined);
        assert.eq(view.getUint32(9, true), -i >>> 0);
        assert.eq(viewSetFloat(view, 25, i % 2 ? -0 : i, false), undefined);
        assert.ok(Object.is(view.getFloat64(25), i % 2 ? -0 : i));
    }

    viewSetUint(view, false, Infinity, 9);
    assert.eq(view.getUint32(9), 0);
    viewSetInt(view, 1, NaN);
    assert.eq(view.getInt32(1), 0);
    viewSetFloat(view, 13, NaN, true);
    assert.ok(Number.isNaN(view.getFloat64(13, true)));
    viewSetFloat(view, 13, -Infinity, true);
    assert.eq(view.getFloat64(13, true), -Infinity);
    viewSetIntEndian(view, 1, 0x10203040, {});
    assert.eq(view.getInt32(1, true), 0x10203040);
    viewSetInt(view, 1.75, -1);
    assert.eq(view.getInt32(1), -1);

    assert.throws(() => viewSetInt(view, 1, 1n), TypeError);
    assert.throws(() => viewSetFloat(view, 25, Symbol(), false), TypeError);
    assert.throws(() => viewSetInt(view, -1, 7), RangeError);
    assert.throws(() => viewSetFloat(view, 26, 7, false), RangeError);
    assert.ok(Object.is(view.getFloat64(25), -0));

    let floatCalls = 0;

    view.setFloat64 = () => {
        floatCalls++;

        return 3;
    };

    assert.eq(viewSetFloat(view, 25, 99, false), undefined);
    assert.eq(floatCalls, 1);
    assert.ok(Object.is(view.getFloat64(25), -0));
    delete view.setFloat64;

    let propertyReads = 0;
    let indexReads = 0;
    let valueReads = 0;
    const index = { valueOf() {
        indexReads++;

        return 1;
    } };
    const value = { valueOf() {
        valueReads++;

        return 42;
    } };

    view.setInt32 = () => 17;
    assert.eq(viewSetInt(view, index, value), 17);
    assert.eq(indexReads, 0);
    assert.eq(valueReads, 0);
    Object.defineProperty(view, 'setInt32', {
        configurable: true,
        get() {
            propertyReads++;

            return DataView.prototype.setInt32;
        },
    });
    assert.eq(viewSetInt(view, index, value), undefined);
    assert.eq(propertyReads, 1);
    assert.eq(indexReads, 1);
    assert.eq(valueReads, 1);
    assert.eq(view.getInt32(1), 42);
    delete view.setInt32;

    // Failed bounds and detached checks must happen after value conversion.
    assert.throws(() => viewSetInt(view, 30, value), RangeError);
    assert.eq(valueReads, 2);
    let transferred;

    assert.throws(() => viewSetInt(view, 1, { valueOf() {
        valueReads++;
        transferred = buffer.transfer();

        return 7;
    } }), TypeError);
    assert.eq(valueReads, 3);
    assert.eq(new DataView(transferred, 1).getInt32(1), 42);
    assert.throws(() => viewSetInt(view, 1, 7), TypeError);

    const immutable = new DataView(transferred.transferToImmutable(), 1);

    // Immutability is checked before any value or index conversion.
    assert.throws(() => viewSetInt(immutable, index, value), TypeError);
    assert.eq(indexReads, 1);
    assert.eq(valueReads, 3);
    assert.eq(immutable.getInt32(1), 42);

    const movingBuffer = new ArrayBuffer(16, { maxByteLength: 32 });
    const movingView = new DataView(movingBuffer);
    const sharedView = new DataView(new SharedArrayBuffer(16));

    assert.eq(viewSetInt(movingView, 1, 37), undefined);
    assert.eq(movingView.getInt32(1), 37);
    movingBuffer.resize(4);
    assert.throws(() => viewSetInt(movingView, 1, 7), RangeError);
    assert.eq(viewSetInt(sharedView, 1, 19), undefined);
    assert.eq(sharedView.getInt32(1), 19);
}

async function run(mode, data) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: { ...tjs.env, TJS_QUICKJS_JIT: mode, TJS_QUICKJS_JIT_DATA: data, TJS_QUICKJS_JIT_STATS: '1' },
    });
    const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

    try {
        const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text(), proc.stdout.text() ]);

        assert.eq(status.term_signal, null, stderr);
        assert.eq(status.exit_status, 0, stderr);

        return stderr;
    } finally {
        clearTimeout(watchdog);
    }
}

if (tjs.args[3] === 'child') {
    exercise();
    exerciseDataViewStores();
} else {
    const interpreter = await run('0', '1');
    const baseline = await run('baseline', '1');
    const optimized = await run('1', '1');
    const disabled = await run('1', '0');

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        for (const output of [ baseline, optimized ]) {
            for (const counter of [ 'typed_load_hits', 'typed_store_hits', 'typed_sum_hits', 'typed_f64_hits',
                'array_load_hits', 'string_length_hits', 'string_char_code_hits', 'dataview_load_hits' ]) {
                assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(output), output);
            }

            const [ , loads ] = output.match(/\bdataview_load_hits=(\d+)\b/);

            // Every tested DataView method and the default-endian form must contribute native reads.
            assert.ok(Number(loads) >= 7 * 96 - 4 * 31, output);

            const stores = output.match(/\bdataview_store_hits=(\d+)\b/);

            // All four write forms must contribute native stores.
            assert.ok(stores && Number(stores[1]) >= 4 * (96 - 31), output);
        }

        assert.ok(/\btyped_simd_hits=0\b/.test(baseline), baseline);

        if (optimized.includes('typed_simd_enabled=1')) {
            assert.ok(/\btyped_simd_hits=[1-9]\d*\b/.test(optimized), optimized);
        }

        assert.ok(!interpreter.includes('typed_load_hits='), interpreter);
        assert.ok(!disabled.includes('typed_load_hits='), disabled);
    }
}
