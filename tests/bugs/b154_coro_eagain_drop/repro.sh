#!/usr/bin/env bash
# tests/bugs/b154_coro_eagain_drop/repro.sh — BUG-154 回归门禁（自包含，纯 bash）
# EAGAIN-as-EOF：coro reactor poll 就绪后 recv EAGAIN 被当 EOF → 高并发丢连接。
# 每轮 N 路并发 GET /，全部必须返回 200；任一失败（连不上/EOF/超时/非 200）→ 回归。
# 修复前：~20-33% 丢（24 路并发实测）；修复后全 200。
# 用法: bash tests/bugs/b154_coro_eagain_drop/repro.sh [port] [rounds] [conc]
# 退出码：0 = 全 200（已修复） 1 = 出现丢包（回归） 2 = 构建/依赖缺失
set -uo pipefail
cd "$(dirname "$0")"
PORT="${1:-8814}"
ROUNDS="${2:-20}"
CONC="${3:-24}"
MYPC=../../../build/mypc
STDLIB=../../../stdlib
SRV=/tmp/b154_self_srv
[ -x "$MYPC" ] || { echo "缺 mypc: $MYPC（先构建 MYPLanguage）"; exit 2; }
if [ ! -x "$SRV" ]; then
  echo "== 编译自包含最小 coro 反应堆 =="
  "$MYPC" -O2 --stdlib "$STDLIB" -o "$SRV" srv.myp || { echo "编译失败"; exit 2; }
fi
B154_PORT="$PORT" "$SRV" >/tmp/b154_self.log 2>&1 &
WP=$!
trap 'kill -9 $WP 2>/dev/null' EXIT
for i in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:${PORT}/" && break
  sleep 0.2
done

fail=0
total=$((ROUNDS * CONC))
for i in $(seq 1 "$ROUNDS"); do
  # 开 CONC 个并发 curl，各判 200
  declare -a pids=()
  for j in $(seq 1 "$CONC"); do
    ( c=$(timeout 3 curl -s --max-time 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT}/" 2>/dev/null); [ "$c" = 200 ] ) &
    pids+=($!)
  done
  for p in "${pids[@]}"; do
    if ! wait "$p"; then fail=$((fail + 1)); fi
  done
done
if [ "$fail" -gt 0 ]; then
  echo "BUG-154 REGRESSION: $fail/$total 请求失败（EAGAIN-as-EOF 丢包复现）"
  exit 1
fi
echo "OK: ${ROUNDS}×${CONC}=${total} 并发请求全 200（EAGAIN-as-EOF 已修复）"
exit 0
