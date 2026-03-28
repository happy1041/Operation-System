#!/bin/bash
# Pintos 一键启动脚本
# 使用 Docker 容器运行 PKU Pintos 开发环境

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PINTOS_SRC="$SCRIPT_DIR/pintos"
IMAGE_NAME="pintos-custom"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Pintos 开发环境启动脚本 ===${NC}"

# 检查 Docker 是否安装
if ! command -v docker &> /dev/null; then
    echo -e "${RED}错误: Docker 未安装或未在 PATH 中${NC}"
    echo "请先安装 Docker Desktop: https://www.docker.com/products/docker-desktop"
    exit 1
fi

# 检查 Docker 是否运行
if ! docker info &> /dev/null; then
    echo -e "${RED}错误: Docker 服务未运行${NC}"
    echo "请启动 Docker Desktop"
    exit 1
fi

# 检查 pintos 目录是否存在
if [ ! -d "$PINTOS_SRC" ]; then
    echo -e "${RED}错误: Pintos 源码目录不存在: $PINTOS_SRC${NC}"
    exit 1
fi

# 检查自定义镜像是否存在，不存在则构建
if ! docker images -q "$IMAGE_NAME" | grep -q .; then
    echo -e "${YELLOW}首次运行，构建自定义镜像（包含 ripgrep）...${NC}"
    if ! docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"; then
        echo -e "${RED}镜像构建失败，使用原始镜像${NC}"
        IMAGE_NAME="pkuflyingpig/pintos"
    fi
fi

# 停止并删除已存在的容器
echo -e "${YELLOW}清理旧容器...${NC}"
docker rm -f pintos 2>/dev/null

# 转换路径格式 (Windows Git Bash 需要)
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
    # Windows 路径转换: G:\path -> /g/path
    PINTOS_SRC=$(echo "$PINTOS_SRC" | sed 's|\\|/|g' | sed 's|^\([A-Za-z]\):|/\L\1|')
fi

echo -e "${GREEN}启动 Pintos 容器...${NC}"
echo "源码目录: $PINTOS_SRC"
echo "使用镜像: $IMAGE_NAME"
echo ""

# 启动容器
docker run -it --rm \
    --name pintos \
    --mount "type=bind,source=$PINTOS_SRC,target=/home/PKUOS/pintos" \
    "$IMAGE_NAME" \
    bash

echo -e "${GREEN}Pintos 容器已退出${NC}"
