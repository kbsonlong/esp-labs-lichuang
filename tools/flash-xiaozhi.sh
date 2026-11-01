#!/usr/bin/env bash
# 一键刷回小智（xiaozhi-esp32 官方固件，立创·实战派 ESP32-S3 版）。
#
# 用法（仓库根目录执行）:
#   ./tools/flash-xiaozhi.sh              # 自动探测串口
#   ./tools/flash-xiaozhi.sh /dev/cu.usbmodem212301   # 指定串口
#
# 首次运行会从 GitHub 下载官方 release（约 3MB）并做 SHA256 校验，之后走本地缓存。
# 固件是 merged 镜像（bootloader+分区表+应用），从 0x0 全量覆盖——教程 lab 会被清掉。
set -euo pipefail

VER="${XIAOZHI_VER:-v2.5.2}"
BOARD="lckfb-lichuang-dev"
# 官方 SHA256SUMS 里 lckfb-lichuang-dev 的校验值（换版本时同步更新这里）
SHA_EXPECTED="3a25b49ec73af0dab438044e1211e9d12dc86d8e60df9ab953d5959114a7f8b9"

PORT="${1:-auto}"
cd "$(dirname "$0")/.."          # repo 根目录
CACHE="tools/.cache"
mkdir -p "$CACHE"
ZIP="$CACHE/${VER}_${BOARD}.zip"

if [ ! -f "$CACHE/merged-binary.bin" ]; then
  URL="https://github.com/78/xiaozhi-esp32/releases/download/${VER}/${VER}_${BOARD}.zip"
  echo "==> 下载 $URL"
  curl -fL --progress-bar -o "$ZIP" "$URL"
  echo "$SHA_EXPECTED  $ZIP" | shasum -a 256 -c -
  unzip -oq "$ZIP" -d "$CACHE"
fi

if [ "$PORT" = "auto" ]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
  echo "!! 找不到串口。请检查："
  echo "   1. USB 线插好（数据线，不是充电线）"
  echo "   2. 直插电脑端口，别走 hub"
  echo "   3. 如果程序崩溃导致串口消失：拔 USB -> 按住 BOOT -> 插 USB -> 松开，再重跑本脚本"
  exit 1
fi
echo "==> 串口: $PORT"

echo "==> 开始刷写（全量覆盖 0x0，教程 lab 内容会被清掉）..."
uv run esptool --chip esp32s3 --port "$PORT" --baud 921600 write_flash 0x0 "$CACHE/merged-binary.bin"

echo
echo "==> 完成！板子已复位。屏幕出现小智启动画面即成功。"
echo "    首次使用到 xiaozhi.me 控制台添加设备，输入屏幕上的验证码。"
