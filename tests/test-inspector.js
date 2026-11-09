import assert from 'tjs:assert';
import path from 'tjs:path';


async function debuggerPort(proc) {
    const reader = proc.stderr.getReader();
    const decoder = new TextDecoder();
    let stderr = '';

    try {
        for (;;) {
            const { done, value } = await reader.read();

            if (done) {
                throw new Error(`Inspector did not start: ${stderr}`);
            }

            stderr += decoder.decode(value, { stream: true });

            const address = stderr.match(/ws:\/\/127\.0\.0\.1:(\d+)\/debug/);

            if (address) {
                return Number(address[1]);
            }
        }
    } finally {
        reader.releaseLock();
    }
}

async function connectDebugger(url) {
    const address = new URL(url);
    const socket = await tjs.connect('tcp', address.hostname, Number(address.port));
    const { readable, writable } = await socket.opened;
    const reader = readable.getReader();
    const writer = writable.getWriter();
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    let buffer = new Uint8Array();

    async function readMore() {
        const { value, done } = await reader.read();

        assert.ok(!done, 'debugger socket closed');
        buffer = new Uint8Array([ ...buffer, ...value ]);
    }

    async function readBytes(size) {
        while (buffer.length < size) {
            await readMore();
        }

        const data = buffer.slice(0, size);

        buffer = buffer.slice(size);

        return data;
    }

    await writer.write(encoder.encode([
        `GET ${address.pathname} HTTP/1.1`,
        `Host: ${address.host}`,
        'Upgrade: websocket',
        'Connection: Upgrade',
        'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==',
        'Sec-WebSocket-Version: 13',
        '',
        '',
    ].join('\r\n')));

    let header = '';

    while (!header.endsWith('\r\n\r\n')) {
        header += decoder.decode(await readBytes(1));
    }

    assert.ok(header.startsWith('HTTP/1.1 101'));

    return {
        async receive() {
            let message = '';

            for (;;) {
                const [ flags, size ] = await readBytes(2);
                const length = size === 126 ? new DataView((await readBytes(2)).buffer).getUint16(0) : size;

                assert.ok(size <= 126, 'test messages fit in the WebSocket chunk size');
                message += decoder.decode(await readBytes(length), { stream: !(flags & 0x80) });

                if (flags & 0x80) {
                    return JSON.parse(message);
                }
            }
        },
        async send(message) {
            const data = encoder.encode(JSON.stringify(message));
            const size = data.length < 126 ? [ data.length ] : [ 126, data.length >> 8, data.length & 255 ];
            const mask = crypto.getRandomValues(new Uint8Array(4));

            size[0] |= 0x80;
            const frame = new Uint8Array([ 0x81, ...size, ...mask, ...data.map((byte, i) => byte ^ mask[i % 4]) ]);

            await writer.write(frame);
        },
        close: () => socket.close(),
    };
}

async function inspect(dir) {
    const source = `export async function add(n) {
    let total = n + 2;
    { let total = 900; void total; }
    {
        let delta = 1;
        total += delta;
        total += 2;
    }
    debugger;
    return total;
}
`;

    await tjs.writeFile(path.join(dir, 'child script.js'), source);
    await tjs.writeFile(path.join(dir, 'main.js'), `
import { add } from './child script.js';
console.log(await add(40));
`);

    const proc = tjs.spawn(
        [ tjs.exePath, '--inspect-brk', '--inspect-port', '0', 'run', 'main.js' ],
        { cwd: dir, stdout: 'pipe', stderr: 'pipe' },
    );
    let finished = false;

    try {
        const port = await debuggerPort(proc);
        const response = await fetch(`http://127.0.0.1:${port}/json/list`);

        assert.eq(response.status, 200, 'HTTP discovery works');

        const [ target ] = await response.json();
        const debuggerClient = await connectDebugger(target.webSocketDebuggerUrl);
        let sequence = 0;
        const pending = [];

        async function take(matches) {
            for (;;) {
                const index = pending.findIndex(matches);

                if (index !== -1) {
                    return pending.splice(index, 1)[0];
                }

                pending.push(await debuggerClient.receive());
            }
        }

        async function send(method, params = {}) {
            const id = ++sequence;

            await debuggerClient.send({ id, method, params });

            const result = await take(message => message.id === id);

            assert.eq(result.error, undefined, method);

            return result.result;
        }

        const pause = async () => (await take(message => message.method === 'Debugger.paused')).params;

        await send('Runtime.enable');
        await send('Debugger.enable');

        const { breakpointId } = await send('Debugger.setBreakpointByUrl', {
            urlRegex: 'child%20script\\.js$',
            lineNumber: 5,
        });

        await send('Runtime.runIfWaitingForDebugger');
        const entry = await pause();

        assert.eq(entry.reason, 'Break on start');
        assert.ok(entry.callFrames[0].url.endsWith('/main.js'));

        await send('Debugger.resume');
        const atBreakpoint = await pause();
        const [ frame, caller ] = atBreakpoint.callFrames;

        assert.eq(atBreakpoint.hitBreakpoints[0], breakpointId);
        assert.eq(frame.functionName, 'add');
        assert.eq(frame.location.lineNumber, 5);
        assert.ok(caller.url.endsWith('/main.js'));

        const { scriptId } = frame.location;
        const { scriptSource } = await send('Debugger.getScriptSource', { scriptId });

        assert.eq(scriptSource, source);

        const { locations } = await send('Debugger.getPossibleBreakpoints', {
            start: { scriptId, lineNumber: 5 },
            end: { scriptId, lineNumber: 7 },
        });

        assert.eq(locations.map(location => location.lineNumber), [ 5, 6 ]);

        const { result: properties } = await send('Runtime.getProperties', { objectId: frame.scopeChain[0].object.objectId });

        assert.eq(properties.find(prop => prop.name === 'total').value.value, 42);
        assert.eq(properties.find(prop => prop.name === 'delta').value.value, 1);

        const { result } = await send('Debugger.evaluateOnCallFrame', {
            callFrameId: frame.callFrameId,
            expression: 'n + total + delta',
        });

        assert.eq(result.value, 83);

        await send('Debugger.setVariableValue', {
            callFrameId: frame.callFrameId,
            scopeNumber: 0,
            variableName: 'total',
            newValue: { value: 50 },
        });
        await send('Debugger.stepOver');
        const stepped = await pause();

        assert.eq(stepped.reason, 'step');
        assert.eq(stepped.callFrames[0].location.lineNumber, 6);

        await send('Debugger.resume');
        assert.eq((await pause()).reason, 'debugCommand');

        // A client that disconnects while paused must let the program finish.
        debuggerClient.close();
        const [ status, stdout ] = await Promise.all([ proc.wait(), proc.stdout.text() ]);

        finished = true;
        assert.eq(status.exit_status, 0);
        assert.eq(stdout.trim(), '53');
    } finally {
        if (!finished) {
            proc.kill('SIGTERM');
            await proc.wait();
        }
    }
}

if (tjs.engine.features.inspector) {
    const dir = await tjs.makeTempDir(path.join(tjs.tmpDir, 'tjs-inspector-XXXXXX'));

    try {
        await inspect(dir);
    } finally {
        await tjs.remove(dir);
    }
}
