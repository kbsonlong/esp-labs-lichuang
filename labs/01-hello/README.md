# 第 01 课：认识板子

打印芯片身份卡（确认 16MB flash + 8MB PSRAM），RGB LED 转色轮。详见 [docs/course.md 第 01 课](../../docs/course.md#第-01-课认识板子labs01-hello)。

## 完整操作

```sh
cd esp-labs-lichuang        # 仓库根目录（首次先跑一次 uv sync）
uv run pio run -d labs/01-hello -t upload -t monitor
```

打开监视器后按一下 **回车**，会打出芯片身份卡；`Ctrl+C` 退出监视器。

串口连不上时先跑 `uv run pio device list` 看板子在不在；在但连不上就强制指定端口：

```sh
ESP_PORT=/dev/cu.usbmodem212301 uv run pio run -d labs/01-hello -t upload -t monitor
```

想刷回小智：仓库根目录跑 `./tools/flash-xiaozhi.sh`。
