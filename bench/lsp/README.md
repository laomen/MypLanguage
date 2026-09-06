# bench/lsp — LSP 性能基准（roadmap §5.3）

5k 类文件的 hover/completion 吞吐 + **区分「计算」与「写出」时间**。

## 前置

`build/myp_lsp` 已构建（含 `MYP_LSP_TIMING` 钩子，见下）：

```bash
cmake --build build --target myp_lsp
```

## 用法

```bash
bash bench/lsp/run.sh [nclass] [nhover]   # 默认 5000 类 / 120 hover
```

驱动 `driver.js`（node，交互式 JSON-RPC over stdio）：
1. `initialize`
2. `didOpen`（合成 5k 类文件）→ `[lsp-timing] didOpen` = 解析+索引 ms
3. `hover` ×120（散布全文件的类名/方法名，**真串行**逐请求计时）→ handler ms
   + 真实往返 ms
4. `didChange`（同文本，失效 completion cache、不重解析）→ `completion` cold 1
   次（构建 5k 类 → 1.5 万 item + 排序去重 + JSON 序列化）
5. `completion` ×30 warm（cache 命中 = 纯序列化+写出）

退出码 0 = 全请求成功（校验 hover 有结果、cold completion 含尾部类）。

## 如何区分「计算」vs「写出」

- **服务端 handler ms**：`src/lsp/lsp_server.cpp` 加了 env 门控钩子
  `MYP_LSP_TIMING=1` → 每个已处理消息把耗时打到 **stderr**（`[lsp-timing] <method> <ms>`，
  与 stdout 的 JSON-RPC 协议隔离，不影响协议）。driver 启动 myp_lsp 时带该 env，
  解析 stderr 得每请求 handler 总耗时（= 计算 + 序列化 + 写）。
- **计算**：cold completion handler ms − warm(cache 命中) ms ≈ 构造/查找/排序成本
  （warm 命中只做 `sendResponse(cache)` = 纯 JSON 序列化 + 写出，无任何查找）。
- **写出**：warm(cache 命中) ms ≈ 纯序列化+写出成本（本机 762KB 响应 ≈ 1.2 ms）。

## 本机参考（2026-09-07，5k 类）

```
index  didOpen(parse+索引)          ~135 ms
hover  handler 平均(纯计算)          ~0.88 ms/req  (~1130 req/s)
hover  往返平均(计算+写出+管道)      ~1.16 ms/req  (~860 req/s)
comp   cold(构造+排序+序列化+写)     ~49 ms  items=15048  resp=762330 B
comp   warm(cache 命中=纯序列化写)   ~1.2 ms/req   (~820 req/s)
comp   计算(≈cold−warm) ≈ 48 ms；写出(≈warm) ≈ 1.2 ms / 762KB
```

读法：didOpen 索引是单次打开的主成本（~135ms）；hover 计算极快（0.88ms，
微响应无写出压力）；completion 冷路径 49ms 里 ~48ms 是构造/排序/序列化 1.5 万条
item 的计算，写出仅 ~1.2ms——优化方向是冷 completion 的 items 构造+排序（而非
IO）。
