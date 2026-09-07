#!/usr/bin/env bash
# install_myp_deb.sh — 把 myp-lang .deb 装到 Debian/Ubuntu 服务器（含 LLVM 依赖）
#
# mypc（自举 MYP 编译器）编译用户程序时 shell out 到外部 opt/llc/ld.lld：
#   · Ubuntu 26.04+ 原生有 llvm-21/lld-21
#   · Ubuntu 22.04/24.04 默认源无 llvm-21 → 走 apt.llvm.org <codename>-21
# 本脚本：探测 → 必要时加源/装 LLVM → 装 .deb → 端到端验证（mypc -O2 编译+运行）。
#
# 用法（root 或 sudo）：
#   sudo bash scripts/install_myp_deb.sh [path/to/myp-lang_*.deb] [--no-llvm]
# 默认 .deb = build/myp-lang_*.deb（仓库内）；--no-llvm = 跳过 LLVM 依赖安装
# （已在目标机装好 opt-21/llc-21/ld.lld-21 时用）。
# 退出码：0 = 装好且验证通过；非 0 = 失败。
set -euo pipefail

DEB="${1:-}"
NO_LLVM=0
for a in "$@"; do
    [ "$a" = "--no-llvm" ] && NO_LLVM=1
done

REPO="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "$DEB" ]; then
    DEB=$(ls "$REPO"/build/myp-lang_*.deb 2>/dev/null | head -1 || true)
fi
if [ -z "$DEB" ] || [ ! -f "$DEB" ]; then
    echo "FAIL: 找不到 .deb（传参或 $REPO/build/myp-lang_*.deb）" >&2; exit 2
fi
DEB="$(readlink -f "$DEB")"

if [ "$(id -u)" -ne 0 ]; then
    echo "FAIL: 需要 root（sudo bash scripts/install_myp_deb.sh …）" >&2; exit 2
fi

# ---- OS 探测 ----
. /etc/os-release 2>/dev/null || { echo "FAIL: 无法读 /etc/os-release" >&2; exit 2; }
case "${ID:-}${ID_LIKE:-}" in
    *debian*|*ubuntu*) : ;;
    *) echo "FAIL: 仅支持 Debian/Ubuntu（当前 ID=$ID ID_LIKE=${ID_LIKE:-}）" >&2; exit 2 ;;
esac
echo "== 目标机: ${PRETTY_NAME:-$ID} $(uname -m) =="

# ---- LLVM 依赖（opt/llc/lld）----
have21() {
    command -v opt-21 >/dev/null 2>&1 && command -v llc-21 >/dev/null 2>&1 \
        && command -v ld.lld-21 >/dev/null 2>&1
}
if [ "$NO_LLVM" = "1" ]; then
    echo "== 跳过 LLVM 安装（--no-llvm）=="
elif have21; then
    echo "== opt-21/llc-21/ld.lld-21 已就位，跳过 =="
else
    echo "== 缺 LLVM 工具 → 安装 llvm-21 + lld-21 =="
    export DEBIAN_FRONTEND=noninteractive
    if apt-cache policy llvm-21 2>/dev/null | grep -qE 'Candidate: [0-9]'; then
        apt-get update -qq
        apt-get install -y llvm-21 lld-21
    else
        # 默认源没有 → apt.llvm.org <codename>-21
        CODENAME="${VERSION_CODENAME:-}"
        [ -z "$CODENAME" ] && CODENAME=$(echo "$VERSION_ID" | awk '{print "jammy"}')
        echo "== 默认源无 llvm-21 → 加 apt.llvm.org $CODENAME-21 =="
        KEYRING=/usr/share/keyrings/llvm-snapshot.gpg
        wget -q https://apt.llvm.org/llvm-snapshot.gpg.key -O /tmp/llvm-snapshot.gpg.key
        gpg --batch --yes --dearmor -o "$KEYRING" /tmp/llvm-snapshot.gpg.key
        rm -f /tmp/llvm-snapshot.gpg.key
        echo "deb [signed-by=$KEYRING] http://apt.llvm.org/$CODENAME/ llvm-toolchain-$CODENAME-21 main" \
            > /etc/apt/sources.list.d/llvm-21.list
        apt-get update -qq
        apt-get install -y llvm-21 lld-21
    fi
    for t in opt-21 llc-21 ld.lld-21; do
        command -v "$t" >/dev/null 2>&1 || { echo "FAIL: 安装后仍缺 $t" >&2; exit 1; }
    done
fi

# ---- 安装 .deb ----
echo "== 安装 $DEB =="
export DEBIAN_FRONTEND=noninteractive
apt-get install -y "$DEB"

# ---- 端到端验证 ----
echo "== 验证 =="
mypc --version 2>&1 | head -1
VDIR=$(mktemp -d)
trap 'rm -rf "$VDIR"' EXIT
printf 'import env;\nclass B { action: @constructor B() { Console.writeLine("myp-deb-ok"); } }\nint main() { B b = new B(); return 0; }\n' \
    > "$VDIR/hello.myp"
( cd "$VDIR" && mypc -O2 hello.myp -o hello >/dev/null 2>&1 && ./hello ) | grep -q 'myp-deb-ok' \
    || { echo "FAIL: mypc 编译/运行验证未通过" >&2; exit 1; }
echo "== 安装成功，端到端验证通过（myp-deb-ok）=="
