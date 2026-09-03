#!/bin/bash
# 两阶段并行回归：1) 编译 P=4（-o 唯一名，控 LLVM 内存）；2) 运行 P=6（无编译内存）。
# 判据：编译错→COMPILE FAIL；运行含 FAIL/onnx load failed/DBG fail→RUNTIME FAIL。
cd "$(dirname "$0")/../../.."
rm -f /tmp/rt_*.res /tmp/rt_*
tests=$(ls examples/deeplearning/infer_tests/*_main.myp | sed 's/.*\///;s/_main.myp//')
compile_one() {
  t="$1"
  # 并发 mypc 偶发瞬时失败（LLVM 资源/竞态）→ 重试兜底
  try=0
  while [ $try -lt 3 ]; do
    if ./build/mypc "examples/deeplearning/infer_tests/${t}_main.myp" -o "/tmp/rt_$t" --stdlib stdlib --package-path examples/deeplearning >/dev/null 2>&1; then
      echo C > "/tmp/rt_$t.res"; return
    fi
    try=$((try+1)); sleep 1
  done
  echo "COMPILE FAIL: $t"; echo F > "/tmp/rt_$t.res"
}
export -f compile_one
echo "$tests" | xargs -P 4 -I{} bash -c 'compile_one "$@"' _ {}
run_one() {
  t="$1"
  [ "$(cat /tmp/rt_$t.res)" = "F" ] && return
  out=$(cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 "/tmp/rt_$t" 2>&1)
  if echo "$out" | grep -qE 'FAIL|fail [a-z]|onnx load failed|DBG fail'; then
    echo "RUNTIME FAIL: $t"
    echo "$out" | grep -E 'FAIL|fail|onnx' | head -2
    echo F > "/tmp/rt_$t.res"
  else
    echo P > "/tmp/rt_$t.res"
  fi
}
export -f run_one
echo "$tests" | xargs -P 6 -I{} bash -c 'run_one "$@"' _ {}
pass=$(grep -l '^P' /tmp/rt_*.res 2>/dev/null | wc -l)
fail=$(grep -l '^F' /tmp/rt_*.res 2>/dev/null | wc -l)
echo "== pass=$pass fail=$fail =="
