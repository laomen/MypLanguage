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

function completion(id) {
    send({
        jsonrpc: "2.0",
        id,
        method: "textDocument/completion",
        params: { textDocument: { uri }, position: { line: 0, character: 0 } },
    });
}

function documentSymbols(id) {
    send({
        jsonrpc: "2.0",
        id,
        method: "textDocument/documentSymbol",
        params: { textDocument: { uri } },
    });
}

send({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} });
send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri, text: source } },
});
completion(2);
completion(3);
hover(4, source, 0, "Alpha");
hover(5, source, 2, "start");
hover(6, source, 4, "changed");
hover(7, source, 6, "count");
hover(8, source, 8, "twice");
hover(9, source, 10, "Color");
hover(10, source, 11, "helper");
documentSymbols(13);
documentSymbols(14);
send({
    jsonrpc: "2.0",
    method: "textDocument/didChange",
    params: { textDocument: { uri }, contentChanges: [{ text: changedSource }] },
});
hover(15, changedSource, 0, "Beta");
completion(16);
documentSymbols(17);
send({ jsonrpc: "2.0", id: 18, method: "shutdown", params: {} });

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
    [4, "class Alpha\n---\nactions: 1, events: 1, properties: 1"],
    [5, "Alpha.start(int) → void [@startup]"],
    [6, "Alpha.changed(int) → event"],
    [7, "Alpha.count : int"],
    [8, "Alpha::twice(int) → int"],
    [9, "enum Color { Red, Blue }"],
    [10, "function helper(int) → int"],
    [15, "class Beta\n---\nactions: 1, events: 1, properties: 1"],
]);

let failures = 0;
for (const [id, text] of expected) {
    const actual = responses.get(id)?.result?.contents?.value;
    if (actual !== text) {
        console.error(`hover ${id}: expected ${JSON.stringify(text)}, got ${JSON.stringify(actual)}`);
        failures++;
    }
}

const initialCompletion = responses.get(2)?.result;
const cachedCompletion = responses.get(3)?.result;
const changedCompletion = responses.get(16)?.result;
const initialLabels = initialCompletion?.items?.map((item) => item.label) || [];
const changedLabels = changedCompletion?.items?.map((item) => item.label) || [];
if (JSON.stringify(cachedCompletion) !== JSON.stringify(initialCompletion)) {
    console.error("completion cache changed an identical response");
    failures++;
}
if (!initialLabels.includes("Alpha") || initialLabels.includes("Beta")) {
    console.error("initial completion does not contain the expected class");
    failures++;
}
if (!changedLabels.includes("Beta") || changedLabels.includes("Alpha")) {
    console.error("changed completion was not invalidated");
    failures++;
}

const initialSymbols = responses.get(13)?.result;
const cachedSymbols = responses.get(14)?.result;
const changedSymbols = responses.get(17)?.result;
const initialSymbolNames = initialSymbols?.map((symbol) => symbol.name) || [];
const changedSymbolNames = changedSymbols?.map((symbol) => symbol.name) || [];
if (JSON.stringify(cachedSymbols) !== JSON.stringify(initialSymbols)) {
    console.error("document symbol cache changed an identical response");
    failures++;
}
if (!initialSymbolNames.includes("Alpha") || initialSymbolNames.includes("Beta")) {
    console.error("initial document symbols do not contain the expected class");
    failures++;
}
if (!changedSymbolNames.includes("Beta") || changedSymbolNames.includes("Alpha")) {
    console.error("changed document symbols were not invalidated");
    failures++;
}

if (failures > 0) process.exit(1);
console.log(`myp-lsp PASS=${expected.size + 6} FAIL=0`);
