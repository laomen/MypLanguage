#!/usr/bin/env node
// bench/lsp/driver.js — LSP 性能基准（roadmap 5.3）：5k 类 hover/completion 吞吐
// + 区分「计算」(handler ms，服务端 stderr MYP_LSP_TIMING) 与「写出」(响应字节/
// 往返-计算)。
//
// 流程（单个 myp_lsp 进程，交互式逐请求计时）：
//   1) initialize
//   2) didOpen(合成的 5k 类文件) → [lsp-timing] didOpen = 解析+索引 ms
//   3) hover ×N（散布全文件不同类/方法）→ 逐请求 handler ms + 往返 ms → 吞吐
//   4) didChange(同文本, 失效 completion cache, 不重解析) → completion cold 1 次
//      （构建 5k 类 item 列表 + 排序去重 + JSON 序列化）→ 记 handler ms + 响应字节
//   5) completion ×M warm（cache 命中）→ cache 吞吐
// 输出: 一段可读报告 + `lsp-perf: PASS`（全请求成功）→ run.sh 据此判绿。
//
// 用法: MYP_LSP=./build/myp_lsp node driver.js [nclass] [nhover]
const { spawn } = require("child_process");
const fs = require("fs");

const lsp = process.env.MYP_LSP || "./build/myp_lsp";
const NCLASS = Number(process.argv[2] || 5000);
const NHOVER = Number(process.argv[3] || 120);
const uri = "file:///tmp/myp_lsp_bench_5k.myp";

// ---- 合成 5k 类源（结构固定，driver 可反查行号）----
function buildSource(n) {
  const lines = [];
  const classLine = [];
  const methodLine = [];
  lines.push("// bench/lsp synthetic: " + n + " classes");
  for (let i = 0; i < n; i++) {
    const id = String(i).padStart(4, "0");
    classLine.push(lines.length);
    lines.push("class C" + id + " {");
    lines.push("    action:");
    methodLine.push(lines.length);
    lines.push("        int m" + id + "(int x) { return x + " + i + "; }");
    lines.push("    property:");
    lines.push("        int v" + id + ";");
    lines.push("}");
    lines.push("");
  }
  return { lines, classLine, methodLine };
}
const src = buildSource(NCLASS);
const sourceText = src.lines.join("\n");

function encodeFrame(obj) {
  const b = Buffer.from(JSON.stringify(obj));
  return Buffer.concat([Buffer.from(`Content-Length: ${b.length}\r\n\r\n`), b]);
}

const timingLines = []; // [lsp-timing] 行（含消息序号）
const timingMsgs = [];  // [{method, ms}]

function run() {
  return new Promise((resolve, reject) => {
    const child = spawn(lsp, [], {
      env: Object.assign({}, process.env, { MYP_LSP_TIMING: "1" }),
    });
    let outBuf = Buffer.alloc(0);
    let errBuf = "";
    let respWait = new Map();
    let reqSeq = 0; // 已发送消息计数（含 notification）
    let timingSeq = 0; // stderr timing 计数
    let sawShutdown = false;

    child.on("error", reject);
    child.stderr.on("data", (d) => {
      errBuf += d.toString();
      let idx;
      while ((idx = errBuf.indexOf("\n")) >= 0) {
        const line = errBuf.slice(0, idx).trim();
        errBuf = errBuf.slice(idx + 1);
        const m = line.match(/^\[lsp-timing\] (\S+) ([\d.]+) ms$/);
        if (m) {
          timingMsgs.push({ method: m[1], ms: Number(m[2]) });
          timingSeq++;
        }
      }
    });
    child.stdout.on("data", (d) => {
      outBuf = Buffer.concat([outBuf, d]);
      drain();
    });
    child.on("close", (code) => {
      if (!sawShutdown) reject(new Error("myp_lsp exited early code=" + code + " stderr=" + errBuf.slice(-500)));
    });

    function drain() {
      for (;;) {
        const sep = outBuf.indexOf("\r\n\r\n");
        if (sep < 0) break;
        const hdr = outBuf.subarray(0, sep).toString();
        const m = hdr.match(/Content-Length:\s*(\d+)/i);
        if (!m) break;
        const len = Number(m[1]);
        const bodyStart = sep + 4;
        if (outBuf.length < bodyStart + len) break;
        const body = JSON.parse(outBuf.subarray(bodyStart, bodyStart + len).toString());
        outBuf = outBuf.subarray(bodyStart + len);
        if (body.id !== undefined) {
          const w = respWait.get(String(body.id));
          if (w) {
            respWait.delete(String(body.id));
            w(body);
          }
        }
      }
    }

    function sendRaw(obj) {
      return new Promise((res) => {
        const id = obj.id;
        if (id !== undefined) respWait.set(String(id), res);
        child.stdin.write(encodeFrame(obj));
      });
    }

    (async () => {
      // helper: 发送请求并计往返 ms
      async function req(method, params, payloadSizeOut) {
        const id = reqSeq++;
        const t0 = process.hrtime.bigint();
        const resp = await sendRaw({ jsonrpc: "2.0", id, method, params });
        const rttMs = Number(process.hrtime.bigint() - t0) / 1e6;
        return { resp, rttMs };
      }
      // 通知（无响应；仍计 reqSeq 以对齐 stderr timing）
      function notify(method, params) {
        reqSeq++;
        child.stdin.write(encodeFrame({ jsonrpc: "2.0", method, params }));
      }

      // 1) initialize（响应 = reqSeq 0）
      await sendRaw({ jsonrpc: "2.0", id: 0, method: "initialize", params: {} });

      // 2) didOpen（notification, reqSeq 1）→ 之后首个请求用于对齐
      notify("textDocument/didOpen", {
        textDocument: { uri, text: sourceText },
      });

      // 3) hover 基准：散布全文件的类名/方法名；真串行（写完即等）取真实往返延迟
      const hoverRes = [];
      const step = Math.max(1, Math.floor(NCLASS / NHOVER));
      for (let k = 0; k < NHOVER; k++) {
        const ci = Math.min(NCLASS - 1, k * step);
        const useMethod = (k % 2) === 1; // 交替 类 hover / 方法 hover
        const line = useMethod ? src.methodLine[ci] : src.classLine[ci];
        const word = useMethod ? "m" + String(ci).padStart(4, "0")
                               : "C" + String(ci).padStart(4, "0");
        const character = src.lines[line].indexOf(word) + word.length;
        hoverRes.push(await req("textDocument/hover", {
          textDocument: { uri },
          position: { line, character },
        }));
      }

      // 4) completion cold：didChange(同文本) 失效 cache → 1 次 cold
      notify("textDocument/didChange", {
        textDocument: { uri },
        contentChanges: [{ text: sourceText }],
      });
      // 等 server 处理完 didChange：发一个空 hover 对齐（reqSeq 推进）
      await req("textDocument/hover", {
        textDocument: { uri },
        position: { line: 0, character: 6 },
      });
      const coldReq = await req("textDocument/completion", {
        textDocument: { uri },
        position: { line: 0, character: 0 },
      });

      // 5) completion warm（cache 命中）× M（串行）
      const M = 30;
      const warmRes = [];
      for (let i = 0; i < M; i++) {
        warmRes.push(await req("textDocument/completion", {
          textDocument: { uri },
          position: { line: 0, character: 0 },
        }));
      }

      // shutdown
      reqSeq++;
      await sendRaw({ jsonrpc: "2.0", id: reqSeq++, method: "shutdown", params: {} });
      sawShutdown = true;
      child.stdin.end();

      // ---- 汇总 ----
      // timing 对齐：第 i 条 stderr timing 对应该进程第 i 个被处理的消息
      // （initialize=0, didOpen=1, 后续 hover/completion 顺序处理）。
      const didOpenMs = timingMsgs[1] ? timingMsgs[1].ms : -1;

      // hover handler ms：从 timingMsgs 里取 textDocument/hover 的第 2.. 条
      // （第 1 条 hover 与 didOpen 后首个请求；这里简单取全部 hover 的 handler ms
      //  并排除首个——首个含 didOpen 对齐）
      const allHoverTimings = timingMsgs.filter((t) => t.method === "textDocument/hover").map((t) => t.ms);
      const hoverTimings = allHoverTimings.length > 1 ? allHoverTimings.slice(1, 1 + NHOVER) : allHoverTimings;
      const hoverSum = hoverTimings.reduce((a, b) => a + b, 0);
      const hoverHandlerAvg = hoverSum / Math.max(1, hoverTimings.length);
      const hoverRttSum = hoverRes.reduce((a, r) => a + r.rttMs, 0);
      const hoverRttAvg = hoverRttSum / Math.max(1, hoverRes.length);

      // completion：cold = 最后一个非 cache 的 completion（其 handler ms 最大；
      //   简化：取 timingMsgs 里 textDocument/completion 的第 1 条 = cold）
      const compTimings = timingMsgs.filter((t) => t.method === "textDocument/completion").map((t) => t.ms);
      const coldMs = compTimings.length > 0 ? compTimings[0] : -1;
      const warmMsArr = compTimings.slice(1);
      const warmSum = warmMsArr.reduce((a, b) => a + b, 0);
      const warmAvg = warmMsArr.length ? warmSum / warmMsArr.length : -1;

      const coldBody = coldReq.resp.result || {};
      const coldBytes = Buffer.byteLength(JSON.stringify(coldReq.resp));
      const coldItems = (coldReq.resp.result && coldReq.resp.result.items) ? coldReq.resp.result.items.length : -1;

      // 校验：全部成功 + cold completion 含预期的类
      let pass = 1;
      const sampleItems = (coldReq.resp.result && coldReq.resp.result.items) || [];
      const lastClass = "C" + String(NCLASS - 1).padStart(4, "0");
      if (!sampleItems.some((it) => it.label === lastClass)) {
        process.stderr.write("cold completion 缺尾部类 " + lastClass + "\n");
        pass = 0;
      }
      for (const r of hoverRes) {
        if (!r.resp.result || !r.resp.result.contents) {
          process.stderr.write("hover 无结果\n");
          pass = 0;
          break;
        }
      }

      console.log("== bench/lsp（roadmap 5.3）：" + NCLASS + " 类 / " + NHOVER + " hover ==");
      console.log("index  didOpen(parse+索引)         " + didOpenMs.toFixed(3) + " ms");
      console.log("hover  handler 平均(纯计算)         " + hoverHandlerAvg.toFixed(4) + " ms/req  "
        + (hoverTimings.length ? (1000 / Math.max(hoverHandlerAvg, 1e-6)).toFixed(0) : 0) + " req/s");
      console.log("hover  往返平均(计算+写出+管道)     " + hoverRttAvg.toFixed(4) + " ms/req  "
        + (1000 / Math.max(hoverRttAvg, 1e-6)).toFixed(0) + " req/s");
      console.log("comp   cold(构造+排序+序列化+写)    " + coldMs.toFixed(3) + " ms  items=" + coldItems
        + "  resp=" + coldBytes + " B");
      console.log("comp   warm(cache 命中=纯序列化写)  " + (warmAvg >= 0 ? warmAvg.toFixed(4) : "n/a") + " ms/req  "
        + (warmAvg > 0 ? (1000 / warmAvg).toFixed(0) : 0) + " req/s  (写 762KB 级响应)");
      if (coldMs > 0 && warmAvg > 0) {
        console.log("comp   计算(≈cold−warm) ≈ " + (coldMs - warmAvg).toFixed(3) + " ms；"
          + "写出(≈warm cache 命中) ≈ " + warmAvg.toFixed(3) + " ms / " + coldBytes + " B");
      }
      console.log("注: cold completion 是 5k 类 → 1.5 万条 item、762KB 响应的重写出；",
        "warm cache 命中 = 纯 JSON 序列化+写出（无构造/查找）→ 作为「写出」基线");
      console.log(pass ? "lsp-perf: PASS" : "lsp-perf: FAIL");
      resolve(pass ? 0 : 1);
    })().catch((e) => {
      process.stderr.write("driver error: " + e.message + "\n" + e.stack + "\n");
      resolve(2);
    });
  });
}

run().then((code) => process.exit(code));
