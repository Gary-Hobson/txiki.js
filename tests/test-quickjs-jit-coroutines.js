import assert from 'tjs:assert';

function* generate(record) {
    try {
        const value = yield record.first;

        yield record.second + value;

        return record.last;
    } finally {
        record.close();
    }
}

async function settle(record) {
    try {
        const value = await record.first;

        return record.second + value;
    } catch (error) {
        record.close();
        throw error;
    }
}

async function* generateAsync(record) {
    try {
        yield record.first;

        const value = await record.second;

        yield record.third + value;

        return record.last;
    } finally {
        record.close();
    }
}

async function consume(iterable) {
    let total = 0;

    for await (const value of iterable) {
        total += value;

        if (total > 4) {
            break;
        }
    }

    return total;
}

function* relay(iterable) {
    return yield* iterable;
}

async function* relayAsync(iterable) {
    return yield* iterable;
}

async function exercise() {
    const events = [];
    const marker = new Error('coroutine error');
    let collect = false;
    const record = {
        get first() {
            events.push('first');

            return 1;
        },
        get second() {
            events.push('second');

            if (collect) {
                tjs.engine.gc.run();
            }

            return 2;
        },
        get third() {
            events.push('third');

            return 3;
        },
        get last() {
            events.push('last');

            return 4;
        },
        close() {
            events.push('close');
        },
    };

    for (let i = 0; i < 96; i++) {
        const generator = generate(record);
        const asyncGenerator = generateAsync(record);

        events.length = 0;
        assert.eq(generator.next(), { value: 1, done: false });
        assert.eq(generator.next(5), { value: 7, done: false });
        assert.eq(generator.next(), { value: 4, done: true });
        assert.eq(events.join(','), 'first,second,last,close');

        events.length = 0;
        assert.eq(await settle(record), 3);
        assert.eq(events.join(','), 'first,second');

        events.length = 0;
        assert.eq(await asyncGenerator.next(), { value: 1, done: false });
        assert.eq(await asyncGenerator.next(), { value: 5, done: false });
        assert.eq(await asyncGenerator.next(), { value: 4, done: true });
        assert.eq(events.join(','), 'first,second,third,last,close');

        assert.eq(await consume([ 1, 2, 3, 4 ]), 6);
        assert.eq(await consume([ 1, 2 ]), 3);

        const delegated = relay([ 7, 8 ]);
        const asyncDelegated = relayAsync([ 7, 8 ]);

        assert.eq(delegated.next(), { value: 7, done: false });
        assert.eq(delegated.return(i), { value: i, done: true });
        assert.eq(await asyncDelegated.next(), { value: 7, done: false });
        assert.eq(await asyncDelegated.return(i), { value: i, done: true });
    }

    collect = true;
    const stopped = generate(record);

    events.length = 0;
    assert.eq(stopped.next(), { value: 1, done: false });
    tjs.engine.gc.run();
    assert.eq(stopped.return(marker), { value: marker, done: true });
    assert.eq(events.join(','), 'first,close');

    const thrown = generate(record);

    events.length = 0;
    assert.eq(thrown.next(), { value: 1, done: false });
    assert.throws(() => thrown.throw(marker), marker);
    assert.eq(events.join(','), 'first,close');

    const asyncThrown = generateAsync(record);

    events.length = 0;
    assert.eq(await asyncThrown.next(), { value: 1, done: false });
    tjs.engine.gc.run();
    assert.eq(await asyncThrown.throw(marker).catch(error => error), marker);
    assert.eq(events.join(','), 'first,close');

    const rejected = {
        get first() {
            events.push('rejected');

            return Promise.reject(marker);
        },
        close: record.close,
    };

    events.length = 0;
    assert.eq(await settle(rejected).catch(error => error), marker);
    assert.eq(events.join(','), 'rejected,close');

    const throwing = {
        first: 1,
        get second() {
            events.push('throws');
            tjs.engine.gc.run();
            throw marker;
        },
        close: record.close,
    };

    events.length = 0;
    assert.eq(await settle(throwing).catch(error => error), marker);
    assert.eq(events.join(','), 'throws,close');

    const asyncIterable = {
        [Symbol.asyncIterator]() {
            events.push('iterator');

            return {
                async next() {
                    events.push('next');

                    return {
                        get done() {
                            events.push('done');

                            return false;
                        },
                        get value() {
                            events.push('value');

                            return 3;
                        },
                    };
                },
                async return() {
                    events.push('return');
                    tjs.engine.gc.run();

                    return {};
                },
            };
        },
    };

    events.length = 0;
    assert.eq(await consume(asyncIterable), 6);
    assert.eq(events.join(','), 'iterator,next,done,value,next,done,value,return');

    const delegate = {
        [Symbol.iterator]() {
            return this;
        },
        next(value) {
            events.push(`next:${value}`);
            tjs.engine.gc.run();

            return { value: value ?? 1, done: false };
        },
        return(value) {
            events.push(`return:${arguments.length}:${value}`);

            return { value, done: true };
        },
    };
    const delegated = relay(delegate);

    events.length = 0;
    assert.eq(delegated.next(), { value: 1, done: false });
    assert.eq(delegated.next(9), { value: 9, done: false });
    assert.throws(() => delegated.throw(marker), TypeError);
    assert.eq(events.join(','), 'next:undefined,next:9,return:0:undefined');

    const asyncDelegated = relayAsync({
        ...delegate,
        [Symbol.asyncIterator]() {
            return this;
        },
        async throw(error) {
            assert.is(error, marker);
            events.push('throw');

            return { value: 12, done: true };
        },
    });

    events.length = 0;
    assert.eq(await asyncDelegated.next(), { value: 1, done: false });
    assert.eq(await asyncDelegated.throw(marker), { value: 12, done: true });
    assert.eq(events.join(','), 'next:undefined,throw');

    events.length = 0;
    assert.eq(await settle(record), 3);
    assert.eq(events.join(','), 'first,second');
    console.log('coroutine semantics verified');
}

async function run(mode, shadow = '0') {
    const proc = tjs.spawn([ tjs.exePath, 'run', import.meta.path, 'child' ], {
        stdout: 'pipe',
        stderr: 'pipe',
        env: {
            ...tjs.env,
            TJS_QUICKJS_JIT: mode,
            TJS_QUICKJS_JIT_BOXED: '1',
            TJS_QUICKJS_JIT_SHADOW: shadow,
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
    await exercise();
} else {
    const control = await run('0');
    const boxed = await run('baseline');
    const shadow = await run('baseline', '1');

    assert.eq(boxed.stdout, control.stdout);
    assert.eq(shadow.stdout, control.stdout);

    if (tjs.env.TJS_QUICKJS_JIT_REQUIRED === '1') {
        const counters = [
            'boxed_generator_resumes',
            'boxed_async_resumes',
            'boxed_async_generator_resumes',
            'boxed_iterator_await_results',
            'boxed_delegate_next',
            'boxed_delegate_calls',
            'boxed_coro_terminals',
        ];

        for (const counter of counters) {
            assert.ok(new RegExp(`\\b${counter}=[1-9]\\d*\\b`).test(boxed.stderr), boxed.stderr);
            assert.ok(new RegExp(`\\b${counter}=0\\b`).test(shadow.stderr), shadow.stderr);
        }
    }
}
