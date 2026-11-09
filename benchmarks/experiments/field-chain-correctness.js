const assert = (v, message) => { if (!v) throw Error(message); };
function field(x) { return x.a.b.c; }
function local(x) { var value = x; return value.a.b.c; }
for (let i = 0; i < 10000; i++) {
    const x = {a: {b: {c: i}}};
    assert(field(x) === i && local(x) === i, 'plain chain');
}
const cycle = {};
cycle.a = cycle; cycle.b = cycle; cycle.c = cycle;
assert(field(cycle) === cycle, 'cycle stays rooted');
let calls = 0;
const original = {a: {b: {get c() { calls++; original.a = null; return 17; }}}};
assert(field(original) === 17 && calls === 1, 'stop and own receiver before getter');
const middle = {a: {get b() { calls++; middle.a = null; return {c: 23}; }}};
assert(field(middle) === 23 && calls === 2, 'middle getter runs once');
const keys = [];
const proxied = {a: new Proxy({b: {c: 29}}, {get(target, key) { keys.push(key); return target[key]; }})};
assert(field(proxied) === 29 && keys.join(',') === 'b', 'proxy is observed');
const inherited = {a: Object.create({b: {c: 31}})};
assert(field(inherited) === 31, 'prototype fallback');
Object.defineProperty(inherited.a, 'b', {value: {c: 37}, configurable: true});
assert(field(inherited) === 37, 'own field shadows prototype');
delete inherited.a.b;
assert(field(inherited) === 31, 'deleted own field falls back');
const frozen = Object.freeze({a: Object.freeze({b: Object.freeze({c: 41})})});
assert(field(frozen) === 41, 'non-writable data properties');
let failed = 0;
for (const x of [null, {a: null}, {a: {b: null}}]) {
    try { field(x); } catch (e) { if (e instanceof TypeError) failed++; else throw e; }
}
assert(failed === 3, 'exceptions at each chain depth');
const value = {token: 43};
assert(field({a: {b: {c: value}}}) === value, 'owned final result');
function* resumed(x) { yield 1; return x.a.b.c; }
const g = resumed({a: {b: {c: 47}}});
assert(g.next().value === 1 && g.next().value === 47, 'resumed generator');
let head = {value: 53};
for (let i = 0; i < 1024; i++) head = {next: head};
const long = Function('x', 'return x' + '.next'.repeat(1024) + '.value');
assert(long(head) === 53, 'long plain own chain');
console.log('field chain correctness passed');
