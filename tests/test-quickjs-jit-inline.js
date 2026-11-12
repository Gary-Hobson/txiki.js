import assert from 'tjs:assert';

function addSmall(value) {
    return value + 7;
}

function subtractSmall(value) {
    return value - 255;
}

function inlineAddSite(target, value) {
    return target(value);
}

function inlineSubtractSite(target, value) {
    return target(value);
}

function exercise() {
    let sum = 0;

    for (let value = 0; value < 1024; value++) {
        sum += inlineAddSite(addSmall, value);
        sum += inlineSubtractSite(subtractSmall, value);
    }

    assert.eq(sum, 793600);
    assert.eq(inlineAddSite(addSmall, 2147483647), 2147483654);
    assert.eq(inlineSubtractSite(subtractSmall, -2147483648), -2147483903);
    assert.eq(inlineAddSite(addSmall, '5'), '57');

    let coerced = 0;
    const object = {
        valueOf() {
            coerced++;

            return 11;
        },
    };

    assert.eq(inlineAddSite(addSmall, object), 18);
    assert.eq(coerced, 1);

    let called = 0;

    const replacement = function(value) {
        called++;

        return value - 3;
    };

    assert.eq(inlineAddSite(replacement, 9), 6);
    assert.eq(called, 1);
    assert.eq(inlineAddSite(addSmall, 9), 16);
}

function inlineHits(stderr, name) {
    const line = stderr.split('\n').find(line => line.startsWith(`QuickJS feedback name=${name} `));
    const match = /inline_hits=(\d+)$/.exec(line ?? '');

    assert.ok(match, `Missing ${name} inline counters`);

    return Number(match[1]);
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    for (const mode of [ '0', '1' ]) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: {
                ...tjs.env,
                TJS_QUICKJS_JIT: mode,
                TJS_QUICKJS_JIT_BOXED: '1',
                TJS_QUICKJS_JIT_SHADOW: '0',
                TJS_QUICKJS_JIT_FEEDBACK: 'dump',
            },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
                const add = inlineHits(stderr, 'inlineAddSite');
                const subtract = inlineHits(stderr, 'inlineSubtractSite');

                assert.ok(mode === '0' ? add === 0 : add > 0, `Inline add hits: ${add}`);
                assert.ok(mode === '0' ? subtract === 0 : subtract > 0, `Inline subtract hits: ${subtract}`);
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
