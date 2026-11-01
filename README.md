# esp-labs-lichuang：立创·实战派 ESP32-S3 新手教程

给**立创·实战派 ESP32-S3**（LCKFB，2.0" 屏 + 麦克风 + 扬声器 + 摄像头一体板）定制的零基础动手课程。每课一个独立 PlatformIO 工程，全部利用**板载外设、零接线**——插上 USB 就能学。

课程文本：[docs/course.md](docs/course.md)。

## 这是什么 / 不是什么

- 这是 esp-labs（本地 `~/code/esp-labs`，面向面包板 DevKitC 套件）的**实战派移植与改写版**：课程结构和方法论一脉相承，引脚、外设、课程内容全部按实战派硬件重做。
- 引脚定义以 xiaozhi-esp32 官方板级配置（`main/boards/lckfb/szpi-esp32s3`）为准，逐脚核对。

## 硬件

立创·实战派 ESP32-S3：ESP32-S3-WROOM-1-N16R8（16MB flash + 8MB PSRAM）、2.0" ST7789 320x240 IPS 屏、FT5x06 触摸、ES8311+ES7210 音频、OV2640 摄像头、WS2812 RGB LED、BOOT 键。一块板走完从点灯到语音的全部课程。

## 快速开始

```sh
uv sync                                  # 一次性：把 PlatformIO 装进本目录 .venv
uv run pio device list                   # 板子在哪个串口？
uv run pio run -d labs/01-hello -t upload -t monitor    # 编译 + 烧录 + 看日志
ESP_PORT=/dev/cu.usbmodem212301 uv run pio run -d labs/01-hello -t upload   # 强制指定端口
```

macOS 提示：板子插 hub 可能每几秒闪断，**直插 Mac 本机端口**最稳。刷任何固件前确认串口稳定存在。

## 课程地图（基础篇）

| # | 课程 | 工程 | 你学会 |
| - | ---- | ---- | ------ |
| 01 | 认识板子 | `labs/01-hello` | setup/loop、串口日志、身份卡（确认 16MB+8MB）、WS2812 |
| 02 | 灯光状态机 | `labs/02-led` | 非阻塞 millis()、状态机、模式切换 |
| 03 | 点亮屏幕 | `labs/03-display` | SPI 四线、RGB565、背光 PWM、帧率 |
| 04 | 把按键做对 | `labs/04-button` | 中断、消抖、长按/短按 |
| 05 | I2C 总线扫描 | `labs/05-i2c-scan` | I2C 协议、地址、探测板载外设 |

进度：基础篇 5/5 ✅、W1 ✅、W2 ✅ 已真机验证；其余 14 课代码已全部输出（编译 14/14 通过，待真机验收，各课 README 有 ⚠️ 标注）；实战篇 P1-P3 需求文档见 docs/projects.md。完整课程设计见 [docs/course.md](docs/course.md)。

## 板卡引脚速查

见 [kit/include/pins.h](kit/include/pins.h)——所有课程共用这一个事实来源，注释里标了哪些引脚被板载外设占用、哪些（10/11/21/47）可以自由外接。

## 一键刷回小智

折腾完课程想回小智，一条命令（首次自动下载官方 release 并做 SHA256 校验，之后走本地缓存）：

```sh
./tools/flash-xiaozhi.sh                          # 自动探测串口
./tools/flash-xiaozhi.sh /dev/cu.usbmodem212301   # 或手动指定
```

⚠️ merged 镜像从 0x0 全量覆盖，教程 lab 会被清掉（随时可重刷回来）。

## 致谢

课程结构与部分代码风格源自本地 esp-labs 项目；硬件引脚定义参考 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（Apache-2.0）。
