import assert from 'tjs:assert';

function canonical(value) {
    return ({ escapeField: value }).escapeField;
}

function twoFields(unused, value) {
    return ({ ignored: unused, escapeField: value }).escapeField;
}

function throughLocal(value) {
    const literal = { escapeField: value };

    return literal.escapeField;
}

function choose(flag, value) {
    return flag ? ({ escapeField: value }).escapeField : ({ ignored: value }).ignored;
}

function returningObject(value) {
    return { escapeField: value };
}

function capture(value, publish) {
    const literal = { escapeField: value };

    publish(() => literal);

    return literal.escapeField;
}

function getter(value, record) {
    return ({
        get escapeField() {
            record.getters++;

            return value;
        },
    }).escapeField;
}

function inherited(proto) {
    return ({ __proto__: proto }).escapeField;
}

function computed(key, value) {
    return ({ [key()]: value }).escapeField;
}

function initializer(factory) {
    return ({ escapeField: factory() }).escapeField;
}

function exercise() {
    const token = { sentinel: true };
    const discarded = { unused: true };
    const record = { getters: 0, keys: 0, factories: 0, inherited: 0, prototype: 0 };
    const proto = {
        get escapeField() {
            record.inherited++;

            return token;
        },
    };
    const duplicate = new Function('first', 'last', 'return ({escapeField: first, escapeField: last}).escapeField;');
    let captured;

    Object.defineProperty(Object.prototype, 'escapeField', {
        configurable: true,
        get() {
            record.prototype++;

            return undefined;
        },
    });

    try {
        for (let i = 0; i < 320; i++) {
            assert.eq(canonical(token), token);
            assert.eq(twoFields(discarded, token), token);
            assert.eq(throughLocal(token), token);
            assert.eq(choose(i & 1, token), token);
            assert.eq(duplicate(discarded, token), token);
            assert.eq(getter(token, record), token);
            assert.eq(inherited(proto), token);
            assert.eq(computed(() => {
                record.keys++;

                return 'escapeField';
            }, token), token);
            assert.eq(initializer(() => {
                record.factories++;

                return token;
            }), token);
            assert.eq(capture(token, read => {
                captured = read;
            }), token);
            assert.ok(captured().escapeField === token);
            const one = returningObject(token);
            const two = returningObject(token);

            assert.ok(one !== two);
            assert.ok(one.escapeField === token && two.escapeField === token);
        }
    } finally {
        delete Object.prototype.escapeField;
    }

    assert.eq(record, { getters: 320, keys: 320, factories: 320, inherited: 320, prototype: 0 });
    console.log(JSON.stringify(record));
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
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

if (tjs.args[3] === 'child') {
    exercise();
} else {
    const interpreter = await run('0');
    const jit = await run('1');

    assert.eq(jit.stdout, interpreter.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bboxed_escape_literals=[1-9]\d*\b/.test(jit.stderr), jit.stderr);
        assert.ok(/\bboxed_returns=[1-9]\d*\b/.test(jit.stderr), jit.stderr);
    }

    if (tjs.env.TJS_QUICKJS_JIT_LAYOUT_REQUIRED === '1') {
        assert.ok(/\bboxed_escape_layouts=[1-9]\d*\b/.test(jit.stderr), jit.stderr);
    }
}
