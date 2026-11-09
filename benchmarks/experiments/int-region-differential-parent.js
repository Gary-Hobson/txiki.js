// Differential output is compared byte-for-byte against the retained baseline.
let seed = 1627;
function next(n) {
    seed = Math.imul(seed, 1664525) + 1013904223 | 0;
    return (seed >>> 0) % n;
}
const ops = ['+', '-', '*', '^', '&', '|', '<<', '>>', '>>>'];
const terms = ['a', 'b', 'x', 'y', '0', '1', '-1', '31', '2147483647', '-2147483648'];
const inputs = [
    [3, 5], [0, 0], [-1, 1], [-31, 8], [2147483647, 1], [-2147483648, -1],
    [0.5, 1], [-0, 0], [NaN, 1], [Infinity, -Infinity], ['3', 2],
    [true, false], [null, undefined], [1n, 2n], [Symbol('x'), 1]
];
const out = [];
function encode(f, x, y) {
    try {
        const r = f(x, y);
        return typeof r + ':' + (Object.is(r, -0) ? '-0' : String(r));
    } catch (e) {
        return 'throw:' + e.name;
    }
}
for (let trial = 0; trial < 96; trial++) {
    let body = 'var a=x+y;var b=a*3;';
    for (let j = 0; j < 6; j++) {
        const lhs = j % 2 ? 'a' : 'b';
        const left = terms[next(terms.length)];
        const right = terms[next(terms.length)];
        body += lhs + '=' + left + ops[next(ops.length)] + '(' + right + ');';
    }
    body += trial % 3 === 0 ? 'return a<b;' : trial % 3 === 1 ? 'return a^b;' : 'return a+b;';
    for (const [x, y] of inputs) {
        // A fresh function prevents a previous fallback from hiding later bugs.
        const f = Function('x', 'y', body);
        out.push(encode(f, 3, 5), encode(f, x, y), encode(f, -7, 11));
    }
}
console.log(JSON.stringify(out));
