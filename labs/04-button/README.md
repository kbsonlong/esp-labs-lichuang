# 第 04 课：把按键做对

BOOT 键的中断 + 消抖 + 长短按：短按换 LED 颜色、长按开关背光、屏幕滚动事件流。详见 [docs/course.md 第 04 课](../../docs/course.md#第-04-课把按键做对labs04-button)。

## 完整操作

```sh
cd esp-labs-lichuang        # 仓库根目录（首次先跑一次 uv sync）
uv run pio run -d labs/04-button -t upload -t monitor
```

现象：屏幕显示"短按: 换色 长按: 背光"；短按 BOOT 换 LED 颜色，按住 ≥800ms 切背光；事件实时上屏，串口同步打 `按下 #n`。`Ctrl+C` 退出监视器。

想刷回小智：仓库根目录跑 `./tools/flash-xiaozhi.sh`。
