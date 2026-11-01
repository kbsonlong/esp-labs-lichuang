// 第 03 课：点亮板载屏幕。
// 实战派带一块 2.0" IPS 屏：ST7789 驱动、SPI 接口、320x240。全部焊在板上，零接线。
//
// 这一课学：
// 1. SPI 四条线的分工：SCLK 时钟、MOSI 数据、DC 命令/数据切换、背光。
// 2. RGB565：一个像素 16bit——红 5 位、绿 6 位、蓝 5 位（人眼对绿色最敏感，所以多 1 位）。
// 3. 背光不是 SPI 的一部分：GPIO42 单独 PWM 调光，且这块板低电平点亮（倒极性）。
// 4. 隐藏关卡：LCD_CS 不在 ESP32 手里，在 I2C 扩展芯片 PCA9557 的 bit0 上！
//    不驱动扩展器、不把 CS 拉低，SPI 配得再对屏也是黑的。官方规则：
//    "CS 拉低必须放在复位之后、初始化之前，否则液晶屏无法显示。"
//
// 现象：彩条 + 文字 + 每秒刷新的帧率计。按 BOOT 键切换亮度。
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// 驱动 IO 扩展器 PCA9557（I2C 0x19），并把 LCD_CS 拉低。
// bit0 = LCD_CS（低电平选中）  bit1 = PA_EN（功放）  bit2 = DVP_PWDN（摄像头）
// 配置值与官方固件逐字节一致：output=0x03, config=0xF8（bit0-2 输出），最后 CS 拉低。
void initIoExpander() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  Wire.write(0x03);  // 输出寄存器：bit0=1 bit1=1 bit2=0
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x03);
  Wire.write(0xF8);  // 方向寄存器：bit0-2 设为输出
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  Wire.write(0x02);  // bit0 -> 0：LCD_CS 拉低（选中屏幕），屏幕初始化前必做
  Wire.endTransmission();
}

// 把实战派的屏配置固定成一个类：引脚、SPI 模式、面板参数，一处写清。
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

 public:
  LGFX() {
    // ---- 总线：SPI 的"线"和"节奏" ----
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;     // 官方固件用的同一路硬件 SPI
      cfg.spi_mode = 0;             // 真机穷举实测：mode 0 可用
      cfg.freq_write = 40000000;    // 40MHz 写入
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_TFT_SCLK;  // 41
      cfg.pin_mosi = PIN_TFT_MOSI;  // 40
      cfg.pin_miso = -1;            // 屏只进不出
      cfg.pin_dc = PIN_TFT_DC;      // 39
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);

    // ---- 面板：这块屏的"身份证" ----
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_TFT_CS;      // -1：CS 由 PCA9557 控制（见 initIoExpander）
      cfg.pin_rst = PIN_TFT_RST;    // -1：复位与整板复位共用
      cfg.pin_busy = -1;
      cfg.panel_width = 240;        // 原生 240x320，横屏用 setRotation 转
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;         // 不回读像素
      cfg.invert = true;            // 真机穷举实测：必须反显，否则黑白颠倒
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

constexpr uint32_t kBacklightFreq = 5000;
constexpr uint8_t kBacklightRes = 8;  // 8bit：0-255

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
  // 等 USB 串口真正连上再打印，避免输出在主机重新枚举期间丢失。
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== 第 03 课：点亮屏幕 ===");

  // BOOT 键要轮询（调背光），必须先初始化成输入上拉；
  // 漏了这行，digitalRead 会刷 "IO 0 is not set as GPIO" 警告。
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  // 顺序很重要：先让扩展器选中屏幕，再初始化屏。
  initIoExpander();

  // 背光先点亮，能看到屏从黑到亮的过程。
  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, kBacklightFreq, kBacklightRes);
  setBacklight(80);

  // LGFX 初始化包含总线 + 面板全套命令。
  tft.init();
  tft.setRotation(1);  // 横屏：320 宽 x 240 高
  tft.fillScreen(0x0000);

  log_i("屏幕就绪：%dx%d", tft.width(), tft.height());
}

void loop() {
  static uint32_t frames = 0;
  static uint32_t lastFpsAt = 0;
  static uint8_t brightnessLevels[] = {30, 80, 160, 255};
  static uint8_t brightnessIdx = 1;
  static uint32_t lastPress = 0;

  // 每帧：重画彩条 + 文字。
  drawColorBars();
  tft.fillRect(0, tft.height() / 2, tft.width(), tft.height() / 2, 0x0000);
  tft.setCursor(8, tft.height() / 2 + 8);
  tft.setTextColor(rgb565(0, 255, 255));
  tft.setTextSize(2);
  tft.println("Hello, lichuang!");
  tft.setCursor(8, tft.height() / 2 + 40);
  tft.setTextColor(rgb565(255, 255, 0));
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
