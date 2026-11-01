// A1 电子木鱼。
//
// 这一课学：
// 1. 新 I2S API（Arduino-ESP32 3.x 的 ESP_I2S.h / I2SClass）：setPins + begin，
//    告别旧版 i2s_driver_install 那套结构体大礼包。
// 2. 音频 DAC 不是"接上就能响"：ES8311 要先走 I2C 写一堆寄存器（时钟、SDP、音量、
//    解除静音）才开始干活。这是所有音频芯片的标准打开方式。
// 3. 硬件真相：喇叭前面有一级功放（PA），PA_EN 挂在 PCA9557 扩展器的 bit1 上——
//    initIoExpander() 最后写的 output=0x02 正好同时做到"屏 CS 选中(bit0=0)"和
//    "PA_EN=1(bit1=1)"，所以本课直接复用 W 系列的初始化，一个函数两头兼顾。
// 4. 正弦波 + 包络线 = 最朴素的合成器：起音 5ms、指数衰减，就是"木鱼哒"的短促感。
//
// 玩法：每按一次 BOOT，音阶前进一个音并播放（do-re-mi-fa-sol-la-si-do'，循环），
// 屏幕显示当前音符名。
//
// ⚠️ 本课代码未经真机验证：ES8311 寄存器序列按公开驱动资料 best-effort 抄写（见 UNVERIFIED 注释）。
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

constexpr uint16_t kCyan = 0x07FF, kYellow = 0xFFE0, kGreen = 0x07E0, kGrey = 0x7BEF;

// PCA9557 IO 扩展器：bit0 = 屏 CS（低选中），bit1 = PA_EN 功放开关（1 = 开）。
// 用屏、出声之前都必须跑这一段，否则屏全黑 / 喇叭无声。
void initIoExpander() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(0x19);
  Wire.write(0x01);  // output 寄存器
  Wire.write(0x03);  // 先全拉高（CS 释放）
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x03);  // config 寄存器
  Wire.write(0xF8);  // 低 3 位输出，其余输入
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  Wire.write(0x02);  // 最终态 0x02：bit0=0 屏 CS 选中，bit1=1 PA_EN=1
  Wire.endTransmission();
}

// ---- ES8311 音频 DAC（I2C 地址 0x18）----
// ⚠️ UNVERIFIED：以下寄存器序列按公开驱动资料（esp-adf / esp-bsp 的 es8311 驱动
// 与数据手册）best-effort 抄写，未逐步真机验证。若无声：
// 1) 用 05-i2c-scan 确认 0x18 在线；2) 对照 ES8311 数据手册逐条核寄存器值。
constexpr uint8_t kEs8311Addr = 0x18;

void es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kEs8311Addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void es8311Init() {
  es8311Write(0x00, 0x80);  // UNVERIFIED: 软复位（bit7=1）
  delay(10);
  es8311Write(0x00, 0x00);  // UNVERIFIED: 解除复位，时钟走内部
  es8311Write(0x01, 0x3F);  // UNVERIFIED: 时钟管理，先关分频输出
  es8311Write(0x02, 0x00);  // UNVERIFIED: 时钟管理：BCLK/MCLK 关系
  es8311Write(0x03, 0x10);  // UNVERIFIED: ADC 采样率档位（我们只用 DAC，但一并配上）
  es8311Write(0x04, 0x10);  // UNVERIFIED: DAC 采样率档位，MCLK = 256fs
  es8311Write(0x0D, 0x01);  // UNVERIFIED: 系统寄存器：时钟/数据路径
  es8311Write(0x0E, 0x02);  // UNVERIFIED: 系统寄存器：内部参考与偏置
  es8311Write(0x12, 0x00);  // UNVERIFIED: DAC 输入选择 = I2S 数字输入
  es8311Write(0x13, 0x10);  // UNVERIFIED: DAC 输出通道配置
  es8311Write(0x14, 0x0A);  // UNVERIFIED: DAC 模拟段设置
  es8311Write(0x32, 0xBF);  // UNVERIFIED: DAC 数字音量（ datasheet 里 0xBF ≈ 0dB）
  es8311Write(0x37, 0x08);  // UNVERIFIED: DAC 解除静音
  Serial.println("[ES8311] I2C 配置完成（寄存器序列 UNVERIFIED）");
}

// ---- 新 I2S API：一个 I2SClass 搞定 ----
constexpr uint32_t kSampleRate = 16000;

I2SClass i2s;

// ---- 音阶表：音符名 + 频率（C 大调八度）----
struct Note {
  const char *name;
  float freq;
};
constexpr Note kScale[] = {
    {"do", 261.63f}, {"re", 293.66f}, {"mi", 329.63f}, {"fa", 349.23f},
    {"sol", 392.00f}, {"la", 440.00f}, {"si", 493.88f}, {"do+", 523.25f},
};
constexpr int kScaleLen = sizeof(kScale) / sizeof(kScale[0]);
int g_noteIdx = 0;

// 播放一个"木鱼音"：正弦波 + 5ms 起音 + 指数衰减包络，约 0.45 秒。
// 一小块一小块生成再写进 I2S，不用预先开大缓冲。
void playWoodfish(float freq) {
  constexpr int kFrames = (int)(kSampleRate * 0.45f);  // 0.45 秒
  constexpr int kChunk = 128;                          // 每次生成 128 帧
  const float kAttack = 0.005f * kSampleRate;          // 5ms 起音
  for (int f = 0; f < kFrames; f += kChunk) {
    const int n = (f + kChunk <= kFrames) ? kChunk : (kFrames - f);
    int16_t stereo[kChunk * 2];  // 16bit 立体声交错：L R L R ...
    for (int i = 0; i < n; i++) {
      const int k = f + i;
      const float t = (float)k / kSampleRate;
      // 包络：起音快速升到 1，之后指数衰减（衰减系数决定"短促感"）
      float env = (k < kAttack) ? ((float)k / kAttack)
                                : expf(-6.0f * (float)(k - kAttack) / (kFrames - kAttack));
      const int16_t s = (int16_t)(sinf(2.0f * PI * freq * t) * 12000.0f * env);
      stereo[2 * i] = s;      // 左声道
      stereo[2 * i + 1] = s;  // 右声道（单音源，两路同相）
    }
    i2s.write(stereo, n * 4);  // 4 字节/帧（2 声道 x 16bit）
  }
}

void drawNote() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("A1 电子木鱼");
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 56);
  tft.printf("按 BOOT 敲一下  (%d/%d)", g_noteIdx + 1, kScaleLen);
  // 大字音符名
  tft.setTextColor(kCyan);
  tft.setTextSize(8);
  tft.setCursor(40, 110);
  tft.print(kScale[g_noteIdx].name);
  tft.setTextColor(kGreen);
  tft.setTextSize(2);
  tft.setCursor(8, 210);
  tft.printf("%.2f Hz", (double)kScale[g_noteIdx].freq);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== A1 电子木鱼 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();  // 屏 CS + PA_EN 一次到位

  // 背光：倒极性 PWM，255 = 灭，0 = 最亮
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

  es8311Init();

  // 新 I2S API：先 setPins 再 begin。参数顺序 (bclk, ws, dout, din, mclk)，
  // 本课只放音，din = -1。ES8311 是从机，ESP32 当主机供时钟。
  i2s.setPins(14, 13, 45, -1, 38);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    log_e("I2S 初始化失败");
    tft.fillScreen(0x0000);
    tft.setTextColor(0xF800);
    tft.setTextSize(2);
    tft.setCursor(8, 110);
    tft.print("I2S init FAILED");
    while (true) delay(1000);
  }
  Serial.printf("[I2S] TX 就绪: %lu Hz 16bit stereo\n", (unsigned long)kSampleRate);

  drawNote();
}

void loop() {
  // BOOT 按键边沿检测（带 20ms 消抖）：按下 -> 音阶前进一格并敲一记
  static bool lastPressed = false;
  static uint32_t lastEdge = 0;
  const bool pressed = (digitalRead(PIN_BOOT_BTN) == LOW);
  if (pressed != lastPressed && millis() - lastEdge > 20) {
    lastEdge = millis();
    lastPressed = pressed;
    if (pressed) {
      Serial.printf("[木鱼] %s (%.2f Hz)\n", kScale[g_noteIdx].name,
                    (double)kScale[g_noteIdx].freq);
      playWoodfish(kScale[g_noteIdx].freq);
      g_noteIdx = (g_noteIdx + 1) % kScaleLen;  // 循环音阶
      drawNote();
    }
  }
  delay(5);
}
