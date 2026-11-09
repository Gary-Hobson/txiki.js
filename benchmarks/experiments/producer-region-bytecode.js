function check(x, message) { if (!x) throw Error(message); }
const source = `
function savedProducer(s,i) { var c; if ((c=s.charCodeAt(i)) > 64) return c; return -1; }
globalThis.savedProducer = savedProducer;
`;
console.log('PRODUCER_BYTECODE_COMPILE');
const bytecode = tjs.engine.compile(new TextEncoder().encode(source), 'producer-bytecode.js');
const first = tjs.engine.serialize(bytecode);
console.log('PRODUCER_BYTECODE_READ');
const restored = tjs.engine.deserialize(first);
const second = tjs.engine.serialize(restored);
check(first.length === second.length && first.every((v, i) => v === second[i]), 'canonical read/write');
await tjs.engine.evalBytecode(restored);
for (let i = 0; i < 80; i++) check(savedProducer('AZ', i & 1) === (i & 1 ? 90 : 65), 'read plan execution');
check(savedProducer(new String('AZ'), 0) === 65, 'read plan generic fallback');
console.log('producer bytecode compiler/reader correctness passed');
