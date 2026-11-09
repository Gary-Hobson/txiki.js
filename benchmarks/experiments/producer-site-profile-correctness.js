function assert(v) { if (!v) throw Error('producer diagnostic fixture'); }
function direct(s, i) { return s.charCodeAt(i) === 97; }
function local(s, i) { var c = s.charCodeAt(i); return c < 110; }
function multi(s, i) { var c = s.charCodeAt(i); var a = c + 1; return a * 2 < 250; }
function nested(s, i) { return Math.abs(s.charCodeAt(i)); }
const original = String.prototype.charCodeAt;
const proxy = new Proxy(original, {});
const boxed = new String('abc');
// Five snapshots: expected direct eligible increments 20,20,20,20, then zero.
tjs.engine.gc.run();
for (let i = 0; i < 20; i++) assert(direct('abc', 0));
tjs.engine.gc.run();
for (let i = 0; i < 20; i++) assert(local('abc', 0));
tjs.engine.gc.run();
for (let i = 0; i < 20; i++) assert(multi('abc', 0));
tjs.engine.gc.run();
for (let i = 0; i < 20; i++) assert(nested('abc', 0) === 97);
tjs.engine.gc.run();
for (let i = 0; i < 20; i++) {
    assert(original.call('abc', 0) === 97);
    assert(proxy.call('abc', 0) === 97);
    assert(boxed.charCodeAt(0) === 97);
}
tjs.engine.gc.run();
console.log('producer site diagnostic correctness passed');
