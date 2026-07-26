# MYP Language for VS Code

MYP 语言支持扩展——语法高亮、诊断、代码补全、悬停信息、符号列表。

## 功能

- ✅ **语法高亮** — 关键字、类型、字符串、注释、运算符
- ✅ **实时诊断** — 编译错误自动标红（需要先编译 `myp_lsp`）
- ✅ **代码补全** — 关键字、类名、方法名、属性名
- ✅ **悬停信息** — 鼠标悬停显示类型签名
- ✅ **文档符号** — 大纲视图显示类、函数、枚举
- ✅ **包管理集成** — 支持 `myp init/build/install/run`

## 安装

### 前提条件

编译 MYP 编译器（需要 LLVM 21）：

```bash
cd MYPLanguage/build
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm
make -j$(nproc)
```

### 安装扩展

```bash
# 方法 1：复制到 VS Code 扩展目录
cp -r vscode-myp ~/.vscode/extensions/myp-lang.vscode-myp

# 方法 2：打包为 VSIX
npm install -g @vscode/vsce
cd vscode-myp
vsce package
code --install-extension vscode-myp-*.vsix
```

### 配置

在 VS Code 设置中搜索 `myp`，可配置：

| 设置 | 说明 |
|------|------|
| `myp.lspPath` | `myp_lsp` 可执行文件路径（默认自动查找） |
| `myp.stdlibPath` | 标准库目录路径（默认自动查找） |
| `myp.trace.server` | LSP 通信日志开关 |

## 使用

打开任意 `.myp` 文件即可自动激活语言服务。

![demo](https://via.placeholder.com/600x400?text=MYP+Extension+Screenshot)

## 构建命令

```bash
myp init mypkg    # 创建新包
myp build         # 构建当前包
myp run           # 构建并运行
myp install <path> # 安装依赖
```
