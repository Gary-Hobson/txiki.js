import assert from 'tjs:assert';

function warm(fn, count) {
    let total = 0;

    for (let i = 0; i < count; i++) {
        total += fn(i);
    }

    return total;
}

function load(array, index) {
    return array[index];
}

function length(string) {
    return string.length;
}

function charCode(string, index) {
    return string.charCodeAt(index);
}

function warmData() {
    const typed = new Int32Array([ 19, 23 ]);
    const object = {};
    const dense = [ object ];
    const string = 'a\u2603';

    for (let i = 0; i < 96; i++) {
        assert.eq(load(typed, i & 1), i & 1 ? 23 : 19);
        assert.ok(load(dense, 0) === object);
        assert.eq(length(string), 2);
        assert.eq(charCode(string, 1), 0x2603);
    }
}

function exercise() {
    const oldest = new Function('value', 'return value + 1;');
    const newest = [];
    const callCount = 96;
    const expected = callCount * (callCount + 1) / 2;

    warmData();
    assert.eq(warm(oldest, 1024), 1024 * 1025 / 2);

    // 2304 simultaneously live code pages exceed the 8 MiB budget on
    // AArch64 systems with either 4 KiB or 16 KiB pages.
    for (let i = 0; i < 2304; i++) {
        const fn = new Function('value', 'return value + 1;');

        newest.push(fn);
        assert.eq(warm(fn, 32), 32 * 33 / 2);
    }

    for (const fn of newest) {
        assert.eq(warm(fn, callCount), expected);
    }

    assert.eq(warm(oldest, 1024), 1024 * 1025 / 2);
    warmData();
    console.log(newest.length, oldest(2));
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
            TJS_QUICKJS_JIT_BOXED: '0',
            TJS_QUICKJS_JIT_DATA: '1',
        },
    });
    const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

    try {
        const [ status, stdout, stderr ] = await Promise.all([ proc.wait(), proc.stdout.text(), proc.stderr.text() ]);

        assert.eq(status.term_signal, null, stderr);
        assert.eq(status.exit_status, 0, stderr);
        assert.eq(stdout.trim(), '2304 3');

        if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
            assert.ok(/\bcache_deferrals=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\bcache_evictions=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\bcache_reinstalls=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\bdata_cache_evictions=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\bdata_cache_reinstalls=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\btyped_load_hits=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\barray_load_hits=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\bstring_length_hits=[1-9]\d*\b/.test(stderr), stderr);
            assert.ok(/\bstring_char_code_hits=[1-9]\d*\b/.test(stderr), stderr);
        }
    } finally {
        clearTimeout(watchdog);
    }
}
