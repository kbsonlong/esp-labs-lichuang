# A2 录音机

⚠️ 本课代码未经真机验证（音频寄存器序列为 best-effort）。

ES7210 音频 ADC 录 5 秒（16kHz / 16bit / 单声道 ≈ 160KB，存 PSRAM），录完拼上 44 字节 WAV 头，再用 ES8311 回放。BOOT 键开始录音，录制进度条实时上屏。

## 学什么

- I2S 收方向（RX，DIN=12）：从 ES7210 搬 PCM
- PSRAM：`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`，160KB 缓冲的正确的家
- WAV 文件头：44 字节 RIFF/fmt/data 元数据，亲手拼一遍
- 全双工 `I2SClass`：`setPins()` 同时给 DOUT 和 DIN，begin 一次建一对 TX/RX 通道

## 操作

```bash
cd ~/code/esp-labs-lichuang
uv run pio run -d labs/audio-lab-a2
uv run pio run -d labs/audio-lab-a2 -t upload
uv run pio device monitor -d labs/audio-lab-a2
```

## 预期现象

1. 屏幕显示"A2 录音机"，提示按 BOOT 开始
2. 按 BOOT：红色进度条走 5 秒，串口打印录制进度
3. 录完自动回放（喇叭播出刚才 5 秒的声音），绿色进度条
4. 回放结束回到待机画面，可再次录音

## 排错

- **录出来全是静音**：ES7210 寄存器序列是 UNVERIFIED 的 best-effort——用 `05-i2c-scan` 确认 0x41 在线，然后重点核对主控格式(0x08)、SDP(0x09)、MIC 增益(0x14)与时钟(0x01)
- **录出来全是噪音**：采样率或 SDP 数据格式不匹配；确认 I2S 16kHz/16bit 与 ES7210 配置一致
- **回放无声**：PA_EN 必须=1（`initIoExpander()` 的最终 `0x02` 已含）；ES8311 静音寄存器(0x37)是否解除
- **PSRAM 分配失败**：确认 board 是 `esp32-s3-devkitc1-n16r8`（R8 才有 8MB PSRAM）
- **录音开头有"咔"声**：DMA 里残留旧数据，代码已空读一帧缓冲，如仍有可加大丢弃量
