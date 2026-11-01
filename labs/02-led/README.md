# 第 02 课：灯光状态机

WS2812 三种灯效（呼吸/彩虹/快闪），按 BOOT 键切换。学非阻塞 `millis()` 与状态机。详见 [docs/course.md 第 02 课](../../docs/course.md#第-02-课灯光状态机labs02-led)。

## 完整操作

```sh
cd esp-labs-lichuang        # 仓库根目录（首次先跑一次 uv sync）
uv run pio run -d labs/02-led -t upload -t monitor
```

现象：LED 呼吸 -> 按 BOOT 键 -> 切彩虹 -> 再按 -> 快闪；串口同步打印 `切换模式 -> xxx`。`Ctrl+C` 退出监视器。

串口连不上时强制指定端口：

```sh
ESP_PORT=/dev/cu.usbmodem212301 uv run pio run -d labs/02-led -t upload -t monitor
```

想刷回小智：仓库根目录跑 `./tools/flash-xiaozhi.sh`。
