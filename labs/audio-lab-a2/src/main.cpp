// A2 录音机。
//
// 这一课学：
// 1. I2S 收（RX）方向：DIN=12 接 ES7210 音频 ADC 的输出，全双工配好 pins
//    之后 readBytes 就是"从麦克风搬 PCM"。
// 2. PSRAM 用武之地：16kHz x 16bit x 单声道 x 5 秒 = 160KB，主 RAM 里算奢侈，
//    N16R8 的 8MB PSRAM 里九牛一毛。heap_caps_malloc(..., MALLOC_CAP_SPIRAM)。
// 3. WAV 文件头：44 字节的 RIFF 元数据 + 裸 PCM。录完亲手拼一个头，
//    这份缓冲就随时可以导出/存卡/网络发送。
// 4. 回放：把单声道样本复制成立体声交错帧，从 DOUT=45 送进 ES8311。
//
// 玩法：按 BOOT 开始录 5 秒（进度条上屏），录完自动回放。
//
// ⚠️ 本课代码未经真机验证：ES8311 / ES7210 寄存器序列按公开驱动资料 best-effort 抄写（见 UNVERIFIED 注释）。
#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <LovyanGFX.hpp>

#include "pins.h"

namespace {

// ---- 屏驱动（同 W1/W2，真机验证过的配置）----
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_TFT_SCLK;
      cfg.pin_mosi = PIN_TFT_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_TFT_DC;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_TFT_CS;
      cfg.pin_rst = PIN_TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

constexpr uint16_t kCyan = 0x07FF, kYellow = 0xFFE0, kGreen = 0x07E0, kRed = 0xF800, kGrey = 0x7BEF;

void initIoExpander() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  Wire.write(0x03);
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x03);
  Wire.write(0xF8);
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  Wire.write(0x02);  // bit0=0 屏 CS 选中，bit1=1 PA_EN=1（功放开）
  Wire.endTransmission();
}

// ---- 通用小工具：I2C 写一个寄存器 ----
bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// ---- ES8311 音频 DAC（0x18）：回放用 ----
// ⚠️ UNVERIFIED：寄存器序列 best-effort，同 A1，出问题对照数据手册核。
void es8311Init() {
  i2cWriteReg(0x18, 0x00, 0x80);  // UNVERIFIED: 软复位
  delay(10);
  i2cWriteReg(0x18, 0x00, 0x00);  // UNVERIFIED: 解除复位
  i2cWriteReg(0x18, 0x01, 0x3F);  // UNVERIFIED: 时钟管理，先关分频输出
  i2cWriteReg(0x18, 0x02, 0x00);  // UNVERIFIED: 时钟管理
  i2cWriteReg(0x18, 0x03, 0x10);  // UNVERIFIED: ADC 采样率档位
  i2cWriteReg(0x18, 0x04, 0x10);  // UNVERIFIED: DAC 采样率档位，MCLK = 256fs
  i2cWriteReg(0x18, 0x0D, 0x01);  // UNVERIFIED: 时钟/数据路径
  i2cWriteReg(0x18, 0x0E, 0x02);  // UNVERIFIED: 内部参考与偏置
  i2cWriteReg(0x18, 0x12, 0x00);  // UNVERIFIED: DAC 输入 = I2S
  i2cWriteReg(0x18, 0x13, 0x10);  // UNVERIFIED: DAC 输出通道配置
  i2cWriteReg(0x18, 0x14, 0x0A);  // UNVERIFIED: DAC 模拟段设置
  i2cWriteReg(0x18, 0x32, 0xBF);  // UNVERIFIED: DAC 数字音量
  i2cWriteReg(0x18, 0x37, 0x08);  // UNVERIFIED: 解除静音
  Serial.println("[ES8311] I2C 配置完成（UNVERIFIED）");
}

// ---- ES7210 音频 ADC（0x41）：录音用 ----
// ⚠️ UNVERIFIED：寄存器序列按公开驱动资料（esp-adf es7210 驱动）best-effort 抄写。
// 若录到全零/全噪音，重点核对：主控格式(0x08)、SDP(0x09/0x0A)、MIC 增益(0x14+)、时钟(0x01)。
void es7210Init() {
  i2cWriteReg(0x41, 0x00, 0xFF);  // UNVERIFIED: 软复位全部寄存器
  delay(10);
  i2cWriteReg(0x41, 0x00, 0x00);  // UNVERIFIED: 解除复位
  i2cWriteReg(0x41, 0x01, 0x3F);  // UNVERIFIED: 时钟管理，配置期间先关时钟
  i2cWriteReg(0x41, 0x06, 0x00);  // UNVERIFIED: MCLK 分频 = 1
  i2cWriteReg(0x41, 0x07, 0x20);  // UNVERIFIED: OSR 档位
  i2cWriteReg(0x41, 0x08, 0x10);  // UNVERIFIED: 主控：16bit
  i2cWriteReg(0x41, 0x09, 0x30);  // UNVERIFIED: SDP：ADC1/2 = I2S 格式 16bit
  i2cWriteReg(0x41, 0x0A, 0x30);  // UNVERIFIED: SDP：ADC3/4 同上
  i2cWriteReg(0x41, 0x20, 0x0A);  // UNVERIFIED: 麦克风偏置与上电
  i2cWriteReg(0x41, 0x14, 0x28);  // UNVERIFIED: MIC1 增益（0x28 ≈ 30dB）
  i2cWriteReg(0x41, 0x15, 0x28);  // UNVERIFIED: MIC2 增益
  i2cWriteReg(0x41, 0x18, 0x00);  // UNVERIFIED: ADC1 数字音量 = 0dB
  i2cWriteReg(0x41, 0x1A, 0x00);  // UNVERIFIED: ADC 静音解除
  i2cWriteReg(0x41, 0x01, 0x00);  // UNVERIFIED: 开时钟，开始工作
  Serial.println("[ES7210] I2C 配置完成（UNVERIFIED）");
}

// ---- 新 I2S API：全双工（dout + din 同时给，begin 会建一对 TX/RX 通道）----
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kSeconds = 5;
constexpr size_t kMonoSamples = kSampleRate * kSeconds;      // 80000 样本
constexpr size_t kWavSize = 44 + kMonoSamples * 2;           // 头 44B + 160KB PCM
constexpr size_t kFramesPerChunk = 512;                      // 每次搬运 512 帧

I2SClass i2s;
uint8_t *g_wav = nullptr;  // PSRAM：44 字节头 + 单声道 16bit PCM

// 手工拼 44 字节标准 WAV 头（PCM 16bit 单声道 16kHz）。
// 注：WAV 是小端格式，ESP32 也是小端，直接按字节写即可。
void buildWavHeader(uint8_t *h, uint32_t sampleRate, uint32_t dataBytes) {
  memcpy(h + 0, "RIFF", 4);
  uint32_t riffSize = 36 + dataBytes;
  memcpy(h + 4, &riffSize, 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  uint32_t fmtSize = 16;
  memcpy(h + 16, &fmtSize, 4);
  uint16_t fmtPcm = 1;  // PCM
  memcpy(h + 20, &fmtPcm, 2);
  uint16_t ch = 1;
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sampleRate, 4);
  uint32_t byteRate = sampleRate * 2;  // 单声道 16bit
  memcpy(h + 28, &byteRate, 4);
  uint16_t align = 2;
  memcpy(h + 32, &align, 2);
  uint16_t bits = 16;
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
}

// 录 kSeconds 秒：读立体声帧，只留左声道（ES7210 单麦输出在左槽）。
// 每搬运约 1/10 秒刷一次进度条。
void record() {
  int16_t *chunk = (int16_t *)malloc(kFramesPerChunk * 2 * sizeof(int16_t));  // 立体声帧
  size_t got = 0;
  size_t lastDrawn = SIZE_MAX;
  // 先空读一帧缓冲：丢掉按键按下前积压在 DMA 里的旧数据
  i2s.readBytes((char *)chunk, kFramesPerChunk * 4);
  while (got < kMonoSamples) {
    const size_t n = i2s.readBytes((char *)chunk, kFramesPerChunk * 4);
    if (n == 0) continue;
    const size_t frames = n / 4;  // 4 字节/立体声帧
    for (size_t i = 0; i < frames && got < kMonoSamples; i++) {
      memcpy(g_wav + 44 + got * 2, &chunk[2 * i], 2);  // 左声道 = 偶数样本
      got++;
    }
    // 进度条（每变化 10% 才重画，别让 SPI 屏拖慢搬运）
    const size_t pct = got * 10 / kMonoSamples;
    if (pct != lastDrawn) {
      lastDrawn = pct;
      tft.fillRect(20, 140, (int)(200 * pct / 10), 28, kRed);
      tft.setTextColor(kYellow);
      tft.setTextSize(2);
      tft.fillRect(0, 190, 320, 24, 0x0000);
      tft.setCursor(20, 190);
      tft.printf("录制中 %.1f / %d 秒", (double)(got / (float)kSampleRate), (int)kSeconds);
    }
  }
  free(chunk);
}

// 回放：单声道样本复制成立体声交错帧写 TX。
void playback() {
  static int16_t stereo[kFramesPerChunk * 2];
  size_t sent = 0;
  size_t lastDrawn = SIZE_MAX;
  while (sent < kMonoSamples) {
    const size_t n = (kMonoSamples - sent < kFramesPerChunk) ? (kMonoSamples - sent) : kFramesPerChunk;
    const int16_t *src = (const int16_t *)(g_wav + 44 + sent * 2);
    for (size_t i = 0; i < n; i++) {
      stereo[2 * i] = src[i];
      stereo[2 * i + 1] = src[i];
    }
    i2s.write(stereo, n * 4);
    sent += n;
    const size_t pct = sent * 10 / kMonoSamples;
    if (pct != lastDrawn) {
      lastDrawn = pct;
      tft.fillRect(20, 140, (int)(200 * pct / 10), 28, kGreen);
    }
  }
}

void drawIdle() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("A2 录音机");
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 70);
  tft.print("按 BOOT 开始录音");
  tft.setTextColor(kCyan);
  tft.setCursor(8, 100);
  tft.printf("16kHz 16bit 单声道 %d 秒", (int)kSeconds);
  tft.drawRect(19, 139, 202, 30, kGrey);  // 进度条外框
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== A2 录音机 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 110);
  tft.print("初始化音频...");

  // 160KB 缓冲放 PSRAM（N16R8 的 8MB 八线 PSRAM）
  g_wav = (uint8_t *)heap_caps_malloc(kWavSize, MALLOC_CAP_SPIRAM);
  if (g_wav == nullptr) {
    log_e("PSRAM 分配失败（%u 字节）", kWavSize);
    while (true) delay(1000);
  }
  Serial.printf("[内存] WAV 缓冲 %u 字节 @ PSRAM %p\n", kWavSize, g_wav);

  es7210Init();
  es8311Init();

  // 全双工 pins：BCLK=14 WS=13 DOUT=45 DIN=12 MCLK=38（官方引脚表）
  i2s.setPins(14, 13, 45, 12, 38);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    log_e("I2S 初始化失败");
    while (true) delay(1000);
  }
  Serial.printf("[I2S] 全双工就绪: %lu Hz 16bit stereo\n", (unsigned long)kSampleRate);

  drawIdle();
}

void loop() {
  static bool lastPressed = false;
  static uint32_t lastEdge = 0;
  const bool pressed = (digitalRead(PIN_BOOT_BTN) == LOW);
  if (pressed != lastPressed && millis() - lastEdge > 20) {
    lastEdge = millis();
    lastPressed = pressed;
    if (pressed) {
      // ---- 录 5 秒 ----
      Serial.println("[录音] 开始");
      tft.fillRect(20, 140, 200, 28, 0x0000);
      record();
      buildWavHeader(g_wav, kSampleRate, kMonoSamples * 2);
      Serial.printf("[录音] 完成，已拼 WAV 头，共 %u 字节\n", kWavSize);

      // ---- 回放 ----
      Serial.println("[回放] 开始");
      tft.fillRect(0, 190, 320, 24, 0x0000);
      tft.setTextColor(kGreen);
      tft.setTextSize(2);
      tft.setCursor(20, 190);
      tft.print("回放中...");
      tft.fillRect(20, 140, 200, 28, 0x0000);
      playback();
      Serial.println("[回放] 完成");
      drawIdle();
    }
  }
  delay(5);
}
