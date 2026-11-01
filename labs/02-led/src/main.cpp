// 第 02 课：灯光状态机。
// 实战派没有外接 LED 的必要——板载 WS2812 本身就能做全彩演示。这一课学三件事：
// 1. 用 millis() 写非阻塞代码：主循环不被 delay() 卡死，随时能响应新输入。
// 2. 用"状态机"组织复杂行为：一个变量决定当前模式，切换只改变量。
// 3. 亮度的本质：WS2812 靠数据传 8bit 灰度（相当于 PWM 的效果），我们做呼吸灯。
//
// 三种模式循环：呼吸 -> 彩虹流动 -> 单色快闪。每按一次 BOOT 键手动切到下一种。
#include <Arduino.h>

#include "pins.h"

namespace {

enum class Mode : uint8_t { kBreathing, kRainbow, kBlink };
Mode g_mode = Mode::kBreathing;
const char *modeName(Mode m) {
  switch (m) {
    case Mode::kBreathing: return "呼吸";
    case Mode::kRainbow:   return "彩虹流动";
    case Mode::kBlink:     return "快闪";
  }
  return "?";
}

constexpr uint8_t kMaxBrightness = 60;  // 0-255，护眼上限。

// 把 (r,g,b) 乘上 0-255 的亮度系数后输出。
void show(uint8_t r, uint8_t g, uint8_t b, uint8_t scale) {
  rgbLedWrite(PIN_RGB, r * scale / 255 * kMaxBrightness / 255,
                g * scale / 255 * kMaxBrightness / 255,
                b * scale / 255 * kMaxBrightness / 255);
}

// 色轮：位置 0-255 -> RGB。
void wheel(uint8_t pos, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (pos < 85) {
    r = 255 - pos * 3, g = pos * 3, b = 0;
  } else if (pos < 170) {
    pos -= 85;
    r = 0, g = 255 - pos * 3, b = pos * 3;
  } else {
    pos -= 170;
    r = pos * 3, g = 0, b = 255 - pos * 3;
  }
}

// 每种模式一个"渲染函数"：给它当前时间，它算出现在该显示什么。
// 全部用 millis() 推算，不用 delay()——这就是非阻塞。
void renderBreathing(uint32_t now) {
  // 2 秒一个呼吸周期。正弦近似：三角波就够用。
  uint32_t phase = now % 2000;
  uint8_t scale = (phase < 1000) ? phase / 4 : (2000 - phase) / 4;  // 0..250
  show(255, 80, 0, scale);  // 暖橙色呼吸
}

void renderRainbow(uint32_t now) {
  uint8_t r, g, b;
  wheel((now / 5) % 256, r, g, b);  // 约 1.3 秒转一圈
  show(r, g, b, 255);
}

void renderBlink(uint32_t now) {
  bool on = (now / 100) % 2 == 0;  // 100ms 亮、100ms 灭
  show(0, 255, 120, on ? 255 : 0);
}

}  // namespace

void setup() {
  rgbLedWrite(PIN_RGB, 0, 0, 0);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);  // 按下读 LOW；上拉保证悬空时稳定读 HIGH。
  Serial.begin(115200);
  // 等 USB 串口真正连上再打印，避免输出在主机重新枚举期间丢失。
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== 第 02 课：灯光状态机 ===");
  Serial.printf("当前模式：%s。按 BOOT 键切换。\n", modeName(g_mode));
}

void loop() {
  const uint32_t now = millis();

  switch (g_mode) {
    case Mode::kBreathing: renderBreathing(now); break;
    case Mode::kRainbow:   renderRainbow(now);   break;
    case Mode::kBlink:     renderBlink(now);     break;
  }

  // 按键轮询：按下（LOW）时切模式。简单消抖：切完强制等 300ms。
  // 第 04 课会把按键做成中断 + 正经消抖，这里先用最朴素的写法。
  static uint32_t lastPress = 0;
  if (digitalRead(PIN_BOOT_BTN) == LOW && now - lastPress > 300) {
    lastPress = now;
    g_mode = static_cast<Mode>((static_cast<uint8_t>(g_mode) + 1) % 3);
    log_i("切换模式 -> %s", modeName(g_mode));
  }
}
