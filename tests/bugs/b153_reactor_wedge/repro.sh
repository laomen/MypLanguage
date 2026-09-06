#!/usr/bin/env bash
# tests/bugs/b153_reactor_wedge/repro.sh — BUG-153 自包含复现驱动（纯 coro 反应堆，无 fork）
# 每轮：2 路同一瞬间并发 GET / + 1 路跟进单发；任一非 200 → 判楔（acceptor 不再响应，
#   进程存活=非崩溃）。实测 1-2 轮即楔（3/3）。
# 用法: bash tests/bugs/b153_reactor_wedge/repro.sh [port] [rounds]
# 退出码：0=复现（楔死） 3=未复现（全 200）
set -uo pipefail
cd "$(dirname "$0")"
PORT="${1:-8803}"
ROUNDS="${2:-40}"
MYPC=../../../build/mypc
STDLIB=../../../stdlib
SRV=/tmp/b153_self_srv
[ -x "$MYPC" ] || { echo "缺 mypc: $MYPC（先构建 MYPLanguage）"; exit 2; }
if [ ! -x "$SRV" ]; then
  echo "== 编译自包含最小 coro 反应堆 =="
  "$MYPC" -O2 --stdlib "$STDLIB" -o "$SRV" srv.myp || { echo "编译失败"; exit 2; }
fi
B153_PORT="$PORT" "$SRV" >/tmp/b153_self.log 2>&1 &
WP=$!
trap 'kill -9 $WP 2>/dev/null' EXIT
for i in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:${PORT}/" && break
  sleep 0.2
done

one() {
  local c; c=$(timeout 3 curl -s --max-time 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT}/")
  [ "$c" = 200 ]
}
echo "== 2 路同一瞬间并发 GET / × $ROUNDS 轮 =="
hit=0
for i in $(seq 1 "$ROUNDS"); do
  one & a=$!; one & b=$!
  wait $a; ra=$?; wait $b; rb=$?
  one; rf=$?
  if [ $ra -ne 0 ] || [ $rb -ne 0 ] || [ $rf -ne 0 ]; then
    echo "WEDGE round=$i  并发1=$([ $ra -eq 0 ] && echo 200 || echo X) 并发2=$([ $rb -eq 0 ] && echo 200 || echo X) 跟进单发=$([ $rf -eq 0 ] && echo 200 || echo X)  server-alive=$(kill -0 $WP 2>/dev/null && echo yes || echo NO)"
    hit=1
    break
  fi
done
if [ "$hit" = 0 ]; then echo "NO-WEDGE: ${ROUNDS} 轮全 200（未复现）"; exit 3; fi
echo "复现成功（进程存活=非崩溃，acceptor 不再被 poll → BUG-153）"
exit 0
