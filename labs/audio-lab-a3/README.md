# A3 回声机器

⚠️ 本课代码未经真机验证（音频寄存器序列为 best-effort）。

按住 BOOT 说话：ES7210 录音的同时每帧算 RMS 音量，音量条实时上屏（屏 + I2S + I2C 三路并行）；松开 BOOT 立即回放，像一台对讲机/回声机。

## 学什么

- 真全双工：一个 `I2SClass` 同时跑 TX（DOUT=45）与 RX（DIN=12）
- 实时电平表：RMS（均方根）就是"音量"，静噪门限 + 映射成进度条
- 主循环并行：录音块间穿插刷屏（30ms 节流），SPI 屏不拖慢音频搬运

## 操作

```bash
cd ~/code/esp-labs-lichuang
uv run pio run -d labs/audio-lab-a3
uv run pio run -d labs/audio-lab-a3 -t upload
uv run pio device monitor -d labs/audio-lab-a3
```

## 预期现象

1. 屏幕显示"A3 回声机器"，提示按住 BOOT 说话
2. 按住 BOOT：绿色音量条跟着说话实时跳动（大声变红），下方显示 REC 秒数
3. 松开 BOOT：喇叭回放刚才录的内容（最长 10 秒），青色进度条
4. 回放完回待机；录音短于 0.25 秒视为误触忽略

## 排错

- **音量条不动**：ES7210 寄存器序列是 UNVERIFIED 的 best-effort——用 `05-i2c-scan` 确认 0x41 在线；核对 MIC 增益(0x14)是否够大
- **回放无声**：PA_EN 必须=1（`initIoExpander()` 的最终 `0x02` 已含）；ES8311(0x18) 是否在线、静音(0x37)是否解除
- **回声啸叫**：这是"录完才回放"的设计，不会实时啸叫；若想实时监听会啸叫（正反馈）
- **声音断续**：串口监视器打印过多拖慢循环；或 PSRAM 分配失败降级到了内部 RAM
- **屏幕全黑**：`initIoExpander()` 必须在 `tft.init()` 之前
