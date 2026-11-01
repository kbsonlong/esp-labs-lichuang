// B2 BLE GATT 开关灯。
//
// 这一课学：
// 1. GATT 模型：Server（我们的板子）挂着一棵"服务 -> 特征值"树。
//    手机是 Client，主动来读写。这是 BLE 里最常用的数据交互方式。
// 2. 自定义 Service/Characteristic 的 UUID：写 App 的同学自定义 128 位 UUID，
//    课程图省事用 16 位短 UUID 0xFFF0/0xFFF1（和很多廉价蓝牙模块同款，别上正式产品）。
// 3. 写特征值 = 手机发数据下来；读特征值 = 手机拉数据上去。
//    一个特征值可以同时带 READ | WRITE 两个属性。
// 4. RGB 色环（color wheel）：把 0-255 当成色相角，走一圈 红→绿→蓝→红。
//    单字节既能当"旋钮"用，又天然覆盖全部颜色。
//
// 玩法：手机 nRF Connect -> Scanner 连上 lichuang-XXXX -> 找 0xFFF1 特征值，
// 点上传箭头写一个字节（0-255，如 0x00 / 0x55 / 0xAA），板载 RGB 灯立刻变色，
// 屏幕显示最近一次写入。读 0xFFF1 会返回当前颜色的 R,G,B 三个字节。
//
// ⚠️ 本课代码未经真机验证。NimBLE 2.5.1 的回调签名以 pioarduino 实测为准。
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// 16 位短 UUID：Service 0xFFF0，特征值 0xFFF1（读+写同一条）。
constexpr char kServiceUuid[] = "FFF0";
constexpr char kColorCharUuid[] = "FFF1";

// 当前颜色（写特征值刷新，读特征值取走；主循环拿去点灯+上屏）。
struct Color {
  uint8_t r = 255, g = 255, b = 255;  // 上电默认白
  uint8_t lastWrite = 255;            // 最近一次写入的原始字节
  bool updated = false;               // 有新写入待处理
};
Color g_color;

// ---- 色环：0-255 -> RGB。经典 Adafruit wheel 算法，三段线性渐变 ----
void colorWheel(uint8_t pos, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (pos < 85) {            // 红 -> 绿
    r = 255 - pos * 3;
    g = pos * 3;
    b = 0;
  } else if (pos < 170) {    // 绿 -> 蓝
    const uint8_t p = pos - 85;
    r = 0;
    g = 255 - p * 3;
    b = p * 3;
  } else {                   // 蓝 -> 红
    const uint8_t p = pos - 170;
    r = p * 3;
    g = 0;
    b = 255 - p * 3;
  }
}

// ---- 屏驱动（同 B1）----
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

// ---- NimBLE 回调。注意 2.x 的签名都带 NimBLEConnInfo（1.x 没有）----
class ColorCharCallbacks : public NimBLECharacteristicCallbacks {
  // 手机写 0xFFF1：取第一个字节当色环位置。
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    const std::string value = pChar->getValue();
    if (value.empty()) {
      Serial.println("[GATT] 收到空写入，忽略");
      return;
    }
    const uint8_t pos = static_cast<uint8_t>(value[0]);
    g_color.lastWrite = pos;
    colorWheel(pos, g_color.r, g_color.g, g_color.b);
    g_color.updated = true;
    // 读特征值返回当前色值：把 R,G,B 写回去，手机一读就是它
    const uint8_t rgb[3] = {g_color.r, g_color.g, g_color.b};
    pChar->setValue(std::vector<uint8_t>(rgb, rgb + 3));
    Serial.printf("[GATT] 写入 %u -> RGB(%u,%u,%u) 来自 %s\n", pos,
                  g_color.r, g_color.g, g_color.b, connInfo.getAddress().toString().c_str());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    Serial.println("[GATT] 手机已连接");
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    Serial.printf("[GATT] 断开（reason=%d），重新开始广播\n", reason);
    NimBLEDevice::startAdvertising();  // 断开后继续可被发现
  }
};

void startGatt() {
  NimBLEDevice::init("lichuang-ble");

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(NimBLEUUID(kServiceUuid));
  NimBLECharacteristic *colorChar = service->createCharacteristic(
      NimBLEUUID(kColorCharUuid),
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  colorChar->setCallbacks(new ColorCharCallbacks());
  // 初始读值 = 白色
  const uint8_t initRgb[3] = {g_color.r, g_color.g, g_color.b};
  colorChar->setValue(std::vector<uint8_t>(initRgb, initRgb + 3));
  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(service->getUUID());
  advertising->start();
  Serial.println("[GATT] 服务已启动，等待连接");
}

void drawScreen() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("B2 GATT 开关灯");

  tft.setTextColor(kCyan);
  tft.setTextSize(2);
  tft.setCursor(8, 50);
  tft.print("最近写入:");
  tft.setTextColor(0xFFFF);
  tft.setTextSize(4);
  tft.setCursor(8, 80);
  tft.printf("0x%02X", g_color.lastWrite);

  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 140);
  tft.printf("RGB %3u %3u %3u", g_color.r, g_color.g, g_color.b);

  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 180);
  tft.print("nRF Connect 连接");
  tft.setCursor(8, 204);
  tft.print("写 FFF1: 0-255");

  // 屏幕右下角画个当前颜色的色块
  tft.fillRect(220, 200, 90, 30, tft.color565(g_color.r, g_color.g, g_color.b));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== B2 GATT 开关灯 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  startGatt();
  rgbLedWrite(PIN_RGB, g_color.r, g_color.g, g_color.b);
}

void loop() {
  if (g_color.updated) {
    g_color.updated = false;
    rgbLedWrite(PIN_RGB, g_color.r, g_color.g, g_color.b);
    drawScreen();
  }
  delay(20);
}
