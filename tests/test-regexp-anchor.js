import assert from 'tjs:assert';

assert.ok(/^foo/.test('fooX'), 'anchor matches at the beginning');
assert.eq(/^foo/.exec('Xfoo'), null, 'anchor cannot match a later start');
assert.eq(/^foo/.exec(''), null, 'nonempty anchored pattern fails on empty input');
assert.eq(/^/.exec('')[0], '', 'bare anchor matches empty input');
assert.eq(/^/.exec('abc').index, 0, 'bare anchor matches only the beginning');
assert.eq(/((^foo))/.exec('foo').slice(0), [ 'foo', 'foo', 'foo' ], 'unconditional captures before anchor');
assert.eq(/((^foo))/.exec('Xfoo'), null, 'captured anchor fails at later starts');
assert.eq(/^(a|(b))\2$/.exec('bb').slice(0), [ 'bb', 'b', 'b' ], 'anchored captures and backreference');
assert.eq(/^a(?=b)b/.exec('ab')[0], 'ab', 'lookahead after mandatory anchor');
assert.eq(/^a(?!b)c/.exec('ac')[0], 'ac', 'negative lookahead after mandatory anchor');
assert.eq(/(?<=a)^b/.exec('ab'), null, 'lookbehind before anchor retains its semantics');

assert.eq(/^foo|bar/.exec('xbar').index, 1, 'unanchored alternative must remain searchable');
assert.eq(/(^foo)|bar/.exec('xbar')[1], undefined, 'failed alternative resets captures');
assert.eq(/(^)?a/.exec('xa').index, 1, 'optional anchor does not constrain later matches');
assert.eq(/(?=^foo)foo/.exec('xfoo'), null, 'lookaround before anchor is preserved');
assert.eq(/^foo/m.exec('x\nfoo').index, 2, 'multiline anchor accepts newline');
assert.eq(/^foo/m.exec('x\rfoo').index, 2, 'multiline anchor accepts carriage return');
assert.eq(/^foo/m.exec('x\u2028foo').index, 2, 'multiline anchor accepts line separator');
assert.eq(/^foo/m.exec('x\u2029foo').index, 2, 'multiline anchor accepts paragraph separator');
assert.eq(/^foo/m.exec('xfoo'), null, 'multiline still requires a line start');

for (const flags of [ 'g', 'y', 'gy' ]) {
    const re = new RegExp('^foo', flags);

    re.lastIndex = 1;
    assert.eq(re.exec('xfoo'), null, `${flags} rejects nonzero anchored start`);
    assert.eq(re.lastIndex, 0, `${flags} resets lastIndex on failure`);
    assert.eq(re.exec('foo').index, 0, `${flags} can match again after failure`);
    assert.eq(re.lastIndex, 3, `${flags} records successful end`);
    assert.eq(re.exec('foo'), null, `${flags} cannot match after its previous end`);
    assert.eq(re.lastIndex, 0, `${flags} resets after the subsequent failure`);
}

for (const flags of [ 'u', 'v' ]) {
    const re = new RegExp('^(😀)', `${flags}g`);

    re.lastIndex = 1;
    const match = re.exec('😀x');

    assert.eq(match.index, 0, `${flags} normalizes a start inside a surrogate pair`);
    assert.eq(match[1], '😀', `${flags} preserves astral captures`);
    assert.eq(re.lastIndex, 2, `${flags} reports UTF-16 end index`);
    re.lastIndex = 2;
    assert.eq(re.exec('😀x'), null, `${flags} rejects a later complete code point`);
    assert.eq(new RegExp('^\\ud800', flags).exec('\ud800x')[0], '\ud800', `${flags} lone high surrogate`);
    assert.eq(new RegExp('^\\udc00', flags).exec('\udc00x')[0], '\udc00', `${flags} lone low surrogate`);
}

const indexed = /^(a)(b)?/d.exec('a');

assert.eq(indexed.indices.slice(0), [ [ 0, 1 ], [ 0, 1 ], undefined ], 'capture indices and absent optional capture');
assert.eq('xfoo'.search(/^foo/), -1, 'search rejects later anchor');
const search = /^foo/g;

search.lastIndex = 1;
assert.eq('foo'.search(search), 0, 'search starts at zero');
assert.eq(search.lastIndex, 1, 'search restores caller lastIndex');
assert.eq('foo'.match(/^foo/g), [ 'foo' ], 'global match at beginning');
assert.eq('xfoo'.match(/^foo/g), null, 'global match rejects later anchor');
assert.eq([ ...'foo\nfoo'.matchAll(/^foo/gm) ].map(match => match.index), [ 0, 4 ], 'multiline matchAll');
assert.eq('foo\nfoo'.match(/^foo/gm), [ 'foo', 'foo' ], 'multiline global match');

const seen = [];
const replaced = 'foo\nfoo'.replace(/^(foo)/gm, (match, group, offset) => {
    seen.push([ group, offset ]);

    return group.toUpperCase();
});

assert.eq(replaced, 'FOO\nFOO', 'replace callback across line starts');
assert.eq(seen, [ [ 'foo', 0 ], [ 'foo', 4 ] ], 'replace callback captures and offsets');
assert.eq('abc'.replace(/^/g, '_'), '_abc', 'zero-length global replacement');
assert.eq('abc'.split(/^/), [ 'abc' ], 'split handles zero-length start anchor');

const bytecode = globalThis.tjs.engine.compile(new TextEncoder().encode(
    'globalThis.__regexpAnchorRoundtrip = [/((^foo))/.exec("foo")[2], ' +
    '/^foo/.test("xfoo"), /^foo|bar/.exec("xbar").index];'
), 'regexp-anchor-roundtrip');

await globalThis.tjs.engine.evalBytecode(bytecode);
assert.eq(globalThis.__regexpAnchorRoundtrip, [ 'foo', false, 1 ], 'compiled regexp bytecode retains semantics');
delete globalThis.__regexpAnchorRoundtrip;
