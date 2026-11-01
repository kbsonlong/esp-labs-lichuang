# 第 03 课：点亮屏幕

点亮板载 2.0" ST7789（320x240）：彩条 + 文字 + FPS，按 BOOT 键调背光。学 SPI 与 RGB565。详见 [docs/course.md 第 03 课](../../docs/course.md#第-03-课点亮屏幕labs03-display)。

## 完整操作

```sh
cd esp-labs-lichuang        # 仓库根目录（首次先跑一次 uv sync）
uv run pio run -d labs/03-display -t upload -t monitor
```

现象：屏幕亮起 → 上半屏 6 色彩条、下半屏 "Hello, lichuang!" 和运行秒数；串口每秒打一次 FPS；按 BOOT 键背光循环 4 档亮度。`Ctrl+C` 退出监视器。

排错：全黑先查背光；花屏查 `kit/include/pins.h`；反了改 `setRotation(3)`。

想刷回小智：仓库根目录跑 `./tools/flash-xiaozhi.sh`。
