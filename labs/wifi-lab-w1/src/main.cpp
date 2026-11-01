// W1 连网与时钟。
//
// 这一课学：
// 1. Wi-Fi 的三种角色：STA（客户端连别人）、SoftAP（自己当热点被连）、AP+STA 同时开。
// 2. 配网流程（每个 IoT 设备的必修课）：
//    没有凭证 → 板子自己开热点 → 手机连上 → 弹出/打开配网页 → 选 Wi-Fi 输密码
//    → 存入 NVS → 重启进入时钟模式。
// 3. NVS：Preferences 库把凭证写进 flash，重启不丢。
// 4. Captive portal：DNS 劫持 + 302 跳转，手机连上热点自动弹配网页。
// 5. SNTP：连上网后 NTP 对时，从此它知道现在几点。
//
// 交互：
//   首次上电 / 无凭证  → 配网模式（热点 esp32-clock-setup，浏览器开 192.168.4.1）
//   已有凭证           → 直连，屏幕变实时时钟面板
//   长按 BOOT 3 秒     → 清除凭证，重启回到配网模式（换 Wi-Fi 时用）
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

constexpr char kApSsid[] = "esp32-clock-setup";  // 配网热点的名字
constexpr char kNvsNs[] = "wifi";                // NVS 命名空间
constexpr uint32_t kLongPressMs = 3000;          // 长按 BOOT 进配网

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalMode = false;

// ---- 与第 03/04 课相同的屏驱动（LovyanGFX + PCA9557 拉低 CS）----
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

uint8_t rssiBars(int32_t rssi) {
  if (rssi > -55) return 4;
  if (rssi > -67) return 3;
  if (rssi > -75) return 2;
  if (rssi > -85) return 1;
  return 0;
}

// ---- 配网页面：列出扫描到的热点，选中 + 输密码提交 ----
String buildPortalPage() {
  const int n = WiFi.scanComplete();
  String options;
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;  // 隐藏网络无法直接列出
      options += "<option value=\"" + ssid + "\">" + ssid + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
    }
  }
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>时钟配网</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
                "<h2>时钟 Wi-Fi 配网</h2><form method='POST' action='/save'>"
                "<label>选择网络</label><br><select name='ssid' style='width:100%;padding:8px'>" +
                (options.length() ? options : "<option value=''>（暂无扫描结果，刷新重试）</option>") +
                "</select><br><br><label>密码</label><br>"
                "<input name='pass' type='password' style='width:100%;padding:8px'><br><br>"
                "<button style='width:100%;padding:12px;font-size:16px'>保存并连接</button>"
                "</form><p style='color:#888'>保存后设备会重启并连接所选网络。</p></body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", buildPortalPage());
}

void handleSave() {
  const String ssid = server.arg("ssid");
  const String pass = server.arg("pass");
  if (ssid.length() == 0) {
    server.send(400, "text/html; charset=utf-8", "<h3>网络名不能为空</h3><a href='/'>返回</a>");
    return;
  }
  prefs.begin(kNvsNs, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.printf("[配网] 已保存: %s，2 秒后重启\n", ssid.c_str());
  server.send(200, "text/html; charset=utf-8",
              "<h3>已保存，设备重启中…</h3><p>稍后它会连接 " + ssid + " 并开始走字。</p>");
  delay(2000);
  ESP.restart();
}

void startPortal() {
  portalMode = true;
  WiFi.mode(WIFI_AP_STA);  // AP+STA 同开：自己当热点的同时还能扫描周边
  WiFi.softAP(kApSsid);
  dnsServer.start(53, "*", WiFi.softAPIP());  // DNS 劫持：所有域名都指到自己
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  // captive portal：任意未知地址都重定向到配网页，手机连上自动弹窗
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302);
  });
  server.begin();
  Serial.printf("[配网] 热点 %s 已开启，配网页 http://%s/\n", kApSsid, WiFi.softAPIP().toString().c_str());
}

void drawPortalScreen() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kYellow);
  tft.setTextSize(3);
  tft.setCursor(8, 12);
  tft.print("配网模式");
  tft.setTextColor(kCyan);
  tft.setTextSize(2);
  tft.setCursor(8, 60);
  tft.print("1.手机连热点:");
  tft.setTextColor(0xFFFF);
  tft.setCursor(8, 84);
  tft.print(kApSsid);
  tft.setTextColor(kCyan);
  tft.setCursor(8, 116);
  tft.print("2.浏览器打开:");
  tft.setTextColor(0xFFFF);
  tft.setCursor(8, 140);
  tft.print(WiFi.softAPIP().toString());
  tft.setTextColor(kGrey);
  tft.setCursor(8, 180);
  tft.printf("已连接设备: %d", WiFi.softAPgetStationNum());
  tft.setCursor(8, 204);
  tft.print("保存后自动重启连网");
}

// ---- 时钟模式 ----
void drawClock() {
  static time_t last = 0;
  time_t now = time(nullptr);
  if (now == last || now <= 100000) return;  // <=10万秒 = NTP 还没对上时
  last = now;
  struct tm tm;
  localtime_r(&now, &tm);
  char dateBuf[24], timeBuf[16];
  strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tm);
  strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 12);
  tft.print(dateBuf);
  tft.setTextColor(kCyan);
  tft.setTextSize(5);
  tft.setCursor(8, 60);
  tft.print(timeBuf);

  int32_t rssi = WiFi.RSSI();
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 140);
  tft.printf("RSSI %d dBm ", static_cast<int>(rssi));
  for (int i = 0; i < 4; i++) {
    tft.fillRect(180 + i * 14, 148 - i * 6, 10, 8 + i * 6, i < rssiBars(rssi) ? kGreen : 0x2124);
  }
  Serial.printf("%s %s  RSSI %d\n", dateBuf, timeBuf, static_cast<int>(rssi));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== W1 连网与时钟 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  // 读 NVS 里上次保存的凭证。
  prefs.begin(kNvsNs, true);
  const String ssid = prefs.getString("ssid", "");
  const String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("[启动] 无凭证 -> 配网模式");
    startPortal();
    return;
  }

  Serial.printf("[启动] 有凭证: %s，连接中...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(true);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("已连接！IP: %s\n", WiFi.localIP().toString().c_str());
    configTzTime("CST-8", "ntp.aliyun.com", "cn.pool.ntp.org");
  } else {
    // 连不上（密码改了/路由器不在）也进配网，别让设备卡死在黑屏。
    log_e("连接失败 -> 转配网模式");
    WiFi.disconnect();
    startPortal();
  }
}

void loop() {
  if (portalMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    // 每 15 秒重扫周边，配网页的下拉列表保持新鲜。
    static uint32_t lastScan = 0;
    if (millis() - lastScan > 15000) {
      lastScan = millis();
      WiFi.scanNetworks();
    }
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 1000) {
      lastDraw = millis();
      drawPortalScreen();
    }
    return;
  }

  // 时钟模式。
  drawClock();

  // 长按 BOOT 3 秒：清除凭证 -> 重启进配网模式（想换 Wi-Fi 时用）。
  static uint32_t pressStart = 0;
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (pressStart == 0) pressStart = millis();
    if (millis() - pressStart >= kLongPressMs) {
      Serial.println("[按键] 长按 3s，清除凭证并重启进配网模式");
      prefs.begin(kNvsNs, false);
      prefs.clear();
      prefs.end();
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
  delay(20);
}
