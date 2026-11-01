// 第 01 课：认识板子。
// 1. 向串口监视器打印芯片的"身份卡"，确认它真是 16MB flash + 8MB PSRAM 的 N16R8。
// 2. 板载 RGB LED（WS2812）转色轮：程序真的在跑的视觉证据。
//
// 每个 Arduino 程序都有两个函数：
//   setup() 上电（或复位）后跑一次。
//   loop()  setup() 之后永远循环，能跑多快跑多快。
#include <Arduino.h>

#include "pins.h"

namespace {

constexpr uint8_t kBrightness = 40;      // 0-255。WS2812 很刺眼，40 护眼。
constexpr uint32_t kHeartbeatMs = 5000;  // 每隔多久打一行"还活着"。

// 色轮上的一个位置（0-255）换算成红/绿/蓝分量。
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

void printIdentityCard() {
  Serial.println();
  Serial.println("+---------- ESP32 身份卡 ----------+");
  Serial.printf("| 芯片     %s rev %d, %d 核 @ %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(),
                static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  Serial.printf("| Flash    %lu MB   (N16: 应为 16)\n",
                static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf("| PSRAM    %lu MB   (R8: 应为 ~8)\n",
                static_cast<unsigned long>(ESP.getPsramSize() / (1024 * 1024)));
  Serial.printf("| 空闲堆栈 %lu KB\n", static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  // eFuse 里 MAC 最低字节在前，按字节 0 在先打印，和 esptool 显示的一致。
  const uint64_t mac = ESP.getEfuseMac();
  Serial.print("| MAC      ");
  for (int i = 0; i < 6; i++) {
    Serial.printf(i < 5 ? "%02X:" : "%02X\n", static_cast<unsigned>((mac >> (8 * i)) & 0xFF));
  }
  Serial.printf("| SDK      %s\n", ESP.getSdkVersion());
  Serial.println("+----------------------------------+");

  // 硬件与预期不符就大声报警。
  if (ESP.getPsramSize() == 0) {
    log_e("没找到 PSRAM。检查 platformio.ini 的 board 设置（必须是 R8 板）。");
  }
  if (ESP.getFlashChipSize() < 16UL * 1024 * 1024) {
    log_e("Flash 不足 16MB。检查 board 设置。");
  }
}

}  // namespace

void setup() {
  // WS2812 上电瞬间可能闪一下杂色，先固定为灭。
  rgbLedWrite(PIN_RGB, 0, 0, 0);

  Serial.begin(115200);
  Serial.println();
  Serial.println("=== 第 01 课：认识板子 ===");
  Serial.println("打开监视器后按 <回车>，打印芯片身份卡。");
}

void loop() {
  // 串口收到回车 -> 重打身份卡。S3 原生 USB 的输出在主机重新枚举前会丢，
  // 所以"按回车重打"比只在 setup 里打一次可靠——任何时候打开监视器都能拿到。
  static uint32_t lastCardAt = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if ((c == '\n' || c == '\r') && millis() - lastCardAt > 200) {
      // 200ms 去抖：一次回车常被终端拆成 \r\n 两个字符，只算一次。
      lastCardAt = millis();
      printIdentityCard();
    }
  }

  static uint32_t lastBeat = 0;
  static uint8_t pos = 0;

  // 每 50ms 在色轮上走一步：约 12.8 秒转完一圈。
  static uint32_t lastStep = 0;
  if (millis() - lastStep >= 50) {
    lastStep = millis();
    pos++;
    uint8_t r, g, b;
    wheel(pos, r, g, b);
    rgbLedWrite(PIN_RGB, r * kBrightness / 255, g * kBrightness / 255, b * kBrightness / 255);
  }

  // 心跳日志：证明 loop 还在跑，也方便确认串口没断。
  if (millis() - lastBeat >= kHeartbeatMs) {
    lastBeat = millis();
    log_i("还活着，millis()=%lu", static_cast<unsigned long>(millis()));
  }
}
