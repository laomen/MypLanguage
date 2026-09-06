#!/usr/bin/env node
// C7（roadmap §C7）：LSP 协议级覆盖——URI 隔离 / didOpen-didClose 生命周期 /
// 同版本重复 didChange / UTF-8 多字节 / 空·语法错·超大文件 / 高频交替不串位 /
// 非法 JSON-RPC 不崩。
//
// 驱动模型同 test_lsp.js：一次性编码全部 JSON-RPC 帧喂给 myp_lsp，按 id 收集
// 响应并断言。合法消息断言具体值；异常输入断言"进程存活 + shutdown 正常"。

const { spawnSync } = require("child_process");
const lsp = process.env.MYP_LSP || "./build/myp_lsp";

const base = [
    "class Alpha {",
    "    action:",
    "        @startup void start(int value) {}",
    "    function:",
    "        int twice(int v) { return v * 2; }",
    "    property:",
    "        int count;",
    "}",
].join("\n");
const mk = (name) => base.replace("Alpha", name);
const gamma = mk("Gamma"), beta = mk("Beta"), delta = mk("Delta");
const utf8src = "// 中文注释 multi-byte 文本\nclass U8 { function: int f(int v) { return v + 1; } }";
const emptySrc = "";
const badSrc = "class Broken { action: int x( { return 1; }\n";
const bigSrc = "// " + "padpadpadpad".repeat(20000) + "\n" + base; // ~180KB 注释垫

const A = "file:///tmp/myp_lsp_proto_a.myp";
const B = "file:///tmp/myp_lsp_proto_b.myp";
const U = "file:///tmp/myp_lsp_proto_utf8.myp";
const E = "file:///tmp/myp_lsp_proto_empty.myp";
const X = "file:///tmp/myp_lsp_proto_bad.myp";
const F = "file:///tmp/myp_lsp_proto_big.myp";

const frames = [];
function send(m) {
    const body = JSON.stringify(m);
    frames.push(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}
function req(id, method, params) { send({ jsonrpc: "2.0", id, method, params }); }
function nfy(method, params) { send({ jsonrpc: "2.0", method, params }); }
function hover(id, u, line, ch) { req(id, "textDocument/hover", { textDocument: { uri: u }, position: { line, character: ch } }); }
function syms(id, u) { req(id, "textDocument/documentSymbol", { textDocument: { uri: u } }); }
function comp(id, u) { req(id, "textDocument/completion", { textDocument: { uri: u }, position: { line: 0, character: 1 } }); }
function open(u, text) { nfy("textDocument/didOpen", { textDocument: { uri: u, text } }); }
function change(u, text) { nfy("textDocument/didChange", { textDocument: { uri: u }, contentChanges: [{ text }] }); }
function close(u) { nfy("textDocument/didClose", { textDocument: { uri: u } }); }

// ---- 场景编排 ----
send({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} });
open(A, base); open(B, gamma);                 // 两 URI 各自缓存
hover(10, A, 0, 7);                            // Alpha
hover(11, B, 0, 7);                            // Gamma
change(A, beta);                               // 改 A → Beta
hover(12, A, 0, 7);                            // Beta（A 失效重建）
hover(13, B, 0, 7);                            // 仍 Gamma（B 不受 A 污染 → URI 隔离）
change(A, beta);                               // 同版本重复 didChange（同文本）
hover(14, A, 0, 7);                            // 仍 Beta、不崩
close(A);                                      // didClose 清 A 缓存
open(A, delta);                                // 重开 A → Delta
hover(15, A, 0, 7);                            // Delta（close 清 + reopen 重建）
open(U, utf8src);                              // UTF-8 多字节内容
hover(16, U, 0, 6);                            // 中文注释行（position 在多字节后）
hover(17, U, 1, 7);                            // class U8 行
syms(18, U);
open(E, emptySrc); syms(19, E);                // 空文件
open(X, badSrc);  syms(20, X);                 // 语法错误文件
open(F, bigSrc);  hover(21, F, 1, 7);          // 超大文件（~180KB）
for (let i = 0; i < 5; i++) {                  // 高频交替：hover/completion/syms 交叠
    hover(30 + i * 3, A, 0, 7);
    comp(31 + i * 3, A);
    syms(32 + i * 3, A);
}
send({ id: 50, method: "textDocument/hover", params: { textDocument: { uri: A }, position: { line: 0, character: 7 } } }); // 缺 jsonrpc
send({ jsonrpc: "2.0", id: 51, method: "bogus/method", params: {} });          // 未知方法
send("this is not json at all");                                              // 非法帧
send({ jsonrpc: "2.0", id: 52, method: "shutdown", params: {} });

const result = spawnSync(lsp, [], { input: frames.join(""), maxBuffer: 16 * 1024 * 1024 });
const responses = new Map();
let offset = 0;
const sep = Buffer.from("\r\n\r\n");
const stdout = result.stdout || Buffer.alloc(0);
while (offset < stdout.length) {
    const he = stdout.indexOf(sep, offset);
    if (he < 0) break;
    const header = stdout.subarray(offset, he).toString();
    const m = header.match(/Content-Length:\s*(\d+)/i);
    if (!m) break;
    const bs = he + 4, len = Number(m[1]);
    const body = stdout.subarray(bs, bs + len).toString();
    offset = bs + len;
    let msg; try { msg = JSON.parse(body); } catch { continue; }
    if (msg.id !== undefined) responses.set(Number(msg.id), msg);
}

const hv = (id) => responses.get(id)?.result?.contents?.value;
const symNames = (id) => (responses.get(id)?.result || []).map((s) => s.name);
const compLabels = (id) => (responses.get(id)?.result?.items || []).map((c) => c.label);

let failures = 0;
function check(cond, label) {
    if (!cond) { console.error("FAIL: " + label); failures++; }
}
function checkHover(id, expect, label) {
    const got = hv(id);
    if (got !== expect) { console.error(`FAIL: ${label}: expected ${JSON.stringify(expect)}, got ${JSON.stringify(got)}`); failures++; }
}
const sig = (name) => `class ${name}\n---\nactions: 1, events: 0, properties: 1`;

// 1) 基础 + URI 隔离 + 缓存失效
checkHover(10, sig("Alpha"), "A initial");
checkHover(11, sig("Gamma"), "B initial");
checkHover(12, sig("Beta"), "A after didChange → Beta");
checkHover(13, sig("Gamma"), "B unaffected by A change (URI 隔离)");
// 2) 同版本重复 didChange 不崩、值不变
checkHover(14, sig("Beta"), "A repeated didChange");
// 3) didClose 后重开 → 缓存重建
checkHover(15, sig("Delta"), "A close+reopen → Delta");
// 4) UTF-8 多字节：position 在多字节后仍有正确响应（不崩、有内容）
check(responses.has(16) && responses.get(16).result != null, "UTF-8 hover line0 response");
check(responses.has(17) && responses.get(17).result != null, "UTF-8 hover line1 response");
check(Array.isArray(responses.get(18)?.result), "UTF-8 documentSymbol array");
// 5) 空 / 语法错误文件：不崩、符号为空
check(Array.isArray(responses.get(19)?.result) && responses.get(19).result.length === 0, "empty file syms=[]");
check(Array.isArray(responses.get(20)?.result), "syntax-error file syms array (server tolerant)");
// 6) 超大文件不崩、内容正确
checkHover(21, sig("Alpha"), "big file hover");
// 7) 高频交替：每响应 id 结果正确、不串位
for (let i = 0; i < 5; i++) {
    checkHover(30 + i * 3, sig("Delta"), `alt hover #${i}`);
    check(compLabels(31 + i * 3).includes("Delta"), `alt completion #${i} has Delta`);
    check(symNames(32 + i * 3).includes("Delta"), `alt syms #${i} has Delta`);
}
// 8) 非法 JSON-RPC：进程不崩（status 0）、缺 jsonrpc 被容忍、未知方法返回协议错误、shutdown 正常
check(result.status === 0, "process survived malformed frames (status 0)");
check(hv(50) === sig("Delta"), "request missing jsonrpc field still handled");
check(responses.get(51)?.error?.code === -32601, "unknown method → -32601 Method not found");
check(responses.get(52)?.result === null, "shutdown → null");

if (failures > 0) process.exit(1);
console.log(`myp-lsp-proto PASS=30 FAIL=0`);
