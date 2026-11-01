// S1 水平仪：I2C 读 QMI8658 姿态传感器，屏幕画气泡水平仪。
//
// 这一课学：
// 1. I2C 设备的"见面礼"：先读 WHO_AM_I 寄存器确认芯片在线再谈配置，
//    地址扫到 ≠ 芯片能用，WHO_AM_I 对得上才算数。
// 2. QMI8658 的最小可用配置：CTRL1（接口）-> CTRL2（量程/速率）-> CTRL7（使能），
//    三次寄存器写入，传感器就开始吐数据。
// 3. 原始数据 -> 物理量：±4g 量程下 1 LSB = 1/8192 g，除一下就是重力加速度倍数。
// 4. 用重力分量画水平仪：板子放平 Z≈1g；倾斜时 X/Y 分量按比例增大，
//    把分量映射成屏幕上的气泡偏移，就是一个气泡水平仪。
//
// QMI8658 寄存器布局来自公开数据手册（QST QMI8658A/C datasheet）；
// 数据块 0x35 起连读 6 字节 = 加速度 X/Y/Z 各 16 位小端。
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// ---- QMI8658 寄存器（寄存器表与初始化序列来自立创官方例程 02-attitude）----
constexpr uint8_t kQmiAddr = 0x6A;         // 板上实测扫描确认的 7 位地址
constexpr uint8_t kRegWhoAmI = 0x00;       // 芯片 ID
constexpr uint8_t kWhoAmIValue = 0x05;     // 真机实测：本批 QMI8658 应答 0x05（官方例程同值）
constexpr uint8_t kRegReset = 0x60;        // 软复位寄存器，写 0xB0 生效
constexpr uint8_t kRegCtrl1 = 0x02;        // 串口接口：bit6=地址自动递增（I2C 连读必备）
constexpr uint8_t kRegCtrl2 = 0x03;        // 加速度计：[6:4]量程 [3:0]ODR
constexpr uint8_t kRegCtrl3 = 0x04;        // 陀螺仪：[6:4]量程 [3:0]ODR
constexpr uint8_t kRegCtrl7 = 0x08;        // 传感器总开关：bit0=gEN bit1=aEN
constexpr uint8_t kRegAccelXLow = 0x35;    // 加速度输出数据块起始寄存器
constexpr uint8_t kAccelFsG = 4;           // 量程 ±4g
constexpr float kLsbPerG = 32768.0f / kAccelFsG;  // = 8192 LSB/g

// 官方例程的初始化值：CTRL1 开地址自增；CTRL7 加速度+陀螺仪都开；
// CTRL2=0x95 即 ±4g @ 250Hz；CTRL3=0xd5 即 512dps @ 250Hz。
constexpr uint8_t kCtrl1AutoInc = 0x40;
constexpr uint8_t kCtrl7AccGyro = 0x03;
constexpr uint8_t kCtrl2Accel4g250Hz = 0x95;
constexpr uint8_t kCtrl3Gyro512dps250Hz = 0xD5;

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
      cfg.pin_cs = PIN_TFT_CS;      // -1：CS 由 PCA9557 扩展器管，见 initIoExpander()
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

constexpr uint16_t kCyan = 0x07FF, kYellow = 0xFFE0, kGreen = 0x07E0,
                   kGrey = 0x7BEF, kRed = 0xF800, kWhite = 0xFFFF;

// 用屏的第一步：初始化 PCA9557 IO 扩展器并把屏 CS 拉低，否则屏全黑。
void initIoExpander() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(0x19);
  Wire.write(0x01);   // 输出寄存器
  Wire.write(0x03);
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x03);   // 配置寄存器
  Wire.write(0xF8);
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x01);   // 输出寄存器：bit0=0 拉低屏 CS
  Wire.write(0x02);
  Wire.endTransmission();
}

// ---- QMI8658 读写原语 ----
bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kQmiAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(kQmiAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  if (Wire.requestFrom(static_cast<int>(kQmiAddr), static_cast<int>(n)) != static_cast<int>(n)) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

struct Accel { float x, y, z; };  // 单位：g

bool initQmi8658() {
  uint8_t id = 0;
  if (!readRegs(kRegWhoAmI, &id, 1)) {
    Serial.println("[QMI8658] I2C 读 WHO_AM_I 失败：芯片不在线？");
    return false;
  }
  if (id != kWhoAmIValue) {
    Serial.printf("[QMI8658] WHO_AM_I=0x%02X，期望 0x%02X，芯片不对/寄存器布局不符\n",
                  id, kWhoAmIValue);
    return false;
  }
  Serial.printf("[QMI8658] WHO_AM_I=0x%02X OK\n", id);
  // 初始化序列与立创官方例程 02-attitude 逐字节一致：
  writeReg(kRegReset, 0xB0);               // 软复位
  delay(15);                               // 复位后等芯片就绪
  writeReg(kRegCtrl1, kCtrl1AutoInc);      // 地址自动递增（连读数据块必备）
  writeReg(kRegCtrl7, kCtrl7AccGyro);      // 使能加速度计 + 陀螺仪
  writeReg(kRegCtrl2, kCtrl2Accel4g250Hz);
  writeReg(kRegCtrl3, kCtrl3Gyro512dps250Hz);
  delay(25);                               // 等传感器起振出数
  return true;
}

bool readAccel(Accel &out) {
  uint8_t buf[6];
  if (!readRegs(kRegAccelXLow, buf, sizeof(buf))) return false;
  // 16 位小端：低字节在前。有符号数直接拼出来再除灵敏度。
  const int16_t rawX = static_cast<int16_t>(buf[0] | (buf[1] << 8));
  const int16_t rawY = static_cast<int16_t>(buf[2] | (buf[3] << 8));
  const int16_t rawZ = static_cast<int16_t>(buf[4] | (buf[5] << 8));
  out.x = rawX / kLsbPerG;
  out.y = rawY / kLsbPerG;
  out.z = rawZ / kLsbPerG;
  return true;
}

// 画气泡水平仪：外环 + 十字准线 + 气泡。
// 气泡偏移 = 重力水平分量（板子倾斜多少，气泡偏多少）。
void drawLevel(const Accel &a) {
  constexpr int cx = 160, cy = 120, kR = 104;
  static bool firstDraw = true;
  if (firstDraw) {
    firstDraw = false;
    tft.fillScreen(0x0000);
    tft.drawCircle(cx, cy, kR, kGrey);
    tft.drawCircle(cx, cy, kR / 2, kGrey);
    tft.drawCircle(cx, cy, 4, kGrey);
    tft.drawLine(cx - kR, cy, cx + kR, cy, 0x2104);  // 暗十字线
    tft.drawLine(cx, cy - kR, cx, cy + kR, 0x2104);
  }

  // 把水平分量压到 [-1,1] 再乘半径。系数取负：气泡往"翘起的那一侧"跑。
  // UNVERIFIED: 待真机确认——若气泡方向与倾斜方向相反，把 ax/ay 前的负号去掉即可。
  int bx = cx - static_cast<int>(a.x * kR);
  int by = cy - static_cast<int>(a.y * kR);
  // 限制在环内
  const int dx = bx - cx, dy = by - cy;
  const int distSq = dx * dx + dy * dy;
  const int maxDist = kR - 8;
  if (distSq > maxDist * maxDist) {
    // 简单缩放回圆内（不做精确开方，比例法够用）
    const float scale = maxDist / sqrtf(static_cast<float>(distSq));
    bx = cx + static_cast<int>(dx * scale);
    by = cy + static_cast<int>(dy * scale);
  }

  static int px = -100, py = -100;  // 上一帧气泡位置：只擦一小块，避免全屏闪烁
  if (px >= -10) {
    tft.fillCircle(px, py, 7, 0x0000);
  }
  // 越平（|g 水平分量| 越小）气泡越绿，越斜越红
  const float tilt = sqrtf(a.x * a.x + a.y * a.y);
  const uint16_t color = tilt < 0.08f ? kGreen : (tilt < 0.25f ? kYellow : kRed);
  tft.fillCircle(bx, by, 7, color);
  px = bx;
  py = by;

  // 数字读数固定在角落，用等宽感的数据区覆盖刷新
  tft.setTextDatum(lgfx::top_left);
  tft.setTextSize(2);
  tft.setTextColor(kCyan, 0x0000);  // 第二参数 = 背景色：重画时自动擦旧字
  tft.setCursor(4, 4);
  tft.printf("X%+.2f", static_cast<double>(a.x));
  tft.setCursor(4, 24);
  tft.printf("Y%+.2f", static_cast<double>(a.y));
  tft.setCursor(4, 44);
  tft.printf("Z%+.2f", static_cast<double>(a.z));
}

void drawSensorError() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 40);
  tft.print("QMI8658 未就绪");
  tft.setTextColor(kGrey);
  tft.setCursor(8, 80);
  tft.print("检查 I2C 接线/地址");
  tft.setCursor(8, 104);
  tft.print("看串口日志排错");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== S1 水平仪 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  // 背光 GPIO42 低电平点亮，倒极性 PWM
  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);  // 横屏 320x240
  tft.fillScreen(0x0000);

  if (!initQmi8658()) {
    drawSensorError();
    while (true) delay(1000);  // 传感器不在，别往下走
  }
}

void loop() {
  static uint32_t lastPrint = 0;
  Accel a;
  if (!readAccel(a)) {
    Serial.println("[QMI8658] 读数据失败");
    delay(500);
    return;
  }
  drawLevel(a);
  if (millis() - lastPrint >= 200) {  // 5Hz 打印，别刷屏
    lastPrint = millis();
    Serial.printf("X=%+.3f g  Y=%+.3f g  Z=%+.3f g\n",
                  static_cast<double>(a.x), static_cast<double>(a.y), static_cast<double>(a.z));
  }
  delay(20);  // ~40Hz 刷新足够顺滑
}
