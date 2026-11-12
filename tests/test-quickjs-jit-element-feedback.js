import assert from 'tjs:assert';

function readElement(receiver, index) {
    return receiver[index];
}

function writeElement(receiver, index, value) {
    receiver[index] = value;
}

function callElement(receiver, index) {
    return receiver[index]();
}

function exercise() {
    const packed = [ 7, 1.25 ];
    const sparse = [ 7, 8 ];

    Object.defineProperty(sparse, 0, { get: () => 9 });
    const int32 = Int32Array.of(8);
    const float64 = Float64Array.of(-0.75);
    const detached = Int32Array.of(4);

    detached.buffer.transfer();
    const ordinary = { 0: 'first' };
    let proxyReads = 0;
    let proxyWrites = 0;
    const proxy = new Proxy(ordinary, {
        get(target, key, receiver) {
            proxyReads++;

            return Reflect.get(target, key, receiver);
        },
        set(target, key, value, receiver) {
            proxyWrites++;

            return Reflect.set(target, key, value, receiver);
        },
    });

    assert.eq(readElement(packed, 0), 7);
    assert.eq(readElement(packed, 1), 1.25);
    assert.eq(readElement(packed, 2), undefined);
    assert.eq(readElement(packed, -1), undefined);
    assert.eq(readElement(packed, '0'), 7);
    assert.eq(readElement(sparse, 0), 9);
    assert.eq(readElement(int32, 0), 8);
    assert.eq(readElement(float64, 0), -0.75);
    assert.eq(readElement(detached, 0), undefined);
    assert.eq(readElement(proxy, 0), 'first');
    assert.eq(readElement(ordinary, 0), 'first');
    assert.eq(readElement('word', 0), 'w');
    assert.eq(readElement(true, 0), undefined);
    assert.eq(proxyReads, 1);

    writeElement(packed, 0, 5);
    writeElement(packed, 2, 2.75);
    writeElement(packed, -1, 'named');
    writeElement(packed, 'label', 12);
    writeElement(int32, 0, 13);
    writeElement(float64, 0, 2.5);
    writeElement(proxy, 0, 'second');
    writeElement(ordinary, 'other', {});
    assert.eq(packed[0], 5);
    assert.eq(packed[2], 2.75);
    assert.eq(packed[-1], 'named');
    assert.eq(packed.label, 12);
    assert.eq(int32[0], 13);
    assert.eq(float64[0], 2.5);
    assert.eq(ordinary[0], 'second');
    assert.eq(proxyWrites, 1);

    const methods = [ function () {
        return this;
    } ];

    for (let i = 0; i < 160; i++) {
        assert.eq(readElement(packed, 0), i ? i - 1 : 5);
        writeElement(packed, 0, i);
        assert.ok(callElement(methods, 0) === methods);
    }
}

function elementSite(stderr, name, kind) {
    const match = new RegExp(`^QuickJS feedback name=${name} pc=\\d+ ${kind} (.+)$`, 'm').exec(stderr);

    assert.ok(match, `Missing ${name} ${kind} feedback`);

    return Object.fromEntries(match[1].split(' ').map(field => field.split('=')));
}

function assertCounts(site, boxed) {
    const outcomes = [ 'in_bounds', 'at_end', 'out_of_bounds', 'invalid_view', 'unknown' ];

    assert.eq(outcomes.reduce((sum, key) => sum + Number(site[key]), 0), Number(site.samples));
    assert.ok(boxed ? Number(site.boxed_samples) > 0 : site.boxed_samples === '0', site);
    assert.ok(Number(site.in_bounds) > 0, site);
    assert.ok(Number.parseInt(site.value_tags, 16) > 0, site);
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    const configurations = [
        [ '0', '0', '0' ],
        [ '0', '0', 'dump' ],
        [ '1', '1', 'dump' ],
    ];

    for (const [ mode, boxed, feedback ] of configurations) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: {
                ...tjs.env,
                TJS_QUICKJS_JIT: mode,
                TJS_QUICKJS_JIT_BOXED: boxed,
                TJS_QUICKJS_JIT_DATA: '0',
                TJS_QUICKJS_JIT_FEEDBACK: feedback,
                TJS_QUICKJS_JIT_SHADOW: '0',
            },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            if (feedback === '0') {
                assert.ok(!stderr.includes(' element_read ') && !stderr.includes(' element_write '));
            } else if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1' || stderr.includes('QuickJS feedback ')) {
                const read = elementSite(stderr, 'readElement', 'element_read');
                const write = elementSite(stderr, 'writeElement', 'element_write');
                const method = elementSite(stderr, 'callElement', 'element_read');

                assertCounts(read, boxed === '1');
                assertCounts(write, boxed === '1');
                assertCounts(method, boxed === '1');
                assert.eq(read.receivers, '7f');
                assert.eq(write.receivers, '1d');
                assert.eq(read.typed_kinds, write.typed_kinds);
                assert.eq(Number.parseInt(read.typed_kinds, 16).toString(2).replaceAll('0', '').length, 2);
                assert.eq(read.index_range, '-1,2');
                assert.eq(write.index_range, '-1,2');
                assert.ok(Number(read.int_indices) < Number(read.samples));
                assert.eq(read.invalid_view, '1');
                assert.eq(read.at_end, '1');
                assert.eq(read.out_of_bounds, '1');
                assert.eq(write.at_end, '1');
                assert.eq(write.out_of_bounds, '1');
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
