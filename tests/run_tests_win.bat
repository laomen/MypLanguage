@echo off
rem ============================================================
rem  MYP 测试套件 Windows 启动器（依赖 Git Bash / MSYS2）
rem
rem  用法:
rem    cmd> tests\run_tests_win.bat [--rebuild] [--update]
rem    或直接双击运行（弹出窗口显示进度，结束自动关闭前显示退出码）
rem
rem  原理:
rem    测试脚本本身是 bash 脚本（tests/run_tests.sh 及子脚本），已在
rem    tests/lib/portable.sh 移植层支持下兼容 Windows Git Bash
rem    （GNU timeout/ulimit 均有等价物）。本启动器只负责找到 bash.exe
rem    并以仓库根为工作目录调用 run_tests.sh。
rem
rem  前提:
rem    1) 已安装 Git for Windows（https://git-scm.com/download/win）
rem       或 MSYS2（https://www.msys2.org/）
rem    2) 已用 Windows LLVM 构建 mypc.exe 于 build\ 下
rem    3) MinGW-w64 的 gcc 在 PATH（mypc 生成程序需要）
rem ============================================================
setlocal

set "ROOT=%~dp0.."
set "BASH="

rem ---- 1) 在常见安装位置找 bash.exe ----
for %%B in (
    "%ProgramFiles%\Git\bin\bash.exe"
    "%ProgramFiles%\Git\usr\bin\bash.exe"
    "%ProgramFiles(x86)%\Git\bin\bash.exe"
    "%LocalAppData%\Programs\Git\bin\bash.exe"
    "C:\msys64\usr\bin\bash.exe"
    "C:\msys64\mingw64\bin\bash.exe"
    "C:\msys2\usr\bin\bash.exe"
) do (
    if not defined BASH if exist "%%~B" set "BASH=%%~B"
)

rem ---- 2) PATH 兜底 ----
if not defined BASH (
    where bash >nul 2>nul
    if not errorlevel 1 set "BASH=bash"
)

if not defined BASH (
    echo [ERROR] 未找到 bash.exe。
    echo         请安装 Git for Windows 或 MSYS2 后重试：
    echo           Git for Windows: https://git-scm.com/download/win
    echo           MSYS2:           https://www.msys2.org/
    echo.
    echo          （提示：在 cmd 里运行 where bash 可查看是否已在 PATH）
    pause
    exit /b 1
)

rem ---- 3) 切到仓库根并跑测试套件 ----
pushd "%ROOT%"
echo [MYP] 使用 bash: %BASH%
echo [MYP] 工作目录: %CD%
echo.
"%BASH%" tests/run_tests.sh %*
set "RC=%ERRORLEVEL%"
popd

echo.
echo [MYP] 测试套件退出码: %RC%   (0 = 全部通过, 1 = 有失败)
exit /b %RC%
