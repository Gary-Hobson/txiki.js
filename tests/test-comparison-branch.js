import assert from 'tjs:assert';

const operators = [ '<', '<=', '>', '>=', '==', '!=', '===', '!==' ];
const values = [
    0, 1, -1, NaN, Infinity, -0, 1n, 2n, '1', 'x', Symbol('x'), null,
    undefined, true,
    { valueOf() {
        return 1;
    } },
    { valueOf() {
        throw new Error('coercion');
    } }
];

function outcome(fn, a, b) {
    try {
        return [ 'value', fn(a, b) ];
    } catch (error) {
        return [ 'throw', error.name, error.message ];
    }
}

// Force the compiler to also emit branches with a four-byte displacement.
const padding = Array.from({ length: 200 }, (_, i) => `s += ${i};`).join('\n');

for (const operator of operators) {
    const reference = new Function('a', 'b', `return a ${operator} b;`);

    for (const longBranch of [ false, true ]) {
        for (const invert of [ false, true ]) {
            const branch = new Function('a', 'b', `
                let s = 0;
                if (${invert ? '!' : ''}(a ${operator} b)) {
                    ${longBranch ? padding : ''}
                    return true;
                }
                return false;
            `);

            for (const a of values) {
                for (const b of values) {
                    const expected = outcome(reference, a, b);

                    if (invert && expected[0] === 'value') {
                        expected[1] = !expected[1];
                    }

                    assert.deepEqual(outcome(branch, a, b), expected,
                        `${operator}, long=${longBranch}, invert=${invert}`);
                }
            }
        }
    }
}

let count = 0;

while (count < 10000) {
    count++;
}

assert.equal(count, 10000);
