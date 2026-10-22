#!/bin/bash
# 智能安装脚本 - 自动选择最快镜像源
set -e

echo "======================================"
echo "  机器人综合控制系统 - 智能安装"
echo "======================================"
echo ""

# 检查 pip3
if ! command -v pip3 &> /dev/null; then
    echo "错误: 未找到 pip3，请先安装 python3-pip"
    exit 1
fi

# 镜像源列表（按速度/稳定性排序）
MIRRORS=(
    "https://mirrors.aliyun.com/pypi/simple"
    "https://pypi.tuna.tsinghua.edu.cn/simple"
    "https://mirrors.cloud.tencent.com/pypi/simple"
    "https://mirrors.huaweicloud.com/repository/pypi/simple"
    "https://mirrors.ustc.edu.cn/pypi/web/simple"
    "https://pypi.org/simple"
)

# 测试并选择最快的源
echo "正在检测最快镜像源..."
BEST_MIRROR=""
BEST_TIME=999999

for mirror in "${MIRRORS[@]}"; do
    echo -n "  测试 $mirror ... "
    # 用 pip index versions 测速，3秒超时
    start=$(date +%s%N)
    if timeout 3 pip3 index versions PySide6 -i "$mirror" &>/dev/null; then
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))  # 转换为毫秒
        echo "✓ ${elapsed}ms"
        if [ $elapsed -lt $BEST_TIME ]; then
            BEST_TIME=$elapsed
            BEST_MIRROR=$mirror
        fi
    else
        echo "✗ 不可用"
    fi
done

if [ -z "$BEST_MIRROR" ]; then
    echo ""
    echo "⚠️  所有镜像源都不可用，将使用默认源"
    BEST_MIRROR="https://pypi.org/simple"
fi

echo ""
echo "✓ 选择最快源: $BEST_MIRROR (${BEST_TIME}ms)"
echo ""
echo "======================================"
echo "  开始安装依赖"
echo "======================================"

# 升级 pip（使用最快源）
echo "升级 pip..."
pip3 install --upgrade pip -i "$BEST_MIRROR" --quiet --timeout 30 --user 2>/dev/null || \
    pip3 install --upgrade pip -i "$BEST_MIRROR" --quiet --timeout 30

# 安装依赖
echo "安装 PySide6 和 websocket-client ..."
echo "(PySide6 约 100MB，请耐心等待)"
echo ""

pip3 install \
    -i "$BEST_MIRROR" \
    --timeout 60 \
    --retries 2 \
    --user \
    PySide6 websocket-client

# 验证
echo ""
echo "======================================"
echo "  验证安装"
echo "======================================"
python3 -c "import PySide6; print(f'✓ PySide6: {PySide6.__version__}')"
python3 -c "import websocket; print('✓ websocket-client: OK')"

echo ""
echo "======================================"
echo "  ✓ 安装完成！"
echo "======================================"
echo ""
echo "运行程序: python3 main.py"
