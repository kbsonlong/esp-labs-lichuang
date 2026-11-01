// 第 04 课：把按键做对。
// 第 02 课轮询按键凑合能用，但真实产品不能这么写：主循环一忙就可能漏按。
// 这一课升级成教科书做法：
// 1. 中断：按键按下由硬件瞬间捕获（ISR 里只做最少的事，打时间戳就走）。
// 2. 消抖：金属触点闭合时会"弹跳"几毫秒——一次按压产生多个边沿，用时间去抖。
// 3. 长按/短按：按下时刻 + 松开时刻一对比就出来了。
//
// 现象：短按换 LED 颜色，长按（>=800ms）开关背光，屏幕实时显示事件流。
#include <Arduino.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#include "pins.h"

namespace {

SPIClass spi(FSPI);
Adafruit_ST7789 tft(&spi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

// ---- 中断与 loop 之间共享的变量必须是 volatile ----
volatile uint32_t g_pressedAt = 0;     // 按下时刻（ISR 记录）
volatile bool g_fell = false;          // 检测到下降沿（按下）
volatile bool g_rose = false;          // 检测到上升沿（松开）

// ISR 里绝不能 delay/Serial/锁——只记录事实，判断留给 loop。
void IRAM_ATTR onButtonChange() {
  const uint32_t now = millis();
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    g_pressedAt = now;
    g_fell = true;
  } else {
    g_rose = true;
  }
}

constexpr uint32_t kDebounceMs = 30;    // 触点弹跳通常 <10ms，30ms 留足余量
constexpr uint32_t kLongPressMs = 800;  // 按住超过这个时长算长按

uint8_t g_colorIdx = 0;
const uint8_t kColors[][3] = {{255, 40, 40}, {40, 255, 60}, {60, 80, 255}, {255, 220, 0}};
constexpr uint8_t kNumColors = sizeof(kColors) / sizeof(kColors[0]);

void showColor(uint8_t idx) {
  neopixelWrite(PIN_RGB, kColors[idx][0], kColors[idx][1], kColors[idx][2]);
}

bool g_backlightOn = true;
uint16_t g_pressCount = 0;

void logEvent(const char *fmt, ...) {  // 屏幕滚动事件流（简化版：只显示最近 5 条）
  static char lines[5][40];
  static uint8_t head = 0;
  va_list args;
  va_start(args, fmt);
  vsnprintf(lines[head], sizeof(lines[0]), fmt, args);
  va_end(args);
  head = (head + 1) % 5;

  tft.fillRect(0, 96, tft.width(), tft.height() - 96, ST77XX_BLACK);
  for (uint8_t i = 0; i < 5; i++) {
    const uint8_t line = (head + i) % 5;
    tft.setCursor(8, 100 + i * 24);
    tft.setTextColor(i == 4 ? ST77XX_GREEN : ST77XX_DARKGREY);
    tft.setTextSize(2);
    tft.print(lines[line]);
  }
}

}  // namespace

void setup() {
  neopixelWrite(PIN_RGB, 0, 0, 0);
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== 第 04 课：把按键做对 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  // CHANGE = 上升沿和下降沿都触发中断。
  attachInterrupt(digitalPinToInterrupt(PIN_BOOT_BTN), onButtonChange, CHANGE);

  // 背光走 LEDC PWM（低电平点亮）。初始化为 30% 亮度。
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 175);

  spi.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, -1);
  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print("按键实验");
  tft.setCursor(8, 40);
  tft.print("短按: 换色  长按: 背光");

  showColor(0);
}

void loop() {
  // ---- 下降沿：确认"真的按下"（消抖）----
  static uint32_t pressAt = 0;
  static bool pressed = false;
  if (g_fell) {
    g_fell = false;
    const uint32_t t = g_pressedAt;
    if (t - pressAt > kDebounceMs) {  // 距上次"按下"太近的边沿 = 弹跳，丢弃
      pressAt = t;
      pressed = true;
      g_pressCount++;
      log_i("按下 #%u", g_pressCount);
    }
  }

  // ---- 上升沿：松开，结算长按/短按 ----
  if (g_rose) {
    g_rose = false;
    if (pressed) {
      pressed = false;
      const uint32_t held = millis() - pressAt;
      if (held >= kLongPressMs) {
        g_backlightOn = !g_backlightOn;
        ledcWrite(PIN_TFT_BL, g_backlightOn ? 175 : 255);  // 倒极性：175≈30% 亮度
        logEvent("长按 %lums 背光%s", static_cast<unsigned long>(held), g_backlightOn ? "开" : "关");
      } else {
        g_colorIdx = (g_colorIdx + 1) % kNumColors;
        showColor(g_colorIdx);
        logEvent("短按 -> 颜色 %u", g_colorIdx + 1);
      }
    }
  }
}
