/* global jitConversionTestGlobal: writable */
import assert from 'tjs:assert';

function lengthOf(value) {
    const { length } = value;

    return length;
}

function accumulate(target, key, increment) {
    return target[key] += increment();
}

function inspect(first, ...rest) {
    return [ first, arguments.length, rest, new.target ];
}

function metadata() {
    return import.meta;
}

class Base {
    constructor(value) {
        this.value = value;
        this.target = new.target;
    }
}

class Derived extends Base {
}

class Selected extends Base {
    constructor(value) {
        return value;
    }
}

function updateArgument(current, next) {
    current ??= next;

    return current;
}

function makeUpdate() {
    let current;

    return next => current ??= next;
}

function updateGlobal(next) {
    return jitConversionTestGlobal ??= next;
}

function assignGlobal(values) {
    [ jitConversionTestGlobal ] = values;

    return jitConversionTestGlobal;
}

function exercise() {
    const sloppy = Function('first', 'arguments[0] = 7; return [first, arguments.length, arguments[2]];');
    const named = Function('return function self(next) { const value = self &&= next; return [value, self]; };')();
    const dynamic = Function('return function* assign(scope) { with (scope) { field += yield 2; } };')();
    const updateCaptured = makeUpdate();
    const add = () => 4;

    try {
        for (let i = 0; i < 128; i++) {
            assert.eq(lengthOf('quickjs'), 7);
            assert.eq(lengthOf({ length: i }), i);
            assert.eq(accumulate({ k: i }, 'k', add), i + 4);
            assert.eq(inspect(1, 2, 3), [ 1, 3, [ 2, 3 ], undefined ]);
            assert.eq(inspect(), [ undefined, 0, [], undefined ]);
            assert.eq(sloppy(i, 'spare', i), [ 7, 3, i ]);
            assert.eq(named(i), [ i, named ]);
            assert.is(metadata(), import.meta);

            const child = new Derived(i);

            assert.eq(child.value, i);
            assert.is(child.target, Derived);
            assert.is(new Selected(child), child);
            assert.eq(updateArgument(null, i), i);
            assert.eq(updateArgument(0, i), 0);
            assert.eq(updateCaptured(i), 0);

            globalThis.jitConversionTestGlobal = null;
            assert.eq(updateGlobal(i), i);
            assert.eq(globalThis.jitConversionTestGlobal, i);
            assert.eq(assignGlobal([ i + 1 ]), i + 1);

            const scope = { field: i };
            const suspended = dynamic(scope);

            assert.eq(suspended.next(), { value: 2, done: false });
            assert.eq(suspended.next(4), { value: undefined, done: true });
            assert.eq(scope.field, i + 4);
        }

        delete globalThis.jitConversionTestGlobal;
        assert.throws(() => updateGlobal(1), ReferenceError);
    } finally {
        delete globalThis.jitConversionTestGlobal;
    }

    assert.throws(() => lengthOf(null), TypeError);
    assert.throws(() => Base(3), TypeError);
    assert.throws(() => Derived(3), TypeError);
    assert.throws(() => new Selected(3), TypeError);
    assert.throws(() => new Selected(undefined), ReferenceError);

    const alternate = Reflect.construct(Base, [ 2 ], Selected);

    assert.eq(alternate.value, 2);
    assert.is(alternate.target, Selected);
    assert.is(Object.getPrototypeOf(alternate), Selected.prototype);

    const actions = [];
    const marker = new Error('property conversion');
    const scope = {
        get field() {
            actions.push('reference-get');

            return 3;
        },
        set field(value) {
            actions.push(`reference-set:${value}`);
            tjs.engine.gc.run();
            throw marker;
        },
    };
    const suspended = dynamic(scope);

    assert.eq(suspended.next(), { value: 2, done: false });
    scope[Symbol.unscopables] = { field: true };
    assert.throws(() => suspended.next(5), marker);
    assert.eq(actions.join(','), 'reference-get,reference-set:8');
    actions.length = 0;

    const key = {
        [Symbol.toPrimitive](hint) {
            actions.push(hint);
            tjs.engine.gc.run();

            return 'k';
        },
    };
    const target = {
        get k() {
            actions.push('get');

            return 7;
        },
        set k(value) {
            actions.push(`set:${value}`);
        },
    };

    const rhs = () => {
        actions.push('rhs');

        return 4;
    };

    assert.eq(accumulate(target, key, rhs), 11);
    assert.throws(() => accumulate(null, key, rhs), TypeError);

    assert.throws(() => accumulate(target, {
        [Symbol.toPrimitive]() {
            actions.push('throw');
            throw marker;
        },
    }, rhs), marker);
    assert.eq(actions.join(','), 'string,get,rhs,set:11,throw');
    console.log('boxed conversion and reference semantics verified');
}

async function run(mode) {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_SHADOW: '0',
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

if (tjs.args.includes('child')) {
    exercise();
} else {
    const control = await run('0');
    const boxed = await run('baseline');

    assert.eq(boxed.stdout, control.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        const counters = [ 'boxed_conversions', 'boxed_ctor_checks', 'boxed_special_objects', 'boxed_references' ];

        for (const counter of counters) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
        }
    }
}
