#!/usr/bin/env bash
# run_oom_sweep.sh — M9 确定性分配失败注入（OOM sweep）
#
# 同一程序 tests/stress/oom_sweep.myp 以 MYP_FAIL_ALLOC=N（N=1..12）运行：
# 每次都应在第 N 次 runtime 分配处，以稳定诊断 abort（退出码非 0，stderr 含
# "injected allocation failure (allocation #N)"），且不得静默继续 / 崩溃在别处。
# 无 MYP_FAIL_ALLOC 时程序应正常完成（基线 sanity）。
#
# 用法: bash tests/stress/run_oom_sweep.sh
set -u
cd "$(dirname "$0")/../.."
MYPCC="${MYPCC:-./build/mypc}"
SRC="tests/stress/oom_sweep.myp"
BIN="/tmp/oom_sweep"

if ! "$MYPCC" -O2 "$SRC" -o "$BIN" >/tmp/oom_sweep.compile 2>&1; then
    echo "[oom_sweep] COMPILE FAIL"; cat /tmp/oom_sweep.compile; exit 1
fi

PASS=0; FAIL=0
# 基线：无注入应正常完成
if out=$(MYP_FAIL_ALLOC=0 "$BIN" 2>&1); rc=$?; [ $rc -eq 0 ] && echo "$out" | grep -q "^ok acc="; then
    PASS=$((PASS+1)); echo "  baseline          PASS"
else
    FAIL=$((FAIL+1)); echo "  baseline          FAIL (rc=$rc)"; echo "$out" | tail -3
fi

for n in $(seq 1 12); do
    out=$(MYP_FAIL_ALLOC=$n "$BIN" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  N=$n              FAIL: 未触发注入（程序正常退出）"; FAIL=$((FAIL+1)); continue
    fi
    if ! echo "$out" | grep -q "injected allocation failure (allocation #$n)"; then
        echo "  N=$n              FAIL: rc=$rc 但消息不符"; echo "$out" | tail -3; FAIL=$((FAIL+1)); continue
    fi
    PASS=$((PASS+1)); echo "  N=$n              PASS (allocation #$n aborted)"
done
echo "  oom_sweep: $PASS 通过, $FAIL 失败"
[ $FAIL -eq 0 ]
