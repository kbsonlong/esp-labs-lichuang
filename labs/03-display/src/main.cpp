// 第 03 课：点亮板载屏幕。
// 实战派带一块 2.0" IPS 屏：ST7789 驱动、SPI 接口、320x240。全部焊在板上，零接线。
//
// 这一课学：
// 1. SPI 四条线的分工：SCLK 时钟、MOSI 数据、DC 命令/数据切换、背光。
//    （CS 在这块板上直接接地，所以库里传 -1 = 永远选中。）
// 2. RGB565：一个像素 16bit——红 5 位、绿 6 位、蓝 5 位（人眼对绿色最敏感，所以多 1 位）。
// 3. 背光不是 SPI 的一部分：GPIO42 单独 PWM 调光，且这块板低电平点亮（倒极性）。
//
// 现象：彩条 + 文字 + 每秒刷新的帧率计。按 BOOT 键切换亮度。
#include <Arduino.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#include "pins.h"

namespace {

SPIClass spi(FSPI);           // S3 的原生 SPI2（FSPI），速度最快的硬件外设。
Adafruit_ST7789 tft(&spi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

constexpr uint8_t kBacklightCh = 0;     // LEDC 通道随便挑一个。
constexpr uint32_t kBacklightFreq = 5000;
constexpr uint8_t kBacklightRes = 8;    // 8bit：0-255

// 亮度 0-255（人话方向：越大越亮），内部翻转成倒极性 PWM。
void setBacklight(uint8_t brightness) {
  ledcWrite(PIN_TFT_BL, 255 - brightness);
}

// RGB565 打包：红取高 5 位、绿中 6 位、蓝低 5 位。
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void drawColorBars() {
  // 6 条竖向彩条，占屏幕上半部。
  const uint16_t colors[] = {rgb565(255, 0, 0), rgb565(0, 255, 0), rgb565(0, 0, 255),
                             rgb565(255, 255, 0), rgb565(0, 255, 255), rgb565(255, 0, 255)};
  const int16_t barW = tft.width() / 6;
  for (int i = 0; i < 6; i++) {
    tft.fillRect(i * barW, 0, barW, tft.height() / 2, colors[i]);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== 第 03 课：点亮屏幕 ===");

  // 背光先点亮，能看到屏从黑到亮的过程。
  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, kBacklightFreq, kBacklightRes);
  setBacklight(80);

  // 显式把 SPI 走到实战派的引脚上（MOSI=40, SCLK=41；MISO 屏不需要）。
  spi.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, -1);
  tft.init(TFT_WIDTH, TFT_HEIGHT);  // 原生 240x320
  tft.setRotation(1);               // 横屏：320 宽 x 240 高
  tft.fillScreen(ST77XX_BLACK);

  log_i("屏幕就绪：%dx%d", tft.width(), tft.height());
}

void loop() {
  static uint32_t frames = 0;
  static uint32_t lastFpsAt = 0;
  static uint8_t brightnessLevels[] = {30, 80, 160, 255};
  static uint8_t brightnessIdx = 1;
  static uint32_t lastPress = 0;

  // 每帧：重画彩条 + 文字。整屏 fill 都能跑出高帧率，这就是 DMA 加持的硬件 SPI。
  drawColorBars();
  tft.fillRect(0, tft.height() / 2, tft.width(), tft.height() / 2, ST77XX_BLACK);
  tft.setCursor(8, tft.height() / 2 + 8);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.println("Hello, lichuang!");
  tft.setCursor(8, tft.height() / 2 + 40);
  tft.setTextColor(ST77XX_YELLOW);
  tft.printf("uptime %lus", static_cast<unsigned long>(millis() / 1000));

  frames++;
  if (millis() - lastFpsAt >= 1000) {
    log_i("FPS: %lu", static_cast<unsigned long>(frames));
    frames = 0;
    lastFpsAt = millis();
  }

  // BOOT 键：循环切背光亮度。
  if (digitalRead(PIN_BOOT_BTN) == LOW && millis() - lastPress > 300) {
    lastPress = millis();
    brightnessIdx = (brightnessIdx + 1) % 4;
    setBacklight(brightnessLevels[brightnessIdx]);
    log_i("背光 -> %d", brightnessLevels[brightnessIdx]);
  }
}
