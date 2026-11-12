import assert from 'tjs:assert';

class Base {
    constructor(value) {
        this.value = value;
        this.target = new.target;
    }

    get score() {
        return this.value;
    }

    set score(value) {
        this.value = value;
    }

    get receiver() {
        return this;
    }
}

class Derived extends Base {
    #secret;

    constructor(value) {
        super(value);
        this.#secret = value * 2;
    }

    #read() {
        return this.#secret;
    }

    read() {
        return this.#read();
    }

    write(value) {
        return this.#secret = value;
    }

    readSuper(key) {
        return super[key];
    }

    writeSuper(key, value) {
        return super[key] = value;
    }
}

function exercise() {
    for (let i = 0; i < 144; i++) {
        const value = new Derived(i);
        const replacement = { i };

        assert.is(value.target, Derived);
        assert.eq(value.read(), i * 2);
        assert.is(value.write(replacement), replacement);
        assert.is(value.read(), replacement);
        assert.eq(value.readSuper('score'), i);
        assert.eq(value.writeSuper('score', i + 1), i + 1);
        assert.eq(value.value, i + 1);
        assert.is(value.readSuper('receiver'), value);
    }

    const value = new Derived(3);
    const marker = new Error('super setter');
    const calls = [];
    const key = {
        [Symbol.toPrimitive](hint) {
            calls.push(hint);
            tjs.engine.gc.run();

            return 'score';
        },
    };
    const receiver = new Proxy(value, {
        get(target, property, receiver) {
            calls.push(`get:${property}`);

            return Reflect.get(target, property, receiver);
        },
        set(target, property, replacement) {
            calls.push(`set:${property}:${replacement}`);
            tjs.engine.gc.run();
            throw marker;
        },
    });

    assert.eq(Derived.prototype.readSuper.call(receiver, key), 3);
    assert.throws(() => Derived.prototype.writeSuper.call(receiver, key, 8), marker);
    assert.eq(calls.join(','), 'string,get:value,string,set:value:8');
    assert.eq(value.value, 3);
    assert.throws(() => Derived.prototype.read.call(receiver), TypeError);
    assert.throws(() => Derived.prototype.write.call(receiver, {}), TypeError);
    assert.eq(value.read(), 6);

    Object.setPrototypeOf(Derived.prototype, null);

    try {
        assert.throws(() => Derived.prototype.readSuper.call(value, 'score'), TypeError);
        assert.throws(() => Derived.prototype.writeSuper.call(value, 'score', 9), TypeError);
    } finally {
        Object.setPrototypeOf(Derived.prototype, Base.prototype);
    }

    const inheritedConstructor = new Proxy(Base, {
        construct(target, args, newTarget) {
            calls.push(`construct:${args[0]}`);
            assert.is(newTarget, Derived);
            tjs.engine.gc.run();

            if (args[0] === 13) {
                throw marker;
            }

            return Reflect.construct(target, args, newTarget);
        },
    });

    Object.setPrototypeOf(Derived, inheritedConstructor);

    try {
        const child = new Derived(9);

        assert.eq(child.value, 9);
        assert.eq(child.read(), 18);
        assert.is(child.target, Derived);
        assert.throws(() => new Derived(13), marker);
        assert.eq(calls.join(','), 'string,get:value,string,set:value:8,construct:9,construct:13');
    } finally {
        Object.setPrototypeOf(Derived, Base);
    }

    console.log('boxed class semantics verified');
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
        const counters = [
            'boxed_super_reads',
            'boxed_super_writes',
            'boxed_private_reads',
            'boxed_private_writes',
            'boxed_brand_checks',
        ];

        for (const counter of counters) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
        }
    }
}
