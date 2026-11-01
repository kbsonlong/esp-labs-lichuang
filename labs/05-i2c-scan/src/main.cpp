// 第 05 课：I2C 总线扫描。
// 实战派上所有"智能"外设都挂在同一条 I2C 总线（SDA=1, SCL=2）上：
//   PCA9557  IO 扩展器（管音频功放开关等）     7bit 地址 0x19
//   ES8311   音频 DAC（扬声器输出）            0x18
//   ES7210   音频 ADC（麦克风输入）            0x41
//   FT5x06   触摸控制器（屏上那层玻璃）        0x38
// I2C 只用两根线就能挂一堆设备——每个设备靠 7bit 地址区分。
//
// 扫描原理：遍历 8-119 的地址，发起一次"写寻址"；总线上有设备应答（ACK 拉低 SDA）
// 就说明这个地址有人。0-7 和 120-127 是协议保留段，不扫。
//
// 现象：串口每 5 秒打一张"在线设备表"。这是后面音频篇/触摸篇的地基。
#include <Arduino.h>
#include <Wire.h>

#include "pins.h"

namespace {

// 已知设备小词典：地址 -> 它是谁。（器件清单来自立创官方 wiki）
struct Device {
  uint8_t addr;
  const char *name;
};
constexpr Device kKnown[] = {
    {0x18, "ES8311 音频DAC"},
    {0x19, "PCA9557 IO扩展器"},
    {0x38, "FT6336 触摸"},
    {0x41, "ES7210 音频ADC"},
    {0x6A, "QMI8658 姿态传感器"},
};

const char *describe(uint8_t addr) {
  for (const auto &d : kKnown) {
    if (d.addr == addr) return d.name;
  }
  return "未知设备";
}

// 探测一个地址：beginTransmission + endTransmission 的返回值就是应答情况。
// 0 = 有设备应答；2 = NACK（没人）；其他 = 总线错误。
bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void scanBus() {
  Serial.println();
  Serial.println("+-------- I2C 在线设备 --------+");
  uint8_t found = 0;
  for (uint8_t addr = 8; addr < 120; addr++) {
    if (probe(addr)) {
      found++;
      Serial.printf("| 0x%02X  %s\n", addr, describe(addr));
    }
  }
  Serial.printf("+ 共 %d 个设备应答 ----------+\n", found);
  if (found == 0) {
    log_e("总线上没有任何应答！检查 SDA/SCL 接线或上拉。");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // 等 USB 串口真正连上再打印，避免输出在主机重新枚举期间丢失。
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== 第 05 课：I2C 总线扫描 ===");

  // Arduino 的 Wire 默认引脚不是 1/2，必须显式指定。
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);  // 400kHz 快速模式；板载走线短，跑得动。
}

void loop() {
  static uint32_t lastScan = 0;
  if (millis() - lastScan >= 5000) {  // 每 5 秒扫一次，顺便演示 loop 里的定时任务写法
    lastScan = millis();
    scanBus();
  }
}
