import assert from 'tjs:assert';


const encoder = new TextEncoder();
const decoder = new TextDecoder();
const ascii = '0123456789abcdef';
const mixed = `${ascii}\0é🦊${ascii}`;

assert.eq(decoder.decode(encoder.encode(mixed)), mixed);

const invalid = new Uint8Array([ ...encoder.encode(ascii), 0xff, ...encoder.encode(ascii) ]);

assert.eq(decoder.decode(invalid), `${ascii}\uFFFD${ascii}`);
assert.throws(() => new TextDecoder('utf-8', { fatal: true }).decode(invalid), TypeError);

const firstChunk = new Uint8Array([ ...encoder.encode(ascii), 0xc3 ]);
const secondChunk = new Uint8Array([ 0xa9, ...encoder.encode(ascii) ]);

assert.eq(decoder.decode(firstChunk, { stream: true }), ascii);
assert.eq(decoder.decode(secondChunk), `é${ascii}`);
