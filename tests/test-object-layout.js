import assert from 'tjs:assert';

function simple(i) {
    return { first: i, second: String(i), third: { nested: i, value: i + 1 } };
}

for (let i = 0; i < 1000; i++) {
    const x = simple(i);

    assert.eq(x.first, i);
    assert.eq(x.second, String(i));
    assert.eq(x.third.value, i + 1);
    assert.eq(Reflect.ownKeys(x).join(','), 'first,second,third');
    assert.eq(Object.getPrototypeOf(x), Object.prototype);
    const d = Object.getOwnPropertyDescriptor(x, 'first');

    assert.ok(d.enumerable && d.configurable && d.writable);
    x.extra = 4;
    delete x.second;
    Object.defineProperty(x, 'third', { get() {
        return i;
    } });
    assert.eq(x.third, i);
    assert.eq(simple(i).third.nested, i);
}

const log = [];

const side = i => {
    log.push(i);

    return { index: i, text: String(i) };
};

function recursive(n) {
    return { a: side(n), b: n ? recursive(n - 1) : side(-1), c: side(n + 10) };
}

for (let i = 0; i < 50; i++) {
    log.length = 0;
    const value = recursive(3);

    assert.eq(log.join(','), '3,2,1,0,-1,10,11,12,13');
    assert.eq(value.b.b.a.index, 1);
}

function throwing() {
    return { a: side(1), b: (() => {
        side(2); throw new Error('initializer');
    })(), c: side(3) };
}

for (let i = 0; i < 100; i++) {
    log.length = 0;

    try {
        throwing(); assert.fail('must throw');
    } catch (e) {
        assert.eq(e.message, 'initializer');
    }

    assert.eq(log.join(','), '1,2');
}

function dynamic(k, cond) {
    return { a: 1, b: 2, [k]: 3, ...{ tail: 4 }, get x() {
        return this.a;
    },
    method() {
        return super.toString();
    }, c: cond ? side(5) : side(6) };
}

const d = dynamic('a', true);

assert.eq(d.a, 3); assert.eq(d.b, 2); assert.eq(d.tail, 4); assert.eq(d.x, 3);
assert.eq(d.method(), '[object Object]');
assert.eq(Reflect.ownKeys(d).join(','), 'a,b,tail,x,method,c');
const duplicate = new Function('return ({ a: 1, b: 2, a: 3 });');

assert.eq(duplicate().a, 3); assert.eq(Object.keys(duplicate()).join(','), 'a,b');
const proto = { inherited: 99 };

const special = () => {
    return { a: 1, b: 2, __proto__: proto };
};

assert.eq(Object.getPrototypeOf(special()), proto); assert.eq(special().inherited, 99);

const literalProto = () => {
    return { '__proto__': null, a: 1, b: 2 };
};

assert.eq(Object.getPrototypeOf(literalProto()), null);

const ordered = () => {
    return { 20: 'x', 1: 'y', z: 'z', 0: 'q', a: 'r' };
};

assert.eq(Object.keys(ordered()).join(','), '0,1,20,z,a');

const named = () => {
    return { a: function () {}, b: class {}, c: () => 1 };
};

assert.eq(named().a.name, 'a'); assert.eq(named().b.name, 'b'); assert.eq(named().c.name, 'c');

async function asyncValue() {
    return { a: 1, b: await Promise.resolve(2), c: 3 };
}

assert.eq((await asyncValue()).b, 2);

function* generator() {
    return { a: 1, b: yield 2, c: 3 };
}

const g = generator();

assert.eq(g.next().value, 2); assert.eq(g.next(4).value.b, 4);

function conditional(flag) {
    return { a: 1, b: flag ? side(1) : side(2), c: 3 };
}

assert.eq(conditional(false).b.index, 2);
const getter = { get value() {
    return simple(8);
} };

const values = () => {
    return { a: getter.value, b: getter.value, c: getter.value };
};

assert.eq(values().b.first, 8);

function evaled() {
    return { a: eval('simple(9)'), b: eval('simple(10)') };
}

assert.eq(evaled().b.first, 10);
const many = new Function('return {' + Array.from({ length: 65 }, (_, i) => `key${i}:${i}`).join(',') + '}');

assert.eq(many().key64, 64);
