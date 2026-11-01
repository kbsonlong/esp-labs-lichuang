// S2 触摸画板：I2C 读 FT6336 触摸控制器，手指在屏上画彩色线条。
//
// 这一课学：
// 1. 触摸控制器的寄存器协议：FT6336 是"轮询式"的——不用中断也能干活，
//    周期性读 0x02（触点数），>0 再读 0x03-0x06 拿触点坐标。
// 2. 12 位坐标的拆装：X/Y 各拆在两个寄存器的高低位里（XH 低 4 位是 X[11:8]），
//    拼回去就是 0-234 / 0-179 的面板原生坐标。
// 3. 坐标系变换：触摸面板和屏幕的安装方向相差 90°，
//    把触摸坐标做一次旋转 + 缩放映射到 320x240 横屏。
// 4. 断笔逻辑：抬手（触点数=0）时清掉"上一个点"，下次落下从新点起笔，
//    否则会画出一条横跨屏幕的直线。
//
// FT6336 寄存器来自 FocalTech FT6X36 数据手册（FT6236/FT6336/FT6436 同系列）。
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// ---- FT6336 寄存器（FT6X36 数据手册 Working Mode Register Map）----
constexpr uint8_t kFtAddr = 0x38;    // 板上实测扫描确认
constexpr uint8_t kRegTdStatus = 0x02;  // 触点数（bit[3:0]）
constexpr uint8_t kRegP1Xh = 0x03;      // 触点1：XH（[3:0]=X[11:8]，[7:6]=事件标志）
constexpr uint8_t kRegP1Xl = 0x04;      // 触点1：XL（X[7:0]）
constexpr uint8_t kRegP1Yh = 0x05;      // 触点1：YH（[3:0]=Y[11:8]，[7:4]=触点ID）
constexpr uint8_t kRegP1Yl = 0x06;      // 触点1：YL（Y[7:0]）
constexpr int kTouchMaxX = 234;         // 面板原生坐标范围（竖屏安装）
constexpr int kTouchMaxY = 179;

// ---- 屏驱动（同 W2，真机验证过的模板）----
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
  Wire.write(0x02);
  Wire.endTransmission();
}

// ---- FT6336 读原语 ----
bool readRegs(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(kFtAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(kFtAddr), static_cast<int>(n)) != static_cast<int>(n)) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

struct TouchPoint {
  bool pressed = false;
  int x = 0, y = 0;  // 屏幕坐标（320x240 横屏）
};

// 读触点1，并把面板原生坐标（约 234x179 竖屏）映射到 320x240 横屏。
// 面板相对屏幕转了 90° 安装：屏幕横向 X ≈ 面板 Y，屏幕纵向 Y ≈ 面板 X 的反向。
// UNVERIFIED: 待真机确认——若画的线与手指走向对不上（上下颠或左右颠），
// 按下面注释里的四种组合换轴/翻转即可：
//   sx = ty*320/179 或 (179-ty)*320/179
//   sy = tx*240/234 或 (234-tx)*240/234
bool readTouch(TouchPoint &t) {
  uint8_t status = 0;
  if (!readRegs(kRegTdStatus, &status, 1)) return false;
  const int n = status & 0x0F;
  if (n < 1 || n > 2) {  // 0 = 无触点；>2 = 异常值（手册说最大 2 点）
    t.pressed = false;
    return true;
  }
  uint8_t p[4];
  if (!readRegs(kRegP1Xh, p, sizeof(p))) return false;
  const int rawX = ((p[0] & 0x0F) << 8) | p[1];  // 12 位拼接
  const int rawY = ((p[2] & 0x0F) << 8) | p[3];

  // 旋转 + 缩放：面板短边(179)对应屏宽 320，面板长边(234)对应屏高 240
  t.x = (rawY * 320) / kTouchMaxY;
  t.y = (rawX * 240) / kTouchMaxX;
  // UNVERIFIED: 方向翻转待真机确认，常见两种：
  //   t.x = ((kTouchMaxY - rawY) * 320) / kTouchMaxY;
  //   t.y = ((kTouchMaxX - rawX) * 240) / kTouchMaxX;
  t.x = min(max(t.x, 0), 319);
  t.y = min(max(t.y, 0), 239);
  t.pressed = true;
  return true;
}

// 彩色线条：色相环循环。RGB565 手动算，不引入额外库。
uint16_t rainbow(uint16_t step) {
  const uint8_t hue = (step * 8) & 0xFF;  // 每点前进一点色相
  const uint8_t region = hue / 43;        // 6 个 60° 扇区
  const uint8_t rem = (hue - region * 43) * 6;
  uint8_t r = 0, g = 0, b = 0;
  switch (region) {
    case 0: r = 255; g = rem; break;
    case 1: g = 255; b = 255 - rem; break;
    case 2: g = 255; r = rem; break;
    case 3: r = 255; b = 255 - rem; break;
    case 4: r = 255; b = rem; break;
    default: b = 255; g = rem; break;
  }
  return tft.color565(r, g, b);
}

void drawTitleBar() {
  tft.fillRect(0, 0, 320, 18, 0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print("S2 touch paint | BOOT=clear");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== S2 触摸画板 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);
  drawTitleBar();

  // 确认触摸控制器在线
  uint8_t status = 0;
  if (!readRegs(kRegTdStatus, &status, 1)) {
    Serial.println("[FT6336] I2C 无应答：触摸不可用（画板功能失效）");
    tft.setTextColor(kYellow);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.print("FT6336 not found");
  } else {
    Serial.printf("[FT6336] 在线，TD_STATUS=0x%02X\n", status);
  }
}

void loop() {
  static TouchPoint last;      // 上一帧触点（画线段用）
  static uint16_t colorStep = 0;
  static uint32_t lastClearCheck = 0;

  // BOOT 键清屏（消抖：50ms 内只清一次）
  static uint32_t pressAt = 0;
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (pressAt == 0) pressAt = millis();
    else if (millis() - pressAt > 50 && millis() - lastClearCheck > 300) {
      lastClearCheck = millis();
      tft.fillScreen(0x0000);
      drawTitleBar();
      last.pressed = false;  // 断笔
      Serial.println("[画板] 已清屏");
    }
  } else {
    pressAt = 0;
  }

  TouchPoint t;
  if (!readTouch(t)) {
    Serial.println("[FT6336] 读失败");
    delay(200);
    return;
  }

  if (t.pressed) {
    if (last.pressed) {
      // 有上一点：连线。线宽 2 用画粗线的土办法：画两次错开 1px
      const uint16_t c = rainbow(colorStep++);
      tft.drawLine(last.x, last.y, t.x, t.y, c);
      tft.drawLine(last.x + 1, last.y, t.x + 1, t.y, c);
      tft.drawLine(last.x, last.y + 1, t.x, t.y + 1, c);
    }
    last = t;  // 记下本帧位置，下一帧从这里连线
  } else {
    last.pressed = false;  // 抬手断笔：下次落下从新点起笔
  }

  delay(10);  // FT6336 输出速率约 60Hz，10ms 轮询刚好跟得上
}
