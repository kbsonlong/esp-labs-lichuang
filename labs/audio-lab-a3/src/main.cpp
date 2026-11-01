// A3 回声机器。
//
// 这一课学：
// 1. 真·全双工：一个 I2S 外设同时跑 TX（DOUT=45 -> ES8311）和 RX（DIN=12 <- ES7210），
//    屏 + I2S + I2C 三路并行——录音循环里穿插刷屏，谁也不等谁。
// 2. 实时电平表：每帧算 RMS（均方根）就是"音量"，映射成进度条宽度，
//    录音软件里的电平表原理就这么朴素。
// 3. 按住说话、松手停止：BOOT 按下开始录，松开立即回放，像对讲机。
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

bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// ---- ES8311 音频 DAC（0x18）----
// ⚠️ UNVERIFIED：寄存器序列 best-effort，同 A1/A2。
void es8311Init() {
  i2cWriteReg(0x18, 0x00, 0x80);  // UNVERIFIED: 软复位
  delay(10);
  i2cWriteReg(0x18, 0x00, 0x00);  // UNVERIFIED: 解除复位
  i2cWriteReg(0x18, 0x01, 0x3F);  // UNVERIFIED: 时钟管理，先关分频输出
  i2cWriteReg(0x18, 0x02, 0x00);  // UNVERIFIED: 时钟管理
  i2cWriteReg(0x18, 0x03, 0x10);  // UNVERIFIED: ADC 采样率档位
  i2cWriteReg(0x18, 0x04, 0x10);  // UNVERIFIED: DAC 采样率档位
  i2cWriteReg(0x18, 0x0D, 0x01);  // UNVERIFIED: 时钟/数据路径
  i2cWriteReg(0x18, 0x0E, 0x02);  // UNVERIFIED: 内部参考与偏置
  i2cWriteReg(0x18, 0x12, 0x00);  // UNVERIFIED: DAC 输入 = I2S
  i2cWriteReg(0x18, 0x13, 0x10);  // UNVERIFIED: DAC 输出通道配置
  i2cWriteReg(0x18, 0x14, 0x0A);  // UNVERIFIED: DAC 模拟段设置
  i2cWriteReg(0x18, 0x32, 0xBF);  // UNVERIFIED: DAC 数字音量
  i2cWriteReg(0x18, 0x37, 0x08);  // UNVERIFIED: 解除静音
  Serial.println("[ES8311] I2C 配置完成（UNVERIFIED）");
}

// ---- ES7210 音频 ADC（0x41）----
// ⚠️ UNVERIFIED：寄存器序列 best-effort（esp-adf es7210 驱动风格），同 A2。
void es7210Init() {
  i2cWriteReg(0x41, 0x00, 0xFF);  // UNVERIFIED: 软复位
  delay(10);
  i2cWriteReg(0x41, 0x00, 0x00);  // UNVERIFIED: 解除复位
  i2cWriteReg(0x41, 0x01, 0x3F);  // UNVERIFIED: 配置期间先关时钟
  i2cWriteReg(0x41, 0x06, 0x00);  // UNVERIFIED: MCLK 分频 = 1
  i2cWriteReg(0x41, 0x07, 0x20);  // UNVERIFIED: OSR 档位
  i2cWriteReg(0x41, 0x08, 0x10);  // UNVERIFIED: 主控：16bit
  i2cWriteReg(0x41, 0x09, 0x30);  // UNVERIFIED: SDP：ADC1/2 = I2S 16bit
  i2cWriteReg(0x41, 0x0A, 0x30);  // UNVERIFIED: SDP：ADC3/4 同上
  i2cWriteReg(0x41, 0x20, 0x0A);  // UNVERIFIED: 麦克风偏置与上电
  i2cWriteReg(0x41, 0x14, 0x28);  // UNVERIFIED: MIC1 增益
  i2cWriteReg(0x41, 0x15, 0x28);  // UNVERIFIED: MIC2 增益
  i2cWriteReg(0x41, 0x18, 0x00);  // UNVERIFIED: ADC1 数字音量 = 0dB
  i2cWriteReg(0x41, 0x1A, 0x00);  // UNVERIFIED: ADC 静音解除
  i2cWriteReg(0x41, 0x01, 0x00);  // UNVERIFIED: 开时钟
  Serial.println("[ES7210] I2C 配置完成（UNVERIFIED）");
}

// ---- 全双工 I2S + PSRAM 录音缓冲 ----
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kMaxSeconds = 10;                       // 最长按住 10 秒
constexpr size_t kMaxSamples = kSampleRate * kMaxSeconds;
constexpr size_t kFramesPerChunk = 256;                  // 16ms/块，刷屏节奏刚好

I2SClass i2s;
int16_t *g_rec = nullptr;  // PSRAM：单声道 16bit

// 每块 256 帧的立体声缓冲（放内部 RAM，搬运热路径别碰 PSRAM 读改写）
int16_t g_stereo[kFramesPerChunk * 2];

// 画一次音量条：满宽 200px，噪声门以下显示 0
void drawVolumeBar(int level) {
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  const int w = 2 * level;  // 100 -> 200px
  static int lastW = -1;
  if (w == lastW) return;
  if (w > lastW) {
    tft.fillRect(60 + lastW, 150, w - lastW, 24, (level > 85) ? kRed : kGreen);
  } else {
    tft.fillRect(60 + w, 150, lastW - w, 24, 0x0000);
  }
  lastW = w;
}

void drawIdle() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("A3 回声机器");
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 70);
  tft.print("按住 BOOT 说话");
  tft.setTextColor(kCyan);
  tft.setCursor(8, 100);
  tft.print("松开回放");
  tft.drawRect(59, 149, 202, 26, kGrey);  // 音量条外框
}

// 按住录（松手/满 10 秒停），录完回放。返回录制样本数。
size_t recordWhileHeld() {
  size_t got = 0;
  bool lastDrawSec = false;
  uint32_t lastBarMs = 0;
  // 丢掉按键前积压在 DMA 里的旧数据
  i2s.readBytes((char *)g_stereo, sizeof(g_stereo));
  while (digitalRead(PIN_BOOT_BTN) == LOW && got < kMaxSamples) {
    const size_t n = i2s.readBytes((char *)g_stereo, sizeof(g_stereo));
    if (n == 0) continue;
    const size_t frames = n / 4;
    // 搬运左声道进 PSRAM + 累计平方和
    int64_t sumSq = 0;
    const int16_t *src = g_stereo;
    int16_t *dst = g_rec + got;
    for (size_t i = 0; i < frames && got < kMaxSamples; i++) {
      const int16_t s = src[2 * i];  // 左声道
      dst[i] = s;
      sumSq += (int64_t)s * s;
      got++;
    }
    // RMS -> 音量条（对数感更好，这里开方后放大；静噪门限 300）
    const float rms = sqrtf((float)(sumSq / (frames ? frames : 1)));
    int level = (int)(rms * 100.0f / 8000.0f);
    if (rms < 300.0f) level = 0;
    if (millis() - lastBarMs >= 30) {  // 30ms 刷一次条，屏和录音并行不拖后腿
      lastBarMs = millis();
      drawVolumeBar(level);
      const bool sec = (got / kSampleRate) != ((got - frames) / kSampleRate);
      if (sec != lastDrawSec || got == frames) {
        lastDrawSec = sec;
        tft.fillRect(0, 190, 320, 24, 0x0000);
        tft.setTextColor(kYellow);
        tft.setTextSize(2);
        tft.setCursor(20, 190);
        tft.printf("REC %.1fs", (double)(got / (float)kSampleRate));
      }
    }
  }
  return got;
}

void playback(size_t samples) {
  size_t sent = 0;
  size_t lastPct = SIZE_MAX;
  while (sent < samples) {
    const size_t n = (samples - sent < kFramesPerChunk) ? (samples - sent) : kFramesPerChunk;
    for (size_t i = 0; i < n; i++) {
      g_stereo[2 * i] = g_rec[sent + i];
      g_stereo[2 * i + 1] = g_rec[sent + i];
    }
    i2s.write(g_stereo, n * 4);
    sent += n;
    const size_t pct = sent * 10 / samples;
    if (pct != lastPct) {
      lastPct = pct;
      tft.fillRect(20, 150, (int)(20 * pct), 24, kCyan);
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== A3 回声机器 ===");

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

  g_rec = (int16_t *)heap_caps_malloc(kMaxSamples * 2, MALLOC_CAP_SPIRAM);
  if (g_rec == nullptr) {
    log_e("PSRAM 分配失败");
    while (true) delay(1000);
  }
  Serial.printf("[内存] 录音缓冲 %u 字节 @ PSRAM %p\n", kMaxSamples * 2, g_rec);

  es7210Init();
  es8311Init();

  // 全双工：dout 与 din 同时给，begin 建 TX+RX 一对通道
  i2s.setPins(14, 13, 45, 12, 38);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    log_e("I2S 初始化失败");
    while (true) delay(1000);
  }
  Serial.printf("[I2S] 全双工就绪: %lu Hz 16bit stereo\n", (unsigned long)kSampleRate);

  drawIdle();
}

void loop() {
  if (digitalRead(PIN_BOOT_BTN) != LOW) {
    delay(5);
    return;
  }
  // ---- 按住：录音 + 实时音量条 ----
  Serial.println("[回声] 开始录音（松开停止）");
  drawIdle();  // 顺带清掉上次的进度块
  const size_t got = recordWhileHeld();
  if (got < kSampleRate / 4) {  // 太短当误触
    Serial.println("[回声] 录音太短，忽略");
    drawIdle();
    delay(300);
    return;
  }
  Serial.printf("[回声] 录了 %.2f 秒，开始回放\n", (double)(got / (float)kSampleRate));

  // ---- 松开：回放 ----
  tft.fillRect(0, 190, 320, 24, 0x0000);
  tft.setTextColor(kCyan);
  tft.setTextSize(2);
  tft.setCursor(20, 190);
  tft.print("回放中...");
  playback(got);
  Serial.println("[回声] 完成");
  drawIdle();
  // 等按键松开再继续，避免松手动作立刻触发下一轮
  while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
}
