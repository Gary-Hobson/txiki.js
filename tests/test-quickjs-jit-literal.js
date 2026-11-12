import assert from 'tjs:assert';

function single(value) {
    return { value };
}

function record(value, tag) {
    return { value, tag };
}

function array(value, tag) {
    return [ value, tag, null ];
}

function computed(key, value) {
    return { [key]: value };
}

function sparse(value) {
    // eslint-disable-next-line no-sparse-arrays
    return [ , , value ];
}

function withPrototype(proto, value) {
    return { __proto__: proto, value };
}

function mixed(first, last) {
    return {
        value: first,
        get read() {
            return first;
        },
        // eslint-disable-next-line no-dupe-keys
        value: last,
    };
}

function spread(values, source) {
    return [ { ...source }, ...values ];
}

function exercise() {
    const value = { marker: true };
    const tag = Symbol('tag');
    const proto = { inherited: true };
    const source = { get value() {
        return value;
    } };

    for (let i = 0; i < 256; i++) {
        const obj = single(value);
        const pair = record(value, tag);
        const arr = array(value, tag);
        const holes = sparse(value);
        const custom = withPrototype(proto, value);
        const accessors = mixed(tag, value);
        const elements = spread([ tag, value ], source);

        assert.ok(obj !== single(value) && obj.value === value);
        assert.ok(pair.value === value && pair.tag === tag);
        assert.eq(arr.length, 3);
        assert.ok(arr[0] === value && arr[1] === tag && arr[2] === null);
        assert.ok(computed(tag, value)[tag] === value);
        assert.eq(holes.length, 3);
        assert.ok(!(0 in holes) && !(1 in holes) && holes[2] === value);
        assert.ok(Object.getPrototypeOf(custom) === proto && custom.inherited && custom.value === value);
        assert.ok(accessors.value === value && accessors.read === tag);
        assert.ok(elements[0].value === value && elements[1] === tag && elements[2] === value);
    }

    const marker = new Error('literal input');
    let keyCount = 0;
    let spreadCount = 0;

    try {
        computed({ [Symbol.toPrimitive]() {
            keyCount++;
            throw marker;
        } }, value);
        assert.fail('computed property key should throw');
    } catch (error) {
        assert.ok(error === marker);
    }

    try {
        spread([], { get value() {
            spreadCount++;
            throw marker;
        } });
        assert.fail('object spread should throw');
    } catch (error) {
        assert.ok(error === marker);
    }

    assert.eq(keyCount, 1);
    assert.eq(spreadCount, 1);
    console.log(JSON.stringify({ keyCount, spreadCount }));
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
    const native = await run('1');

    assert.eq(native.stdout, interpreter.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bboxed_literal_objects=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_literal_arrays=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_literal_fields=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_exceptions=[1-9]\d*\b/.test(native.stderr), native.stderr);
    }

    if (tjs.env.TJS_QUICKJS_JIT_LAYOUT_REQUIRED === '1') {
        assert.ok(/\bboxed_literal_layouts=[1-9]\d*\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_literal_layout_fields=[1-9]\d*\b/.test(native.stderr), native.stderr);
    }
}
