import assert from 'tjs:assert';

function read(obj) {
    return obj.score;
}

function bitwiseNot(obj) {
    return ~obj.score;
}

function makeReader(initial) {
    var receiver = initial;

    return {
        read() {
            return receiver.score;
        },
        change(next) {
            receiver = next;
        },
    };
}

function readThenResume(obj, holder) {
    const score = obj.score;

    return 'mark' in holder ? score : -score;
}

function readOrCatch(obj) {
    try {
        return obj.score;
    } catch (error) {
        return error.message;
    }
}

function methodCall(obj, arg) {
    return obj.method(arg);
}

function construct(Constructor, arg) {
    return new Constructor(arg).value;
}

function writeAndRead(obj, value) {
    obj.score = value;

    return obj.score;
}

function element(array, index) {
    return array[index];
}

function writeElement(array, index, value) {
    array[index] = value;

    return array[index];
}

function sumElements(array, count) {
    let sum = 0;

    for (let index = 0; index < count; index += 1) {
        sum += array[index];
    }

    return sum;
}

function runCases() {
    let reads = 0;

    const plain = { score: 7 };
    const inherited = Object.create({
        get score() {
            reads++;

            if (reads % 11 === 0) {
                tjs.engine.gc.run();
            }

            return read(plain);
        },
    });
    const throwing = {
        get score() {
            reads++;

            throw new Error('getter failed');
        },
    };
    const receiver = {
        base: 8,
        method(value) {
            return this.base + value;
        },
    };
    const holder = { mark: true };
    const typed = new Int32Array([ -10, 0, 12 ]);
    const sparse = [ 1, 2, 3 ];
    const reader = makeReader(plain);

    delete sparse[1];

    function Record(value) {
        this.value = value;
    }

    for (let i = 0; i < 192; i++) {
        assert.eq(readThenResume(inherited, holder), 7);
        assert.eq(readOrCatch(throwing), 'getter failed');
        assert.eq(methodCall(receiver, i), i + 8);
        assert.eq(construct(Record, i), i);
        assert.eq(writeAndRead(plain, 7), 7);
        assert.eq(bitwiseNot(plain), -8);
        assert.eq(reader.read(), 7);
        assert.eq(element(typed, 2), 12);
        assert.eq(element(sparse, 1), undefined);
        assert.eq(element('abc', 1), 'b');
        assert.eq(writeElement(typed, 1, i), i);
        assert.eq(sumElements(typed, 3), i + 2);
    }

    assert.eq(reads, 384);
    assert.throws(() => readThenResume(inherited, null), TypeError);
    assert.eq(reads, 385);
    assert.eq(sumElements(new Int32Array(40000).fill(2), 40000), 80000);
    assert.eq(element(new Uint8Array(0), 0), undefined);

    reader.change({
        get score() {
            reader.change(plain);
            tjs.engine.gc.run();

            return 123;
        },
    });
    assert.eq(reader.read(), 123);
    assert.eq(reader.read(), 7);

    receiver.method = function(value) {
        return value * 3;
    };

    assert.eq(methodCall(receiver, 7), 21);

    const proxy = new Proxy(plain, {
        get(target, name, receiver) {
            reads++;

            return Reflect.get(target, name, receiver);
        },
    });

    assert.eq(readThenResume(proxy, {}), -7);
    assert.eq(reads, 386);
    console.log('boxed differential passed');
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

if (tjs.args.includes('child')) {
    runCases();
} else {
    const control = await run('0');
    const boxed = await run('1');

    assert.eq(boxed.stdout, control.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        for (const counter of [
            'boxed_entries', 'boxed_returns', 'boxed_resumes', 'boxed_exceptions',
            'boxed_fuel_exits', 'boxed_fields', 'boxed_elements', 'boxed_calls', 'boxed_rc_pairs_elided',
        ]) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
        }
    }
}
