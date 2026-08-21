# tests/lib/portable.sh — MYP 测试套件跨平台移植层（Linux / Git Bash / MSYS2 / Cygwin）
#
# 被各测试脚本 source（source 本文件后即可使用以下符号）。目标：
#   - Linux 上行为与 GNU coreutils 完全一致（timeout/ulimit 原样透传）
#   - Windows Git Bash/MSYS2 上提供等价物，避免脚本因缺 GNU timeout/ulimit 而失败
#
# 提供的符号：
#   MYP_IS_WINDOWS    1 = 正在 Windows 的 bash 环境（MINGW/MSYS/CYGWIN）
#   myp_timeout SECS CMD...     便携版 timeout（Linux=GNU timeout；Windows=纯 bash
#                               后台 + 轮询 + 超时强杀，退出码 124 表示超时）
#   myp_resolve_bin PATH        Windows 下若 PATH 不存在而 PATH.exe 存在则返回后者；
#                               否则原样返回（mypc 在 Windows 仍按 -o 精确命名 .out，
#                               msys 运行时按 PE 魔数可直接执行无扩展名文件）
#   myp_guard_ulimit MB         Linux 下限制虚拟内存（防 OOM，如自编译 .ll 很大）；
#                               Windows 静默跳过
#
# 注意：本文件只用 POSIX sh + bash 内建，不依赖 GNU 专属工具（timeout/ulimit）。

# ---- 平台检测 ----
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) MYP_IS_WINDOWS=1 ;;
    *) MYP_IS_WINDOWS=0 ;;
esac

# ---- 便携版 timeout ----
# Linux：直接调 GNU timeout（支持 --kill-after 等全语义）。
# Windows：无 GNU timeout —— 后台运行 + 每秒轮询，超时 kill -9，返回 124。
#   注：kill/wait 均为 bash 内建，Git Bash 下对自身派生的 Win32 进程有效；
#       子进程（如 mypc 派生的 gcc）不会被连带强杀——与 GNU timeout 行为接近即可。
myp_timeout() {
    local secs="$1"; shift
    if [ "$MYP_IS_WINDOWS" = "0" ]; then
        command timeout "$secs" "$@"
        return $?
    fi
    "$@" &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        waited=$((waited + 1))
        if [ "$waited" -ge "$secs" ]; then
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
    done
    wait "$pid"
    return $?
}

# ---- 便携版可执行文件解析 ----
# mypc 在 Windows 仍按 -o 精确命名（.out，无 .exe）；msys 运行时按 PE 魔数可直接
# 执行。此函数仅作前向兼容：若将来产出 .exe 变体则优先使用。
myp_resolve_bin() {
    if [ -f "$1" ]; then
        printf '%s\n' "$1"
    elif [ "$MYP_IS_WINDOWS" = "1" ] && [ -f "$1.exe" ]; then
        printf '%s\n' "$1.exe"
    else
        printf '%s\n' "$1"
    fi
}

# ---- 便携版内存上限 ----
myp_guard_ulimit() {  # myp_guard_ulimit 8388608（MB）
    if [ "$MYP_IS_WINDOWS" = "1" ]; then
        return 0
    fi
    if command -v ulimit >/dev/null 2>&1 && ulimit -v >/dev/null 2>&1; then
        ulimit -v "$1" 2>/dev/null || true
    fi
    return 0
}
