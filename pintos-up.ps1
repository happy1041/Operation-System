# Pintos 一键启动脚本 (PowerShell)
# 使用 Docker 容器运行 PKU Pintos 开发环境

$ErrorActionPreference = "Stop"

Write-Host "=== Pintos 开发环境启动脚本 ===" -ForegroundColor Green

# 获取脚本所在目录
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PintosSrc = (Join-Path $ScriptDir "pintos") -replace '\\', '/'
$ImageName = "pintos-custom"

# 检查 Docker 是否安装
try {
    $null = Get-Command docker -ErrorAction Stop
} catch {
    Write-Host "错误: Docker 未安装或未在 PATH 中" -ForegroundColor Red
    Write-Host "请先安装 Docker Desktop: https://www.docker.com/products/docker-desktop"
    exit 1
}

# 检查 Docker 是否运行
$dockerInfo = docker info 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "错误: Docker 服务未运行" -ForegroundColor Red
    Write-Host "请启动 Docker Desktop"
    exit 1
}

# 检查 pintos 目录是否存在
if (-not (Test-Path $PintosSrc)) {
    Write-Host "错误: Pintos 源码目录不存在: $PintosSrc" -ForegroundColor Red
    exit 1
}

# 检查自定义镜像是否存在，不存在则构建
$imageExists = docker images -q $ImageName 2>$null
if (-not $imageExists) {
    Write-Host "首次运行，构建自定义镜像（包含 ripgrep）..." -ForegroundColor Yellow
    docker build -t $ImageName $ScriptDir
    if ($LASTEXITCODE -ne 0) {
        Write-Host "镜像构建失败，使用原始镜像" -ForegroundColor Red
        $ImageName = "pkuflyingpig/pintos"
    }
}

# 停止并删除已存在的容器
Write-Host "清理旧容器..." -ForegroundColor Yellow
docker rm -f pintos 2>$null

Write-Host "启动 Pintos 容器..." -ForegroundColor Green
Write-Host "源码目录: $PintosSrc"
Write-Host "使用镜像: $ImageName"
Write-Host ""

# 启动容器
docker run -it --rm `
    --name pintos `
    --mount "type=bind,source=$PintosSrc,target=/home/PKUOS/pintos" `
    $ImageName `
    bash

Write-Host "Pintos 容器已退出" -ForegroundColor Green
