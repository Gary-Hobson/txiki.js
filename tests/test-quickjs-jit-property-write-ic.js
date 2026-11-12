import assert from 'tjs:assert';

function writeSite(object, value) {
    object.x = value;
}

function mutationSite(object, value) {
    object.x = value;
}

function writeLength(object, value) {
    object.length = value;
}

function transitionSite(object, value) {
    object.added = value;
}

function exercise() {
    const shapes = [ { x: 0 }, { a: 1, x: 0 }, { b: 2, c: 3, x: 0 } ];

    for (const object of shapes) {
        for (let i = 0; i < 96; i++) {
            writeSite(object, i);
            assert.eq(object.x, i);
        }
    }

    assert.eq(shapes[1].a, 1);
    assert.eq(shapes[2].c, 3);

    const owner = { before: true, x: null, after: true };

    for (let i = 0; i < 96; i++) {
        mutationSite(owner, { index: i });
    }

    assert.eq(owner.x.index, 95);
    const held = owner.x;

    mutationSite(owner, { index: 96 });
    assert.eq(held.index, 95);
    assert.eq(owner.x.index, 96);

    Object.defineProperty(owner, 'x', { writable: false });
    assert.throws(() => mutationSite(owner, 17), TypeError);
    assert.eq(owner.x.index, 96);
    Object.defineProperty(owner, 'x', { writable: true });
    mutationSite(owner, 21);
    assert.eq(owner.x, 21);

    let setterCalls = 0;
    let setterValue;
    let receiver;

    Object.defineProperty(owner, 'x', {
        configurable: true,
        set(value) {
            setterCalls++;
            setterValue = value;
            receiver = this;
        },
    });
    mutationSite(owner, 31);
    assert.eq(setterCalls, 1);
    assert.eq(setterValue, 31);
    assert.ok(receiver === owner);

    const child = Object.create(owner);

    mutationSite(child, 41);
    assert.eq(setterCalls, 2);
    assert.ok(receiver === child);
    assert.ok(!Object.hasOwn(child, 'x'));

    delete owner.x;
    owner.extra = 7;
    mutationSite(owner, 51);
    mutationSite(owner, 61);
    assert.eq(owner.x, 61);
    assert.eq(owner.before, true);
    assert.eq(owner.after, true);
    assert.eq(owner.extra, 7);
    mutationSite(child, 71);
    assert.ok(Object.hasOwn(child, 'x'));
    assert.eq(child.x, 71);
    assert.eq(owner.x, 61);

    let proxyCalls = 0;
    const proxy = new Proxy(owner, {
        set(target, key, value, receiver) {
            proxyCalls++;

            return Reflect.set(target, key, value, receiver);
        },
    });

    mutationSite(proxy, 81);
    assert.eq(proxyCalls, 1);
    assert.eq(owner.x, 81);
    const marker = new Error('proxy write');

    try {
        mutationSite(new Proxy(owner, { set() {
            throw marker;
        } }), 91);
        assert.fail('Expected proxy trap');
    } catch (error) {
        assert.eq(error, marker);
    }

    const ordinary = { length: 0 };
    const array = [ 1, 2, 3 ];

    for (let i = 0; i < 40; i++) {
        writeLength(ordinary, i);
    }

    writeLength(array, 1);
    assert.eq(array, [ 1 ]);
    assert.throws(() => writeLength(array, -1), RangeError);
    assert.eq(ordinary.length, 39);

    for (let i = 0; i < 96; i++) {
        const target = Object.create(null);
        const payload = { index: i };

        transitionSite(target, payload);
        const descriptor = Object.getOwnPropertyDescriptor(target, 'added');

        assert.ok(descriptor.value === payload);
        assert.ok(descriptor.configurable && descriptor.writable && descriptor.enumerable);
        assert.eq(target.added.index, i);
    }

    const sealed = Object.preventExtensions(Object.create(null));

    assert.throws(() => transitionSite(sealed, 17), TypeError);
    assert.ok(!Object.hasOwn(sealed, 'added'));

    let inheritedCalls = 0;
    const inherited = Object.create({
        set added(value) {
            inheritedCalls++;
            assert.eq(value, 31);
        },
    });

    transitionSite(inherited, 31);
    assert.eq(inheritedCalls, 1);
    assert.ok(!Object.hasOwn(inherited, 'added'));
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    for (const [ mode, feedback ] of [ [ '0', '0' ], [ '0', 'dump' ], [ '1', 'dump' ] ]) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: {
                ...tjs.env,
                TJS_QUICKJS_JIT: mode,
                TJS_QUICKJS_JIT_BOXED: '1',
                TJS_QUICKJS_JIT_FEEDBACK: feedback,
            },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null, stderr);
            assert.eq(status.exit_status, 0, stderr);

            if (feedback === 'dump' &&
                (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1' || stderr.includes('QuickJS feedback '))) {
                const line = stderr.split('\n').find(line => line.startsWith('QuickJS feedback name=writeSite '));
                const pattern = /property_write shapes=2 hits=(\d+) misses=(\d+) megamorphic=1 boxed_hits=(\d+)$/;
                const state = pattern.exec(line ?? '');

                assert.ok(state, stderr);
                assert.ok(Number(state[1]) > 0 && Number(state[2]) >= 3, line);
                assert.eq(Number(state[3]) > 0, mode === '1', line);

                const transition = stderr.split('\n').find(line => line.startsWith(
                    'QuickJS feedback name=transitionSite ') && line.includes(' property_transition '));
                const counts = /property_transition hits=(\d+) boxed_hits=(\d+)$/.exec(transition ?? '');

                assert.ok(counts, stderr);
                assert.eq(Number(counts[1]), 96, transition);
                assert.eq(Number(counts[2]) > 0, mode === '1', transition);
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
