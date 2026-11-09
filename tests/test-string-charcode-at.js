import assert from 'tjs:assert';

function check(value, msg) {
    assert.ok(value, msg);
}

function same(a, b, msg) {
    check(Object.is(a, b), msg + ': ' + a + ' vs ' + b);
}

const original = String.prototype.charCodeAt;
const reflectApply = Reflect.apply;

function call(receiver, ...args) {
    return reflectApply(original, receiver, args);
}

function reference(s, index) {
    const i = Math.trunc(Number(index)) || 0;

    if (i < 0 || i >= s.length) {
        return NaN;
    }

    const c = [ ...s.slice(i, i + 1) ][0];

    return c.codePointAt(0);
}

const sources = [ '', 'abc', '\0\xff\u0100\ud800x\udc00😀', 'abc'.repeat(700), '😀\u0100'.repeat(700) ];

for (let i = 0; i < 30; i++) {
    let rope = 'a'.repeat(9000);

    for (let j = 0; j < 5; j++) {
        rope += '😀bc'.repeat(200);
    }

    sources.push(rope, rope.slice(100, 8000), JSON.parse(JSON.stringify(rope)));
}

const indices = [ -2147483648, -100, -1, -0, 0, 1, 2, 3, 4, 500, 1024, 6999, 14000, 2147483647,
    -Infinity, Infinity, NaN, 0.9, 1.9, -0.9, undefined, null, false, true, '1', '2.7' ];
let cases = 0;

for (const s of sources) {
    for (const index of indices) {
        same(call(s, index), reference(s, index), 'code unit/coercion');
        same(call(new String(s), index), reference(s, index), 'boxed receiver');
        cases += 2;
    }

    same(call(s), reference(s, undefined), 'omitted index');
}

for (const receiver of [ null, undefined ]) {
    let touched = false;

    try {
        call(receiver, { valueOf() {
            touched = true;

            return 0;
        } }); throw Error('missing receiver error');
    } catch (e) {
        check(e instanceof TypeError && !touched, 'nullish receiver before index conversion');
    }
}

let order = [];
const receiver = { [Symbol.toPrimitive](hint) {
    order.push('receiver:' + hint);

    return 'abc';
} };
const index = { [Symbol.toPrimitive](hint) {
    order.push('index:' + hint);

    return 1;
} };

same(call(receiver, index), 98, 'coercion result');
check(order.join(',') === 'receiver:string,index:number', 'coercion order');
const thrown = new Error('sentinel');

try {
    call({ toString() {
        throw thrown;
    } }, index); throw Error('missing throw');
} catch (e) {
    check(e === thrown, 'receiver exception identity');
}

try {
    call('abc', { valueOf() {
        throw thrown;
    } }); throw Error('missing throw');
} catch (e) {
    check(e === thrown, 'index exception identity');
}

for (const arg of [ 1n, Symbol('index') ]) {
    try {
        call('abc', arg); throw Error('missing index TypeError');
    } catch (e) {
        check(e instanceof TypeError, 'invalid index type');
    }
}

try {
    call(Symbol('receiver'), 0); throw Error('missing receiver TypeError');
} catch (e) {
    check(e instanceof TypeError, 'invalid receiver type');
}

const saved = String.prototype.charCodeAt;

String.prototype.charCodeAt = function() {
    return 777;
};

same('abc'.charCodeAt(0), 777, 'overridden method remains observable');
same(call('abc', 0), 97, 'saved original still works');
String.prototype.charCodeAt = saved;
same(saved.bind('abc')(2), 99, 'bound original');
same(new Proxy(saved, { apply(target, thisArg, args) {
    return reflectApply(target, thisArg, args) + 1;
} }).call('abc', 0),
98, 'proxy interception');
assert.eq(cases, 4940, 'all charCodeAt value cases completed');
