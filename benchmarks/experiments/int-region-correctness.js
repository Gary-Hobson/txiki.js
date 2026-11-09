const assert = (v, m) => { if (!v) throw Error(m); };
const same = (a, b, m) => assert(Object.is(a, b), `${m}: ${a} != ${b}`);

// Repeated reads and writes require distinct local value versions.
function dag(x, y) {
    var a = x + y;
    var b = a * 3;
    a = b - x;
    b = a ^ y;
    return b + ((a << 1) & 255);
}
for (let i = 0; i < 2000; i++) {
    const x = i & 31, y = (i + 7) & 31;
    const a = (x + y) * 3 - x;
    same(dag(x, y), (a ^ y) + ((a << 1) & 255), 'DAG');
}
function repeated(x, y) {
    var a = (x + y) * (x + y);
    a = a + x;
    var b = a - y;
    return b > x;
}
for (let i = 0; i < 100; i++) {
    same(repeated(i, 2), ((i + 2) * (i + 2) + i - 2) > i, 'repeat/cmp');
    same(typeof repeated(i, 2), 'boolean', 'comparison result tag');
}

function overflow(x, y) {
    var a = x + 1;
    var b = a + y;
    a = b * 2;
    return a - y;
}
same(overflow(5, 7), 19, 'warm overflow');
same(overflow(2147483647, 4), 4294967300, 'overflow whole fallback');
same(overflow(0.5, 0.25), 3.25, 'float fallback');
same(overflow('3', 2), 622, 'string concatenation fallback');

function unsigned(x, y) {
    var a = x + y;
    var b = a >>> 0;
    return b + y;
}
same(unsigned(1, 2), 5, 'shift warm');
same(unsigned(-3, 1), 4294967295, 'unsigned overflow');
function zero(x, y) {
    var a = x + y;
    var b = a * -1;
    return b * 1;
}
same(zero(3, 1), -4, 'mul warm');
same(zero(0, 0), -0, 'negative zero');
function negate(x, y) {
    var a = x + y;
    var b = -a;
    return b * 1;
}
same(negate(3, 1), -4, 'neg warm');
same(negate(0, 0), -0, 'neg zero');
same(negate(-2147483648, 0), 2147483648, 'neg min int');

// A late guard failure must not pre-commit either assignment. The original
// sequence invokes conversion once, assigns a, then throws while computing b.
let coercions = 0;
function transaction(x, y) {
    var a = 10, b = 20;
    try {
        a = x + 1;
        b = y + 2;
        a = a + b;
        return [a, b, 'ok'];
    } catch (e) {
        return [a, b, e.message];
    }
}
same(JSON.stringify(transaction(2, 3)), '[8,5,"ok"]', 'transaction warm');
const tx = transaction({valueOf() { coercions++; return 7; }},
                       {valueOf() { coercions++; throw Error('late'); }});
same(JSON.stringify(tx), '[8,20,"late"]', 'late conversion ordering');
same(coercions, 2, 'no duplicate conversion');
function oldObject(x, y) {
    var a = {keep: true};
    a = x + 1;
    var b = a + y;
    a = b * 2;
    return a + y;
}
same(oldObject(2, 3), 15, 'old reference write guard');

// Branches into the middle of a potential region must cut it, and a terminal
// comparison must preserve the original conditional-branch poll.
function branches(n) {
    var sum = 0;
    for (var i = 0; i < n; i++) {
        var a = i + 1;
        var b = a + 2;
        if (b * 2 < n + 9) sum = sum + b;
        else sum = sum - b;
    }
    return sum;
}
let expected = 0;
for (let i = 0; i < 100; i++) {
    const b = i + 3;
    expected += b * 2 < 109 ? b : -b;
}
same(branches(100), expected, 'control-flow boundaries');

// These environments are conservatively excluded from planning.
function captured(x) { var a = x; const read = () => a; a = a + 1; var b = a + 2; return read() + b; }
same(captured(2), 8, 'captured');
const mapped = Function('x', 'var a = x + 1; arguments[0] = 7; var b = x + 2; return a + b;');
same(mapped(2), 12, 'mapped arguments');
function directEval(x) { var a = x + 1; eval('a = 8'); var b = a + 2; return a + b; }
same(directEval(2), 18, 'direct eval');
function* generator(x) { var a = x + 1; var b = a + 2; yield a + b; a = b + 3; return a; }
const g = generator(2);
same(g.next().value, 8, 'generator initial');
same(g.next().value, 8, 'generator resume');
async function asynchronous(x) { var a = x + 1; await 0; var b = a + 2; return a + b; }
same(await asynchronous(2), 8, 'async');
let tdz = false;
try { { const z = a + 1; let a = 1; void z; } } catch (e) { tdz = e instanceof ReferenceError; }
assert(tdz, 'TDZ');

// Candidate can coexist with object literal private opcodes in one function.
function mixed(x, y) {
    var a = x + 1;
    var b = a * 2;
    const obj = {a, b};
    var c = x + y;
    var d = c * 3;
    return {obj, value: d + y};
}
same(JSON.stringify(mixed(2, 3)), '{"obj":{"a":3,"b":6},"value":18}', 'object-plan coexistence');
const source = `globalThis.irRoundTrip=function(x,y){var a=x+y;var b=a*3;a=b-x;b=a^y;return b+((a<<1)&255);};`;
const compiled = tjs.engine.compile(new TextEncoder().encode(source), 'int-region-roundtrip.js');
const encoded = tjs.engine.serialize(compiled);
const twice = tjs.engine.serialize(compiled);
assert(encoded.length === twice.length && encoded.every((x, i) => x === twice[i]), 'canonical bytes stable');
const loaded = tjs.engine.deserialize(encoded);
// The module API executes the expression, while native fixtures cover retained
// global compile-only handles and byte-for-byte canonical identity.
await tjs.engine.evalBytecode(loaded);
same(globalThis.irRoundTrip(2, 3), 40, 'loaded integer region');
tjs.engine.gc.run();
console.log('int region correctness passed');
