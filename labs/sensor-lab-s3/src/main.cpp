// S3 滚动小球：加速度计倾斜控制 + 触摸点击，60Hz 固定步进物理循环。
//
// 这一课学：
// 1. 固定步进物理循环：物理积分必须用"固定 dt"而不是"上一帧实际耗时"——
//    可变 dt 会放大抖动、反弹行为随帧率漂移。这里 60Hz，累计 micros() 到
//    16667us 才走一步，渲染快慢不影响物理节奏。
// 2. 输入/物理/渲染分层：readInput() 只采集不改状态，stepPhysics(dt) 只改
//    状态不画屏幕，render() 只画屏幕不读传感器。三层各自可独立测试。
// 3. 简单刚体碰撞：球撞墙 = 位置钳位 + 速度反向乘弹性系数（restitution）。
// 4. 传感器 -> 游戏输入：加速度计的水平分量直接当屏幕坐标系的重力向量。
//
// QMI8658/FT6336 寄存器细节同 S1/S2（公开数据手册）。
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// ---- QMI8658（同 S1）----
constexpr uint8_t kQmiAddr = 0x6A;
constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kWhoAmIValue = 0xFD;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl7 = 0x08;
constexpr uint8_t kRegAccelXLow = 0x35;
constexpr float kLsbPerG = 32768.0f / 4.0f;  // ±4g 量程

// ---- FT6336（同 S2）----
constexpr uint8_t kFtAddr = 0x38;
constexpr uint8_t kRegTdStatus = 0x02;
constexpr uint8_t kRegP1Xh = 0x03;
constexpr int kTouchMaxX = 234;
constexpr int kTouchMaxY = 179;

constexpr int kScreenW = 320, kScreenH = 240;

// ---- 屏驱动（同 W2 模板）----
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

constexpr uint16_t kCyan = 0x07FF, kYellow = 0xFFE0, kGrey = 0x7BEF, kRed = 0xF800;

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

// ---- I2C 原语（两个芯片共用一条总线）----
bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(n)) != static_cast<int>(n)) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// ================= 输入层 =================

struct InputState {
  bool sensorOk = false;
  float ax = 0, ay = 0, az = 0;  // 单位 g
  bool tap = false;              // 本帧有新点击
  int tapX = 0, tapY = 0;        // 屏幕坐标
};

bool initSensors() {
  uint8_t id = 0;
  if (!readRegs(kQmiAddr, kRegWhoAmI, &id, 1) || id != kWhoAmIValue) {
    Serial.printf("[QMI8658] WHO_AM_I 校验失败（读到 0x%02X，期望 0x%02X）\n", id, kWhoAmIValue);
    return false;
  }
  Serial.printf("[QMI8658] WHO_AM_I=0x%02X OK\n", id);
  writeReg(kQmiAddr, kRegCtrl1, 0x60);
  writeReg(kQmiAddr, kRegCtrl2, 0x23);  // 加速度计 ±4g
  writeReg(kQmiAddr, kRegCtrl7, 0x02);  // aEN
  uint8_t st = 0;
  const bool touchOk = readRegs(kFtAddr, kRegTdStatus, &st, 1);
  Serial.printf("[FT6336] %s（TD_STATUS=0x%02X）\n", touchOk ? "在线" : "不在线", st);
  delay(10);
  return true;
}

// UNVERIFIED: 加速度轴 -> 屏幕轴的映射方向待真机确认（同 S1 气泡方向问题）：
// 若球滚向错误的一侧，调整 ax/ay 的正负号即可。
InputState readInput() {
  InputState in;
  uint8_t buf[6];
  if (readRegs(kQmiAddr, kRegAccelXLow, buf, sizeof(buf))) {
    in.sensorOk = true;
    in.ax = static_cast<int16_t>(buf[0] | (buf[1] << 8)) / kLsbPerG;
    in.ay = static_cast<int16_t>(buf[2] | (buf[3] << 8)) / kLsbPerG;
    in.az = static_cast<int16_t>(buf[4] | (buf[5] << 8)) / kLsbPerG;
  }

  uint8_t st = 0;
  if (readRegs(kFtAddr, kRegTdStatus, &st, 1)) {
    const int n = st & 0x0F;
    if (n >= 1 && n <= 2) {
      uint8_t p[4];
      if (readRegs(kFtAddr, kRegP1Xh, p, sizeof(p))) {
        const int rawX = ((p[0] & 0x0F) << 8) | p[1];
        const int rawY = ((p[2] & 0x0F) << 8) | p[3];
        in.tap = true;
        in.tapX = min(max((rawY * kScreenW) / kTouchMaxY, 0), kScreenW - 1);
        in.tapY = min(max((rawX * kScreenH) / kTouchMaxX, 0), kScreenH - 1);
        // UNVERIFIED: 触摸轴映射方向待真机确认（同 S2）
      }
    }
  }
  return in;
}

// ================= 物理层 =================

struct Ball {
  float x, y;    // 像素
  float vx, vy;  // 像素/秒
};

constexpr float kBallR = 10.0f;
constexpr float kGravityScale = 1600.0f;   // 1g 倾斜 -> 1600 px/s²（手感系数）
constexpr float kRestitution = 0.80f;      // 反弹保留 80% 速度
constexpr float kFriction = 0.995f;        // 每步微阻尼，防止永久滚动
Ball g_ball;

void resetBall() {
  g_ball.x = kScreenW / 2.0f;
  g_ball.y = kScreenH / 2.0f;
  g_ball.vx = 0;
  g_ball.vy = 0;
}

// 60Hz 固定步进：重力来自倾斜 + 墙壁反弹。
void stepPhysics(const InputState &in, float dt) {
  // 倾斜 -> 屏面内重力。板平放时 ax≈ay≈0，球静止；立起来分量趋近 ±1g。
  g_ball.vx += in.ax * kGravityScale * dt;
  g_ball.vy += in.ay * kGravityScale * dt;

  // 点击 = 把球搬到点击位置，速度清零让它从那里重新落下
  if (in.tap) {
    g_ball.x = in.tapX;
    g_ball.y = in.tapY;
    g_ball.vx = 0;
    g_ball.vy = 0;
  }

  g_ball.x += g_ball.vx * dt;
  g_ball.y += g_ball.vy * dt;
  g_ball.vx *= kFriction;
  g_ball.vy *= kFriction;

  // 墙壁反弹：位置钳位 + 速度反向衰减
  const float minX = kBallR, maxX = kScreenW - kBallR;
  const float minY = 20.0f + kBallR, maxY = kScreenH - kBallR;  // 顶部留标题栏
  if (g_ball.x < minX) { g_ball.x = minX; g_ball.vx = -g_ball.vx * kRestitution; }
  if (g_ball.x > maxX) { g_ball.x = maxX; g_ball.vx = -g_ball.vx * kRestitution; }
  if (g_ball.y < minY) { g_ball.y = minY; g_ball.vy = -g_ball.vy * kRestitution; }
  if (g_ball.y > maxY) { g_ball.y = maxY; g_ball.vy = -g_ball.vy * kRestitution; }
}

// ================= 渲染层 =================

void drawFrame() {
  tft.fillRect(0, 0, kScreenW, 18, 0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print("S3 tilt ball | tap=move ball | BOOT=reset");

  // 只擦上一帧球区域 + 画新球，全屏不重画
  static float px = -100, py = -100;
  if (px > 0) {
    tft.fillCircle(static_cast<int>(px), static_cast<int>(py), static_cast<int>(kBallR) + 1, 0x0000);
  }
  tft.fillCircle(static_cast<int>(g_ball.x), static_cast<int>(g_ball.y),
                 static_cast<int>(kBallR), 0xFD20);  // 橙色
  tft.drawCircle(static_cast<int>(g_ball.x), static_cast<int>(g_ball.y),
                 static_cast<int>(kBallR), 0xFFFF);
  px = g_ball.x;
  py = g_ball.y;
}

void drawSensorError() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 40);
  tft.print("QMI8658 未就绪");
  tft.setTextColor(kGrey);
  tft.setCursor(8, 80);
  tft.print("看串口日志排错");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== S3 滚动小球 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  if (!initSensors()) {
    drawSensorError();
    while (true) delay(1000);
  }
  resetBall();
}

void loop() {
  // ---- 固定步进计时：渲染快慢不影响物理节奏 ----
  constexpr uint32_t kStepUs = 16667;  // 1/60 s
  static uint32_t acc = 0;
  static uint32_t last = micros();

  const InputState in = readInput();
  if (!in.sensorOk) {
    Serial.println("[QMI8658] 读数据失败");
    delay(500);
    return;
  }

  // BOOT 键：球回中心
  static bool prevBtn = true;
  const bool btn = digitalRead(PIN_BOOT_BTN) == LOW;
  if (btn && !prevBtn) {
    resetBall();
    Serial.println("[S3] 球已复位");
  }
  prevBtn = btn;

  uint32_t now = micros();
  acc += now - last;
  last = now;
  if (acc > 100000) acc = 100000;  // 后台卡顿保护：最多补 6 步，防止时间债滚雪球

  int steps = 0;
  while (acc >= kStepUs && steps < 6) {
    stepPhysics(in, kStepUs / 1000000.0f);
    acc -= kStepUs;
    steps++;
  }

  drawFrame();
}
