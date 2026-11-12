// Run this with TJS_QUICKJS_JIT set to 0, baseline, and 1 on the same binary.
// Pass "constants" to measure the constant arithmetic paths.
function reduce(n, seed) {
    var result = seed;

    for (var i = 0; i < n; i++) {
        result += i * 3;
    }

    return result;
}

function reduceConstants(n, seed) {
    let result = seed;

    for (let i = 0; i < n; i++) {
        result += ((i << 3) / 8) % 128;
        result = result - -7;
    }

    return result;
}

const iterations = 512;
const calls = 40000;
const constants = globalThis.tjs.args[3] === 'constants';
const kernel = constants ? reduceConstants : reduce;

for (let i = 0; i < 2048; i++) {
    kernel(iterations, i & 255);
}

let checksum = 0;
const start = performance.now();

for (let i = 0; i < calls; i++) {
    checksum += kernel(iterations, i & 255);
}

const elapsedMs = performance.now() - start;

const expectedSeedSum = Math.floor(calls / 256) * 32640 + (calls % 256) * (calls % 256 - 1) / 2;
const expectedPerCall = constants
    ? iterations / 128 * (127 * 128 / 2) + 7 * iterations
    : 3 * iterations * (iterations - 1) / 2;
const expected = calls * expectedPerCall + expectedSeedSum;

if (checksum !== expected) {
    throw new Error(`unexpected checksum ${checksum}, expected ${expected}`);
}

console.log(JSON.stringify({ elapsedMs, calls, iterations, checksum }));
