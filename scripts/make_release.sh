#!/usr/bin/env bash
# make_release.sh — 组装 release/ 分发目录（.deb + 安装脚本 + README + sha256）
#
# 用法：bash scripts/make_release.sh
# 产出（gitignored，可再生）：
#   release/myp-lang_<ver>_amd64.deb
#   release/install_myp_deb.sh
#   release/README.md
#   release/SHA256SUMS
# .deb 缺失时自动 `cpack -G DEB` 重打。
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

# 找/打 .deb
DEB=$(ls build/myp-lang_*.deb 2>/dev/null | head -1 || true)
if [ -z "$DEB" ]; then
    echo "== 无 .deb，cpack -G DEB 重打 =="
    (cd build && cpack -G DEB >/dev/null)
    DEB=$(ls build/myp-lang_*.deb | head -1)
fi
DEB="$(readlink -f "$DEB")"
VER=$(basename "$DEB" | sed -E 's/^myp-lang_([^_]+)_.*/\1/')
echo "== deb: $DEB (v$VER) =="

mkdir -p release
cp "$DEB" release/
cp scripts/install_myp_deb.sh release/install_myp_deb.sh
chmod +x release/install_myp_deb.sh

cat > release/README.md <<EOF
# MYP Language — release $VER

自举编译器工具链 Debian/Ubuntu 包。

## 内容
- \`myp-lang_${VER}_amd64.deb\` — 用户侧工具链（mypc + myp_lsp/fmt/viz/pm/debug
  + stdlib + MYP runtime 归档 + pass 插件 + /usr wrapper）
- \`install_myp_deb.sh\` — Debian/Ubuntu 一键安装（自动补 opt-21/llc-21/ld.lld-21）

## 安装（目标机，root）
    sudo bash install_myp_deb.sh myp-lang_${VER}_amd64.deb
- 仅 .deb 也可（需先装 llvm-21/lld-21）：
    sudo dpkg -i myp-lang_${VER}_amd64.deb   # 缺依赖则: sudo apt-get install -f

## 验证
    mypc --version
    echo 'import env; class B { action: @constructor B() { Console.writeLine("hi"); } }
int main() { B b = new B(); return 0; }' > hi.myp
    mypc -O2 hi.myp -o hi && ./hi        # -> hi
EOF

( cd release && sha256sum myp-lang_${VER}_amd64.deb install_myp_deb.sh > SHA256SUMS )

echo "== release/ 内容 =="
ls -la release/
cat release/SHA256SUMS
