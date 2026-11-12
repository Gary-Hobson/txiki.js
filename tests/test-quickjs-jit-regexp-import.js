import assert from 'tjs:assert';

function makeRegExp() {
    return /a+(?<tail>b)?/gy;
}

function loadModule(specifier, options) {
    return import(specifier, options);
}

async function exercise() {
    const relativePath = './helpers/file-url-mod.js';
    const expectedModule = await import(relativePath);
    let lastRegExp;

    for (let i = 0; i < 100; i++) {
        const re = makeRegExp();

        assert.ok(re !== lastRegExp);
        assert.eq(re.source, 'a+(?<tail>b)?');
        assert.eq(re.flags, 'gy');
        assert.eq(re.lastIndex, 0);
        assert.eq(re.exec('aaab').groups.tail, 'b');
        assert.eq(re.lastIndex, 4);
        lastRegExp = re;

        assert.is(await loadModule(relativePath), expectedModule);
    }

    const order = [];
    const marker = new Error('import options getter');
    const specifier = {
        toString() {
            order.push('specifier');

            return relativePath;
        },
    };
    const options = {
        get with() {
            order.push('options');
            throw marker;
        },
    };

    const rejected = loadModule(specifier, options);

    assert.ok(rejected instanceof Promise);
    assert.is(await rejected.catch(error => error), marker);
    assert.eq(order, [ 'specifier', 'options' ]);
    assert.ok((await loadModule(relativePath, 1).catch(error => error)) instanceof TypeError);
    assert.eq(expectedModule.x, 42);
    console.log('100 independent RegExp objects and module imports');
}

async function run(jit) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: jit,
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
    await exercise();
} else {
    const interpreter = await run('0');
    const native = await run('1');

    assert.eq(native.stdout, interpreter.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        assert.ok(/\bboxed_regexps=69\b/.test(native.stderr), native.stderr);
        assert.ok(/\bboxed_dynamic_imports=71\b/.test(native.stderr), native.stderr);
    }
}
