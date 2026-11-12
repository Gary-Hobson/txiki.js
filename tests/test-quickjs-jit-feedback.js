import assert from 'tjs:assert';

function profileBinary(left, right) {
    return left + right;
}

function profileBranch(left, right) {
    if (left < right) {
        return 11;
    }

    return 22;
}

function profileCall(target, value) {
    return target(value);
}

function profileHot(value) {
    return value + 1;
}

function profileMixed(value) {
    return value + 1;
}

function profileString(value) {
    return value + 1;
}

function makeTarget(delta) {
    return value => value + delta;
}

function exercise() {
    const values = [ [ -4, 1 ], [ 2.5, -3 ], [ NaN, 0.5 ], [ -0, -0 ], [ 8, 1 ], [ '3', 2 ] ];
    const arithmetic = values.map(([ left, right ]) => profileBinary(left, right));

    assert.eq(arithmetic, [ -3, -0.5, NaN, -0, 9, '32' ]);
    assert.ok(Object.is(arithmetic[3], -0));
    assert.eq([ profileBranch(1, 2), profileBranch(5, 1), profileBranch(3, 3), profileBranch(4, 1) ],
        [ 11, 22, 22, 22 ]);

    const increment = makeTarget(1);
    const decrement = makeTarget(-1);
    const double = value => value * 2;

    // The two closures share bytecode but retain different captured values.
    assert.eq([ profileCall(increment, 10), profileCall(decrement, 10),
        profileCall(double, 10), profileCall(Math.abs, -10) ],
    [ 11, 9, 20, 10 ]);

    let total = 0;

    for (let value = 0; value < 1024; value++) {
        total += profileHot(value);
    }

    assert.eq(total, 524800);

    let mixedTotal = 0;
    let text;

    for (let i = 0; i < 8192; i++) {
        mixedTotal += profileMixed((i & 3) === 0 ? 1.25 : 1);
        text = profileString('x');
    }

    assert.eq(mixedTotal, 16896);
    assert.eq(text, 'x1');
}

function findSite(stderr, name, kind) {
    const match = stderr.match(new RegExp(`^QuickJS feedback name=${name} pc=\\d+ ${kind} (.+)$`, 'm'));

    assert.ok(match, `Missing ${name} ${kind} feedback`);

    return Object.fromEntries(match[1].split(' ').map(field => field.split('=')));
}

if (tjs.args[3] === 'child') {
    exercise();
} else {
    for (const mode of [ '0', '1', 'dump' ]) {
        const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
            stderr: 'pipe',
            env: { ...tjs.env, TJS_QUICKJS_JIT: '0', TJS_QUICKJS_JIT_FEEDBACK: mode },
        });
        const watchdog = setTimeout(() => proc.kill('SIGKILL'), tjs.env.TJS_GC_STRESS ? 60000 : 10000);

        try {
            const [ status, stderr ] = await Promise.all([ proc.wait(), proc.stderr.text() ]);

            assert.eq(status.term_signal, null);
            assert.eq(status.exit_status, 0, stderr);

            if (mode !== 'dump') {
                assert.ok(!stderr.includes('QuickJS feedback '));
            } else if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1' || stderr.includes('QuickJS feedback ')) {
                const binary = findSite(stderr, 'profileBinary', 'binary');
                const branch = findSite(stderr, 'profileBranch', 'branch');
                const call = findSite(stderr, 'profileCall', 'call');
                const hot = findSite(stderr, 'profileHot', 'binary');
                const mixed = findSite(stderr, 'profileMixed', 'binary');
                const string = findSite(stderr, 'profileString', 'binary');

                assert.eq(binary.samples, '6');
                assert.eq(binary.lhs_range, '1,-4,8');
                assert.eq(binary.rhs_range, '1,-3,2');
                assert.eq(binary.lhs_nan, '1');
                assert.eq(binary.rhs_nan, '0');
                assert.eq(binary.lhs_negative_zero, '1');
                assert.eq(binary.rhs_negative_zero, '1');
                assert.eq(Number.parseInt(binary.lhs_tags, 16).toString(2).replaceAll('0', '').length, 3);
                assert.eq(Number.parseInt(binary.rhs_tags, 16).toString(2).replaceAll('0', '').length, 2);
                assert.eq(binary.lhs_hist, '-7:1,0:2,8:3');
                assert.eq(binary.rhs_hist, '0:4,8:2');
                assert.eq([ branch.taken, branch.fallthrough ].sort(), [ '1', '3' ]);
                assert.eq([ call.targets, call.count0, call.count1, call.other ], [ '2', '2', '1', '1' ]);
                assert.ok(Number(hot.samples) >= 128 && Number(hot.samples) < 256, hot.samples);
                assert.eq(hot.lhs_hist, `0:${hot.samples}`);
                assert.eq(hot.rhs_hist, `0:${hot.samples}`);
                const [ ints, floats ] = mixed.lhs_hist.split(',').map(entry => Number(entry.split(':')[1]));

                assert.ok(floats > 0 && ints > 2 * floats, mixed.lhs_hist);
                assert.eq(ints + floats, Number(mixed.samples));
                assert.eq(mixed.rhs_hist, `0:${mixed.samples}`);
                assert.ok(Number(string.samples) > 255);
                assert.eq(string.lhs_hist, '-7:255');
                assert.eq(string.rhs_hist, `0:${string.samples}`);
            }
        } finally {
            clearTimeout(watchdog);
        }
    }
}
