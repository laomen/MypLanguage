#!/bin/bash
# 镜像 MYP 仓库到 Gitee (码云)
#
# 前提:
#   1. 在 gitee.com 新建空仓库 (不要初始化 README, 避免与本地历史冲突)
#      例如: https://gitee.com/<你的用户名>/MypLanguage
#   2. 本机已配置 Gitee 凭据 (HTTPS 用户名+密码 或 私人令牌)
#      - 未配置时: git push 会提示输入用户名/密码(令牌), 在终端手动输入
#
# 用法:
#   ./scripts/mirror_gitee.sh https://gitee.com/<用户名>/MypLanguage.git
#   或
#   GITEE_URL=https://gitee.com/<用户名>/MypLanguage.git ./scripts/mirror_gitee.sh
#
# 之后保持同步: git push gitee master

set -u
cd "$(dirname "$0")/.."

URL="${1:-${GITEE_URL:-}}"
if [ -z "$URL" ]; then
    echo "用法: $0 https://gitee.com/<用户名>/MypLanguage.git"
    echo "或设置环境变量 GITEE_URL"
    exit 1
fi

# 添加 gitee remote (若不存在)
if ! git remote | grep -qx gitee; then
    git remote add gitee "$URL"
    echo "已添加 remote: gitee -> $URL"
else
    # 更新已有 gitee remote 的 URL
    git remote set-url gitee "$URL"
    echo "已更新 remote: gitee -> $URL"
fi

echo "推送 master 到 gitee ..."
git push gitee master
echo ""
echo "完成! 若提示输入用户名/密码, 密码请填 Gitee 私人令牌 (私人令牌在 设置→安全设置→私人令牌 生成)"
