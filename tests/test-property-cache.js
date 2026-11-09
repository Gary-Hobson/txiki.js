import assert from 'tjs:assert';


function readCached(object) {
    return object.cached;
}

function callCachedMethod(object) {
    return object.cachedMethod();
}

const first = { cached: 11, other: 'first' };
const second = { before: 'second', cached: 22 };
const third = { before: 0, middle: 1, cached: 33 };

for (let i = 0; i < 32; i++) {
    assert.eq(readCached(first), 11, 'same property in the first slot');
    assert.eq(readCached(second), 22, 'same property in the second slot');
    assert.eq(readCached(third), 33, 'same property in the third slot');
    assert.eq(readCached({}), undefined, 'hint outside the current shape is ignored');
    assert.eq(readCached({ unrelated: 44 }), undefined, 'hint for another atom is ignored');
}

const readded = { cached: 'old', other: 'unchanged' };

assert.eq(readCached(readded), 'old');
delete readded.cached;
assert.eq(readCached(readded), undefined, 'deleted cached property is absent');
readded.between = 'new slot';
readded.cached = 'new';
assert.eq(readCached(readded), 'new', 're-added property uses its current slot');
assert.eq(readded.other, 'unchanged');

const compacted = {};

for (let i = 0; i < 96; i++) {
    compacted[`padding${i}`] = i;
}

compacted.cached = 'survivor';
assert.eq(readCached(compacted), 'survivor');

// Deleting most properties forces shape compaction and moves the surviving slot.
for (let i = 0; i < 96; i++) {
    delete compacted[`padding${i}`];
    assert.eq(readCached(compacted), 'survivor', 'read survives repeated shape compaction');
}

assert.eq(Object.keys(compacted), [ 'cached' ]);
delete compacted.cached;
assert.eq(readCached(compacted), undefined);
compacted.cached = 'after compaction';
assert.eq(readCached(compacted), 'after compaction');

let getterCalls = 0;
const accessor = { cached: 'data', backing: 'getter value' };

assert.eq(readCached(accessor), 'data');
Object.defineProperty(accessor, 'cached', {
    configurable: true,
    get() {
        getterCalls++;
        assert.is(this, accessor, 'own getter receives the original object');

        return this.backing;
    }
});
assert.eq(readCached(accessor), 'getter value', 'data slot changed into an accessor');
accessor.backing = 'updated getter value';
assert.eq(readCached(accessor), 'updated getter value');
assert.eq(getterCalls, 2, 'the getter runs on every read');
Object.defineProperty(accessor, 'cached', { configurable: true, writable: true, value: 'data again' });
assert.eq(readCached(accessor), 'data again', 'accessor changed back into a data property');

const prototype = { cached: 'original prototype' };
const child = Object.create(prototype);

child.backing = 'child value';
assert.eq(readCached(child), 'original prototype');
prototype.cached = 'updated prototype';
assert.eq(readCached(child), 'updated prototype', 'prototype value updates are visible');
child.cached = 'own value';
assert.eq(readCached(child), 'own value');
delete child.cached;
assert.eq(readCached(child), 'updated prototype', 'deleting an own property reveals the prototype');
Object.defineProperty(prototype, 'cached', {
    configurable: true,
    get() {
        assert.is(this, child, 'inherited getter receives the child');

        return this.backing;
    }
});
assert.eq(readCached(child), 'child value');
Object.setPrototypeOf(child, { cached: 'replacement prototype' });
assert.eq(readCached(child), 'replacement prototype', 'prototype replacement is visible');
Object.setPrototypeOf(child, null);
assert.eq(readCached(child), undefined);

let proxyCalls = 0;
const proxyTarget = { cached: 'target' };
const proxy = new Proxy(proxyTarget, {
    get(target, key, receiver) {
        assert.is(target, proxyTarget);
        assert.eq(key, 'cached');
        assert.is(receiver, proxy, 'proxy trap receives the proxy');
        proxyCalls++;

        return `trapped ${proxyCalls}`;
    }
});

assert.eq(readCached(proxyTarget), 'target');
assert.eq(readCached(proxy), 'trapped 1', 'own-property hint does not bypass a proxy trap');
assert.eq(readCached(proxy), 'trapped 2');
assert.eq(proxyCalls, 2);

const proxyChild = {};

Object.setPrototypeOf(proxyChild, new Proxy({}, {
    get(target, key, receiver) {
        assert.eq(key, 'cached');
        assert.is(receiver, proxyChild, 'proxy in the prototype chain receives the child');

        return 'prototype trap';
    }
}));
assert.eq(readCached(proxyChild), 'prototype trap');

const revocable = Proxy.revocable({ cached: 'revocable' }, {});

assert.eq(readCached(revocable.proxy), 'revocable');
revocable.revoke();
assert.throws(() => readCached(revocable.proxy), TypeError, 'revoked proxies still throw');

function cachedMethod() {
    return this.backing;
}

const methodFirst = { cachedMethod, backing: 'first receiver' };
const methodSecond = { backing: 'second receiver', cachedMethod };

for (let i = 0; i < 32; i++) {
    assert.eq(callCachedMethod(methodFirst), 'first receiver');
    assert.eq(callCachedMethod(methodSecond), 'second receiver', 'method calls preserve this across slots');
}

const methodChild = Object.create(methodFirst);

methodChild.backing = 'inherited receiver';
assert.eq(callCachedMethod(methodChild), 'inherited receiver', 'inherited method receives the child');
Object.defineProperty(methodSecond, 'cachedMethod', {
    configurable: true,
    get() {
        assert.is(this, methodSecond, 'method getter receives the object');

        return cachedMethod;
    }
});
assert.eq(callCachedMethod(methodSecond), 'second receiver', 'accessor method preserves the call receiver');
Object.defineProperty(methodSecond, 'cachedMethod', { configurable: true, value: 123 });
assert.throws(() => callCachedMethod(methodSecond), TypeError, 'cached method changed to a non-callable value');

const frozen = { cached: 'frozen value', cachedMethod, backing: 'frozen receiver' };

assert.eq(readCached(frozen), 'frozen value');
assert.eq(callCachedMethod(frozen), 'frozen receiver');
Object.freeze(frozen);
assert.eq(readCached(frozen), 'frozen value', 'reads remain valid after freezing');
assert.eq(callCachedMethod(frozen), 'frozen receiver');
assert.throws(() => {
    frozen.cached = 'changed';
}, TypeError, 'frozen property remains non-writable');
assert.throws(() => {
    delete frozen.cached;
}, TypeError, 'frozen property remains non-configurable');
assert.eq(readCached(frozen), 'frozen value');

function makeCycle(value) {
    const object = { cached: value };

    object.self = object;
    object.cachedMethod = () => readCached(object);
    assert.eq(readCached(object), value);
    assert.eq(callCachedMethod(object), value);

    return new WeakRef(object);
}

const cycleRefs = [];

for (let i = 0; i < 32; i++) {
    cycleRefs.push(makeCycle(i));
}

// Leave the job that created the WeakRefs before checking that hints own no roots.
await new Promise(resolve => setTimeout(resolve, 0));
globalThis.tjs.engine.gc.run();

for (const ref of cycleRefs) {
    assert.is(ref.deref(), undefined, 'cached reads do not retain unreachable object cycles');
}

for (let i = 0; i < 32; i++) {
    assert.eq(readCached({ before: i, cached: i + 1 }), i + 1, 'fresh shapes work after collection');
    assert.eq(readCached({}), undefined);
}
