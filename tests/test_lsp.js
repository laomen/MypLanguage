#!/usr/bin/env node

const { spawnSync } = require("child_process");

const lsp = process.env.MYP_LSP || "./build/myp_lsp";
const uri = "file:///tmp/myp_lsp_hover_test.myp";
const source = [
    "class Alpha {",
    "    action:",
    "        @startup void start(int value) {}",
    "    event:",
    "        changed(int value);",
    "    property:",
    "        int count;",
    "    function:",
    "        int twice(int value) { return value * 2; }",
    "}",
    "enum Color { Red; Blue; }",
    "int helper(int value) { return value; }",
].join("\n");
const changedSource = source.replace("Alpha", "Beta");
const frames = [];

function send(message) {
    const body = JSON.stringify(message);
    frames.push(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

function hover(id, text, line, word) {
    send({
        jsonrpc: "2.0",
        id,
        method: "textDocument/hover",
        params: {
            textDocument: { uri },
            position: { line, character: text.split("\n")[line].lastIndexOf(word) + 1 },
        },
    });
}

send({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} });
send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri, text: source } },
});
hover(2, source, 0, "Alpha");
hover(3, source, 2, "start");
hover(4, source, 4, "changed");
hover(5, source, 6, "count");
hover(6, source, 8, "twice");
hover(7, source, 10, "Color");
hover(8, source, 11, "helper");
send({
    jsonrpc: "2.0",
    method: "textDocument/didChange",
    params: { textDocument: { uri }, contentChanges: [{ text: changedSource }] },
});
hover(9, changedSource, 0, "Beta");
send({ jsonrpc: "2.0", id: 10, method: "shutdown", params: {} });

const result = spawnSync(lsp, [], {
    input: frames.join(""),
    maxBuffer: 4 * 1024 * 1024,
});
if (result.status !== 0) {
    process.stderr.write(result.stderr?.toString() || `myp_lsp exited with ${result.status}\n`);
    process.exit(1);
}

const responses = new Map();
let offset = 0;
const separator = Buffer.from("\r\n\r\n");
while (offset < result.stdout.length) {
    const headerEnd = result.stdout.indexOf(separator, offset);
    if (headerEnd < 0) break;
    const header = result.stdout.subarray(offset, headerEnd).toString();
    const lengthMatch = header.match(/Content-Length:\s*(\d+)/i);
    if (!lengthMatch) break;
    const bodyStart = headerEnd + 4;
    const length = Number(lengthMatch[1]);
    const body = result.stdout.subarray(bodyStart, bodyStart + length).toString();
    offset = bodyStart + length;
    const message = JSON.parse(body);
    if (message.id !== undefined) responses.set(Number(message.id), message);
}

const expected = new Map([
    [2, "class Alpha\n---\nactions: 1, events: 1, properties: 1"],
    [3, "Alpha.start(int) → void [@startup]"],
    [4, "Alpha.changed(int) → event"],
    [5, "Alpha.count : int"],
    [6, "Alpha::twice(int) → int"],
    [7, "enum Color { Red, Blue }"],
    [8, "function helper(int) → int"],
    [9, "class Beta\n---\nactions: 1, events: 1, properties: 1"],
]);

let failures = 0;
for (const [id, text] of expected) {
    const actual = responses.get(id)?.result?.contents?.value;
    if (actual !== text) {
        console.error(`hover ${id}: expected ${JSON.stringify(text)}, got ${JSON.stringify(actual)}`);
        failures++;
    }
}

if (failures > 0) process.exit(1);
console.log(`myp-lsp PASS=${expected.size} FAIL=0`);
