// E2 深度睡眠（Deep Sleep）。
//
// 这一课学：
// 1. 深度睡眠是什么：CPU、RAM、Wi-Fi、蓝牙全部断电，只剩 RTC 域活着（几个 µA，
//    比运行时的几十 mA 低了一万倍）。电池设备（手环、传感器）能扛一年全靠它。
// 2. 唤醒源：深睡中能"叫醒"芯片的事件。本课演示两种：
//      timer  —— 定时器到点（每 10 秒自己醒来）
//      ext0   —— 某个 RTC GPIO 到指定电平（BOOT 键按下，低电平唤醒）
//    多个唤醒源可以同时使能，谁先到点算谁的。
// 3. RTC_DATA_ATTR：放 RTC 慢速内存的变量。深睡断的是主 RAM，这块内存
//    有 RTC 电源撑着，醒来数据还在——跨睡眠记事的唯一地方。
// 4. 醒来 ≠ 重启了吗？是。深度睡眠醒来从 setup() 重新跑，
//    区别在 esp_sleep_get_wakeup_cause() 能告诉你"我是被谁叫醒的"。
//
// ⚠️ 注意：深度睡眠后 USB 串口会断（USB-Serial-JTAG 掉电），醒来重连时
//    监视器可能需要重新打开。烧录也必须手动复位。
#include <Arduino.h>
#include "esp_sleep.h"

#include "pins.h"

namespace {

constexpr uint32_t kSleepUs = 10ULL * 1000 * 1000;  // timer 唤醒：10 秒
constexpr uint8_t kBrightness = 60;

// RTC 内存：跨深度睡眠存活。普通全局变量醒来会被重置成上电初值。
RTC_DATA_ATTR uint32_t s_wakeCount = 0;
RTC_DATA_ATTR uint32_t s_sleepCount = 0;

const char *wakeCauseText(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "定时器到点";
    case ESP_SLEEP_WAKEUP_EXT0: return "BOOT 键按下（ext0）";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "上电/复位（不是从深睡醒来）";
    default: return "其他";
  }
}

// 进睡前 LED 呼吸 3 秒：一个从暗到亮再暗的渐变，用色温不变的暖白
void breatheLed() {
  Serial.println("[LED] 呼吸 3 秒后入睡...");
  // 亮 -> 暗 -> 亮一个来回
  for (int pass = 0; pass < 2; pass++) {
    for (int v = 0; v <= 255; v += 4) {
      const uint8_t x = pass == 0 ? v : 255 - v;
      rgbLedWrite(PIN_RGB, x * kBrightness / 255, x * kBrightness / 255, 0);  // 暖黄
      delay(6);
    }
  }
  rgbLedWrite(PIN_RGB, 0, 0, 0);  // 熄灯再睡，别留个长明灯耗电
}

void enterDeepSleep() {
  Serial.println("[睡眠] 10 秒后 timer 唤醒；期间按住 BOOT 键随时 ext0 唤醒");
  Serial.flush();  // 等串口发完再断电，不然最后几行日志会丢

  // 清掉本次的唤醒源，重新配置下一轮的
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(kSleepUs);
  // ext0：GPIO0 电平变低（按下 BOOT）就醒。ext0 只支持 RTC 域 GPIO，GPIO0 在。
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(PIN_BOOT_BTN), 0);

  esp_deep_sleep_start();  // 睡下去，从这里不再返回；醒来从 setup() 从头跑
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== E2 深度睡眠 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  s_wakeCount++;
  Serial.printf("[唤醒] 原因: %s，第 %lu 次\n", wakeCauseText(cause),
                static_cast<unsigned long>(s_wakeCount));
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[提示] 你按了 BOOT！5 秒后继续入睡（这 5 秒抓紧重新打开串口监视器）");
    delay(5000);
  }

  // 上一轮睡前已使能 timer，正常走到这里说明 timer 到点了
  s_sleepCount = s_wakeCount;  // 睡的次数 = 醒的次数 - 1（首次上电不算）
  breatheLed();
  enterDeepSleep();
}

void loop() {
  // 永远到不了这里：setup() 末尾就睡下去了。深睡设备的 loop 都是空的。
}
