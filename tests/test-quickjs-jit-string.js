import assert from 'tjs:assert';

function append(prefix, number) {
    return prefix + number;
}

if (tjs.args[3] === 'concat-child') {
    const wide = '漢字';
    const long = 'a'.repeat(1024);

    for (let i = 0; i < 512; i++) {
        const number = (i % 68) - 18;
        const expected = String(number);

        assert.eq(append('item:', number), `item:${expected}`);
        assert.eq(append(wide, number), `${wide}${expected}`);
        assert.eq(append(long, number).slice(-expected.length), expected);
        assert.eq(append('', number), expected);
        assert.eq(append('item:', -0), 'item:0');
        assert.eq(Symbol(append('', number)).description, expected);
    }

    let conversions = 0;
    const input = { [Symbol.toPrimitive](hint) {
        assert.eq(hint, 'default');
        conversions++;

        return 12;
    } };

    assert.eq(append('item:', input), 'item:12');
    assert.eq(conversions, 1);
} else {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'concat-child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: '1',
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_SHADOW: '0',
            TJS_QUICKJS_JIT_STATS: '1',
        },
    });
    const [ status, stdout, stderr ] = await Promise.all([ proc.wait(), proc.stdout.text(), proc.stderr.text() ]);

    assert.eq(status.term_signal, null, stderr);
    assert.eq(status.exit_status, 0, stdout + stderr);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bconcat_int_hits=[1-9]\d*\b/.test(stderr), stderr);
    }
}
