// B1 BLE 广播。
//
// 这一课学：
// 1. BLE 广播（Advertising）是什么：设备不连接也一直"喊"自己的名字和数据，
//    手环、信标、米家传感器待机时干的就是这件事。广播是单向的、无连接的。
// 2. 广播包结构：31 字节有限空间，一段一段的 TLV（长度-类型-值）。
//    我们放两个字段：厂商自定义数据（Manufacturer Data）+ 设备名（放扫描响应包）。
// 3. 厂商自定义字段格式：2 字节公司 ID + 自定义字节。0xFFFF 是"测试用"保留 ID，
//    正式产品要去蓝牙 SIG 花钱买自己的 ID（小米 0x038F、苹果 0x004C……）。
// 4. NimBLE-Arduino 2.x 的广播 API：getAdvertising() 拿单例，
//    setAdvertisementData() / setScanResponseData() 塞数据，start() 开喊。
//
// 玩法：烧录后手机装 "nRF Connect"（iOS/Android 都有），打开 Scanner，
// 能看到 lichuang-XXXX 正在广播，展开 RAW DATA 能看到我们的厂商字段。
//
// ⚠️ 本课代码未经真机验证
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>  // esp_read_mac / ESP_MAC_BT
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// ---- 屏驱动（同 03-display / W2，实战派这块屏的"标准三件套"）----
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
  // PCA9557(0x19) 的 bit0 接屏幕 CS（低选中）。不初始化它，屏全黑。
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(0x19);
  Wire.write(0x01);  // output port
  Wire.write(0x03);
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x03);  // config: bit0/bit1/bit2 输出
  Wire.write(0xF8);
  Wire.endTransmission();
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  Wire.write(0x02);  // CS 拉低（选中屏），bit1 备用
  Wire.endTransmission();
}

// ---- 设备名：lichuang-XXXX，XXXX = BT MAC 尾 4 位十六进制 ----
char g_deviceName[24] = "lichuang-????";

void makeDeviceName() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);  // BLE 用的是蓝牙基地址，不是 WiFi MAC
  snprintf(g_deviceName, sizeof(g_deviceName), "lichuang-%02X%02X", mac[4], mac[5]);
}

// ---- 广播数据 ----
// 厂商字段：0xFFFF（测试公司 ID）+ "LCKFB" + 1 字节广播序号。
// 总长 2+6=8 字节，加 2 字节 TLV 头才 10 字节，31 字节限额内很宽裕。
std::vector<uint8_t> buildManufacturerData(uint8_t seq) {
  return {0xFF, 0xFF, 'L', 'C', 'K', 'F', 'B', seq};
}

NimBLEAdvertising *g_advertising = nullptr;

void startAdvertising() {
  g_advertising = NimBLEDevice::getAdvertising();

  NimBLEAdvertisementData advData;  // 广播包：厂商自定义字段
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setManufacturerData(buildManufacturerData(0));

  NimBLEAdvertisementData scanResp;  // 扫描响应包：设备名（响应包是"第二段广播"，能再装 31 字节）
  scanResp.setName(g_deviceName);

  g_advertising->setAdvertisementData(advData);
  g_advertising->setScanResponseData(scanResp);
  g_advertising->start();
  Serial.printf("[BLE] 广播已开始，设备名 %s\n", g_deviceName);
}

void drawScreen(uint32_t uptimeSec) {
  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("B1 BLE 广播");

  tft.setTextColor(kCyan);
  tft.setTextSize(2);
  tft.setCursor(8, 60);
  tft.print("设备名:");
  tft.setTextColor(0xFFFF);
  tft.setTextSize(3);
  tft.setCursor(8, 90);
  tft.print(g_deviceName);

  tft.setTextColor(kGreen);
  tft.setTextSize(3);
  tft.setCursor(8, 150);
  tft.print("广播中...");

  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 200);
  tft.print("nRF Connect 扫我");

  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 230);
  tft.printf("已广播 %lus", static_cast<unsigned long>(uptimeSec));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== B1 BLE 广播 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);  // 背光倒极性：255 = 灭

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  makeDeviceName();

  // NimBLE 初始化。2.x：init() 里可以直接传设备名（也写进 GAP 的 device name）。
  NimBLEDevice::init(g_deviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9 dBm，教室里穿一堵墙够了
  startAdvertising();

  Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());
}

void loop() {
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    drawScreen(millis() / 1000);
  }
  delay(20);
}
