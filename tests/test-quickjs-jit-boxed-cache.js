import assert from 'tjs:assert';

function read(record) {
    return record.value + 1;
}

function reenter(record, callback) {
    return record.value + callback();
}

function* generate(record) {
    const value = yield record.value;

    yield record.value + value;
}

function warmGenerator(record) {
    for (let i = 0; i < 96; i++) {
        const iterator = generate(record);

        assert.eq(iterator.next(), { value: 19, done: false });
        assert.eq(iterator.next(4), { value: 23, done: false });
        assert.eq(iterator.next(), { value: undefined, done: true });
    }
}

function exercise() {
    const record = { value: 19 };
    const live = [];

    warmGenerator(record);
    const suspended = generate(record);

    assert.eq(suspended.next(), { value: 19, done: false });

    for (let i = 0; i < 96; i++) {
        assert.eq(read(record), 20);
        assert.eq(reenter(record, () => 4), 23);
    }

    const total = reenter(record, () => {
        let sum = 0;

        // More than 512 live pages exhaust the 2 MiB boxed cache even
        // when executable mappings use 4 KiB pages.
        for (let i = 0; i < 768; i++) {
            const fn = new Function('record', 'return record.value + 1;');

            live.push(fn);

            for (let j = 0; j < 96; j++) {
                sum += fn(record);
            }
        }

        tjs.engine.gc.run();

        return sum;
    });

    assert.eq(total, 19 + 768 * 96 * 20);
    assert.eq(suspended.next(4), { value: 23, done: false });
    assert.eq(suspended.next(), { value: undefined, done: true });
    warmGenerator(record);

    for (let i = 0; i < 96; i++) {
        assert.eq(read(record), 20);
    }

    console.log(live.length, total);
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: 'baseline',
            TJS_QUICKJS_JIT_STATS: '1',
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_SHADOW: '0',
        },
    });
    const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

    try {
        const [ status, stdout, stderr ] = await Promise.all([ proc.wait(), proc.stdout.text(), proc.stderr.text() ]);

        assert.eq(status.term_signal, null, stderr);
        assert.eq(status.exit_status, 0, stderr);
        assert.eq(stdout.trim(), '768 1474579');

        if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
            for (const counter of [
                'boxed_cache_evictions',
                'boxed_cache_reinstalls',
                'coro_cache_evictions',
                'coro_cache_reinstalls',
                'boxed_cache_pinned',
            ]) {
                assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(stderr), stderr);
            }
        }
    } finally {
        clearTimeout(watchdog);
    }
}
