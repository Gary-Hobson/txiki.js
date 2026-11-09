function check(x, message) { if (!x) throw Error(message); }
function none(s, i) { var c; if ((c = s.charCodeAt(i)) > 64) return c; return -1; }
function bool(s, i) { var c; return (c = s.charCodeAt(i)) <= 64 || c > 100; }
function code(s, i) {
    var c;
    switch (c = s.charCodeAt(i)) {
        case 32: return c + 1;
        case 9: return c + 2;
        default: return c;
    }
}
function oldLocal(s, i, old) { var c = old; if ((c = s.charCodeAt(i)) > 64) return c; return -1; }
function caught(s, i) {
    var c;
    try { if ((c = s.charCodeAt(i)) > 64) return c; return -1; }
    catch (e) { return e.message; }
    finally { globalThis.producerFinally = (globalThis.producerFinally || 0) + 1; }
}
for (let j = 0; j < 80; j++) {
    check(none(' Aaz\uD800\uDC00', 1) === 65, 'none stack');
    check(none(' Aaz', 0) === -1, 'none false');
    check(bool(' Aaz', j % 4) === [true, false, false, true][j % 4], 'boolean keep');
    check(code(' \tAz', j % 4) === [33, 11, 65, 122][j % 4], 'code keep');
    check(oldLocal('Az', 0, {}) === 65, 'post-call local object guard');
    check(oldLocal('Az', 0, Symbol('old')) === 65, 'post-call local symbol guard');
    check(oldLocal('Az', 0, 'old') === 65, 'post-call local string guard');
}
check(none(new String('AZ'), 0) === 65, 'boxed receiver fallback');
check(none('AZ', 1.5) === 90, 'floating index fallback');
check(none('AZ', -1) === -1 && none('AZ', 99) === -1, 'bounds fallback');
check(none('AZ', 0) === 65, 'valid domain after fallback');
let coerced = 0;
check(none('AZ', { valueOf() { coerced++; return 1; } }) === 90 && coerced === 1, 'one coercion');
check(caught('AZ', { valueOf() { throw Error('index throw'); } }) === 'index throw', 'coercion exception');
check(caught('AZ', Symbol()) .includes('symbol'), 'symbol index exception');

const original = String.prototype.charCodeAt;
function replaced(s, i) { var c; if ((c = s.charCodeAt(i)) > 64) return c; return -1; }
let calls = 0;
String.prototype.charCodeAt = function(i) { calls++; return 77; };
check(replaced('AZ', 0) === 77 && calls === 1, 'replacement retires entry');
String.prototype.charCodeAt = original;
check(replaced('AZ', 0) === 65, 'retired canonical entry reusable');
let getterCalls = 0;
Object.defineProperty(String.prototype, 'charCodeAt', { configurable: true,
    get() { getterCalls++; return original; } });
check(none('AZ', 0) === 65 && getterCalls === 1, 'lookup getter remains observable');
Object.defineProperty(String.prototype, 'charCodeAt', { configurable: true, writable: true, value: original });
const proxy = new Proxy(original, { apply(target, self, args) { calls++; return Reflect.apply(target, self, args); } });
check(none({ charCodeAt: proxy, toString() { return 'AZ'; } }, 0) === 65, 'proxy generic call');
check(none({ charCodeAt: original.bind('AZ') }, 1) === 90, 'bound generic call');

function captured(s) { var c; function read() { return c; } if ((c = s.charCodeAt(0)) > 64) return read(); return -1; }
function dynamic(s) { var c; eval('c = 1'); if ((c = s.charCodeAt(0)) > 64) return c; return -1; }
function mapped(s, i) { var c; arguments[1] = 1; if ((c = s.charCodeAt(i)) > 64) return c; return -1; }
function* generator(s) { var c; yield 1; if ((c = s.charCodeAt(0)) > 64) return c; return -1; }
async function asyncFn(s) { var c; await 1; if ((c = s.charCodeAt(0)) > 64) return c; return -1; }
check(captured('AZ') === 65 && dynamic('AZ') === 65 && mapped('AZ', 0) === 65, 'excluded functions');
const it = generator('AZ'); check(it.next().value === 1 && it.next().value === 65, 'generator continuation');
check(await asyncFn('AZ') === 65, 'async continuation');
const rope = 'A'.repeat(20000) + 'Z'.repeat(20000);
original.call(rope, 0);
check(none(rope, 20000) === 90, 'cached rope view');
check(none('\uD800\uDC00', 0) === 0xD800 && none('\uD800\uDC00', 1) === 0xDC00, 'UTF16 units');
console.log('producer region JS correctness passed');
