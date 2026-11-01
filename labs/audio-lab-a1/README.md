# A1 电子木鱼

⚠️ 本课代码未经真机验证（音频寄存器序列为 best-effort）。

用 ES8311 音频 DAC 播放正弦波音阶：每按一次 BOOT，音阶前进一个音（do-re-mi-...-do'，循环）并播放一记带包络的"木鱼"短音，屏幕显示当前音符名。

## 学什么

- Arduino-ESP32 3.x 新 I2S API（`ESP_I2S.h` 的 `I2SClass`）：`setPins()` + `begin()`
- 音频 DAC 的 I2C 配置流程（ES8311 @ 0x18：复位 → 时钟 → 数据通路 → 音量 → 解除静音）
- PA_EN 功放开关挂在 PCA9557 bit1，`initIoExpander()` 一次兼顾屏 CS 与功放
- 正弦波 + 包络线 = 最朴素的合成器

## 操作

```bash
cd ~/code/esp-labs-lichuang
uv run pio run -d labs/audio-lab-a1        # 编译
uv run pio run -d labs/audio-lab-a1 -t upload   # 烧录
uv run pio device monitor -d labs/audio-lab-a1  # 看日志
```

## 预期现象

1. 屏幕显示"A1 电子木鱼"和当前音符名（do）
2. 每按一次 BOOT：喇叭"哒"一声（一个约 0.45s 的衰减正弦音），音符名前进一格
3. 串口打印每次敲击的音符名与频率

## 排错

- **完全无声**：先确认 `initIoExpander()` 跑过（PA_EN 必须置 1 才有声音）；再用 `05-i2c-scan` 确认 ES8311（0x18）在线
- **有电流声/杂音但无音调**：I2C 寄存器序列是 UNVERIFIED 的 best-effort，对照 ES8311 数据手册核对时钟与 SDP（数据格式）寄存器
- **屏幕全黑**：`initIoExpander()` 必须在 `tft.init()` 之前（屏 CS 由 PCA9557 bit0 控制）
- **敲一下响很多声**：按键抖动，本课已有 20ms 消抖，可加长
