import assert from 'tjs:assert';

function propertySite(object) {
    return object.x;
}

function mutationSite(object) {
    return object.x;
}

function exercise() {
    const shapes = [ { x: 11 }, { a: 1, x: 22 }, { b: 1, c: 2, x: 33 } ];

    for (const [ index, object ] of shapes.entries()) {
        for (let i = 0; i < 5; i++) {
            assert.eq(propertySite(object), 11 * (index + 1));
        }
    }

    const owner = { before: true, x: 41, after: true };
    const child = Object.create(owner);

    assert.eq(mutationSite(owner), 41);
    assert.eq(mutationSite(owner), 41);

    let getterCalls = 0;

    Object.defineProperty(owner, 'x', {
        configurable: true,
        get() {
            getterCalls++;

            return this === owner ? 51 : 61;
        },
    });
    assert.eq(mutationSite(owner), 51);
    assert.eq(mutationSite(child), 61);
    assert.eq(getterCalls, 2);

    delete owner.x;
    assert.eq(mutationSite(owner), undefined);
    assert.eq(mutationSite(child), undefined);

    owner.x = 71;
    assert.eq(mutationSite(owner), 71);
    assert.eq(mutationSite(child), 71);

    const marker = new Error('proxy trap');
    const proxy = new Proxy(owner, { get() {
        throw marker;
    } });

    try {
        mutationSite(proxy);
        assert.fail('Expected proxy trap');
    } catch (error) {
        assert.eq(error, marker);
    }
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    for (const mode of [ '0', 'dump' ]) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: { ...tjs.env, TJS_QUICKJS_JIT: '0', TJS_QUICKJS_JIT_FEEDBACK: mode },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            if (mode === 'dump' &&
                (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1' || stderr.includes('QuickJS feedback '))) {
                const line = stderr.split('\n').find(line => line.startsWith('QuickJS feedback name=propertySite '));
                const state = /property shapes=2 hits=(\d+) misses=(\d+) megamorphic=1$/.exec(line ?? '');

                assert.ok(state, 'Expected property IC to become megamorphic');
                assert.eq(state.slice(1), [ '8', '3' ]);
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
