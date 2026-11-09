function check(v, why) { if (!v) throw new Error(why); }
function eq(a, b, why) { check(Object.is(a, b), why + ': ' + String(a)); }
function throws(fn, ctor, why) {
    let thrown = false;
    try { fn(); } catch (e) { thrown = e instanceof ctor; }
    check(thrown, why);
}
const original = String.prototype.charCodeAt;
const sample = 'A\u00ff\u6c34\ud83d\ude00\ud800';
const expected = [65, 255, 27700, 55357, 56832, 55296];
for (let round = 0; round < 200; round++) {
    for (let i = 0; i < expected.length; i++) eq(sample.charCodeAt(i), expected[i], 'UTF-16 code unit');
    eq(sample.charCodeAt(-1), NaN, 'negative index');
    eq(sample.charCodeAt(6), NaN, 'past end');
    eq(''.charCodeAt(0), NaN, 'empty');
}
for (const index of [-2147483648, 2147483647, Infinity, -Infinity, 1e100])
    eq(sample.charCodeAt(index), NaN, 'large/out of range');
eq(sample.charCodeAt(-0), 65, 'minus zero');
eq(sample.charCodeAt(1.75), 255, 'float conversion fallback');
eq(sample.charCodeAt(NaN), 65, 'NaN conversion fallback');
eq(sample.charCodeAt(), 65, 'missing argument padding');
eq(sample.charCodeAt(undefined), 65, 'undefined');
eq(sample.charCodeAt('2'), 27700, 'string index conversion');
eq(sample.charCodeAt(true), 255, 'boolean index conversion');
throws(() => sample.charCodeAt(Symbol()), TypeError, 'symbol index throws');
throws(() => sample.charCodeAt(1n), TypeError, 'BigInt index throws');
throws(() => original.call(null, 0), TypeError, 'null receiver throws');
throws(() => original.call(undefined, 0), TypeError, 'undefined receiver throws');
throws(() => original.call(Symbol(), 0), TypeError, 'symbol receiver throws');
eq(original.call(1234, 2), 51, 'number receiver conversion');
eq(original.call(new String(sample), 2), 27700, 'boxed string fallback');

const order = [];
const receiver = { toString() { order.push('receiver'); return sample; } };
const index = { valueOf() { order.push('index'); return 2; } };
eq(original.call(receiver, index), 27700, 'coercion result');
eq(order.join(','), 'receiver,index', 'coercion order');
throws(() => original.call(sample, { valueOf() { throw new RangeError('index'); } }),
       RangeError, 'coercion exception preserved');
let extraCalls = 0;
eq(sample.charCodeAt(0, { valueOf() { extraCalls++; throw 1; } }, Symbol()), 65, 'extra arguments ignored');
eq(extraCalls, 0, 'extra arguments not coerced');
let rope = '';
for (let i = 0; i < 300; i++) rope += 'x\u6c34';
eq(rope.charCodeAt(599), 27700, 'rope fallback');

try {
    String.prototype.charCodeAt = function charCodeAt(i) { return 800 + i; };
    eq(sample.charCodeAt(3), 803, 'same name replacement is not intrinsic');
    eq(original.call(sample, 3), 55357, 'saved actual builtin still works');
    let getters = 0;
    Object.defineProperty(String.prototype, 'charCodeAt', {
        configurable: true,
        get() { getters++; return function (i) { return 900 + i; }; }
    });
    eq(sample.charCodeAt(4), 904, 'replacement getter is observed');
    eq(getters, 1, 'method getter once');
} finally {
    Object.defineProperty(String.prototype, 'charCodeAt', {
        value: original, writable: true, configurable: true
    });
}
let applies = 0;
const proxy = new Proxy(original, { apply(fn, recv, args) { applies++; return Reflect.apply(fn, recv, args); } });
eq(proxy.call(sample, 1), 255, 'proxy apply fallback');
eq(applies, 1, 'proxy trap called');
eq(original.bind(sample)(2), 27700, 'bound function preserves receiver');
throws(() => new original(0), TypeError, 'not constructable');

// Arg temporaries and their effects remain in the original VM call/cleanup.
let effects = 0;
function value() { effects++; return sample; }
function position() { effects++; return 2; }
eq(value().charCodeAt(position()), 27700, 'evaluated receiver and argument');
eq(effects, 2, 'evaluation count');
tjs.engine.gc.run();
console.log('string intrinsic correctness passed');
