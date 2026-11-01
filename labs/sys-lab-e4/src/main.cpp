// E4 配置系统。
//
// 这一课学：
// 1. Preferences：ESP32 的键值存储（底层就是 NVS flash 分区）。相比裸 NVS API，
//    它是 C++ 风格的 put/get，按"命名空间"隔离。存的配置断电不丢。
//    适合结构简单的配置；字段多而嵌套时，序列化成 JSON 字符串再存也常见。
// 2. 配置的完整生命周期：读默认值 -> 用户在网页改 -> 校验 -> 写回 -> 屏上生效。
//    "校验"是网页配置的灵魂：不合法的输入（纬度 999、间隔 0）永远不该进 flash。
// 3. 恢复出厂：不是擦 flash，而是把配置命名空间 clear() 掉重启，
//    下次启动读不到配置，自然回到默认值。长按 BOOT 3 秒触发。
// 4. 复用 W2 的全部 Wi-Fi 资产：NVS 凭证 + captive portal 配网。
//    Wi-Fi 命名空间（"wifi"）和配置命名空间（"config"）分开，
//    恢复出厂只擦配置，不动用户辛苦配好的网。
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

constexpr char kApSsid[] = "esp32-config-setup";
constexpr char kWifiNs[] = "wifi";      // W2 同款：Wi-Fi 凭证
constexpr char kConfigNs[] = "config";  // 本课主角：业务配置
constexpr uint32_t kLongPressMs = 3000;

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalMode = false;

// ---- 配置结构：默认值 + NVS 读写 ----
struct Config {
  float lat = 23.13f;            // 城市纬度
  float lon = 113.26f;           // 城市经度
  uint32_t refreshSec = 600;     // 刷新间隔（秒）
  uint16_t theme = 0x07FF;       // 主题色 RGB565（默认青色）

  bool valid() const {
    return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 &&
           refreshSec >= 10 && refreshSec <= 86400;
  }
};
Config g_cfg;

void loadConfig() {
  prefs.begin(kConfigNs, true);
  g_cfg.lat = prefs.getFloat("lat", 23.13f);
  g_cfg.lon = prefs.getFloat("lon", 113.26f);
  g_cfg.refreshSec = prefs.getULong("refresh", 600);
  g_cfg.theme = prefs.getUShort("theme", 0x07FF);
  prefs.end();
}

void saveConfig(const Config &c) {
  prefs.begin(kConfigNs, false);
  prefs.putFloat("lat", c.lat);
  prefs.putFloat("lon", c.lon);
  prefs.putULong("refresh", c.refreshSec);
  prefs.putUShort("theme", c.theme);
  prefs.end();
}

// ---- 屏驱动（同 W2）----
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

void drawConfigScreen(const char *banner = "E4 配置系统") {
  tft.fillScreen(0x0000);
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print(banner);

  tft.setTextColor(kCyan);
  tft.setTextSize(2);
  tft.setCursor(8, 56);
  tft.printf("城市: %.2f, %.2f", g_cfg.lat, g_cfg.lon);

  tft.setTextColor(0xFFFF);
  tft.setCursor(8, 92);
  tft.printf("刷新: %lus", static_cast<unsigned long>(g_cfg.refreshSec));

  tft.setCursor(8, 128);
  tft.print("主题色:");
  tft.fillRect(110, 124, 48, 24, g_cfg.theme);
  tft.drawRect(110, 124, 48, 24, kGrey);
  tft.setTextColor(kGrey);
  tft.setCursor(168, 130);
  tft.printf("0x%04X", g_cfg.theme);

  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 200);
  tft.print("改配置: 浏览器 /config");
  tft.setCursor(8, 224);
  tft.print("长按BOOT 3s 恢复出厂");
}

// ---- Wi-Fi 配网（同 W2）----
String buildPortalPage() {
  const int n = WiFi.scanComplete();
  String options;
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      options += "<option value=\"" + ssid + "\">" + ssid + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
    }
  }
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>配置系统配网</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
                "<h2>Wi-Fi 配网</h2><form method='POST' action='/save'>"
                "<label>选择网络</label><br><select name='ssid' style='width:100%;padding:8px'>" +
                (options.length() ? options : "<option value=''>（暂无扫描结果，刷新重试）</option>") +
                "</select><br><br><label>密码</label><br>"
                "<input name='pass' type='password' style='width:100%;padding:8px'><br><br>"
                "<button style='width:100%;padding:12px;font-size:16px'>保存并连接</button></form></body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", buildPortalPage()); }

void handleSave() {
  const String ssid = server.arg("ssid");
  if (ssid.length() == 0) {
    server.send(400, "text/html; charset=utf-8", "<h3>网络名不能为空</h3><a href='/'>返回</a>");
    return;
  }
  prefs.begin(kWifiNs, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  server.send(200, "text/html; charset=utf-8", "<h3>已保存，重启中…</h3>");
  delay(2000);
  ESP.restart();
}

void startPortal() {
  portalMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kApSsid);
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302);
  });
  server.begin();
  Serial.printf("[配网] 热点 %s 已开启，配网页 http://%s/\n", kApSsid, WiFi.softAPIP().toString().c_str());
}

// ---- /config 配置表单 ----
String buildConfigPage(const char *msg = "") {
  char html[1024];
  snprintf(html, sizeof(html),
           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>设备配置</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
           "<h2>设备配置</h2><p style='color:green'>%s</p>"
           "<form method='POST' action='/config'>"
           "<label>纬度 (-90 ~ 90)</label><br><input name='lat' type='number' step='0.01' value='%.2f' style='width:100%%;padding:8px'><br><br>"
           "<label>经度 (-180 ~ 180)</label><br><input name='lon' type='number' step='0.01' value='%.2f' style='width:100%%;padding:8px'><br><br>"
           "<label>刷新间隔秒 (10 ~ 86400)</label><br><input name='refresh' type='number' value='%lu' style='width:100%%;padding:8px'><br><br>"
           "<label>主题色 RGB565 (0-65535)</label><br><input name='theme' type='number' value='%u' style='width:100%%;padding:8px'><br><br>"
           "<button style='width:100%%;padding:12px;font-size:16px'>保存</button></form>"
           "<p><a href='/'>回首页</a></p></body></html>",
           msg, static_cast<double>(g_cfg.lat), static_cast<double>(g_cfg.lon),
           static_cast<unsigned long>(g_cfg.refreshSec), g_cfg.theme);
  return String(html);
}

void handleConfigGet() { server.send(200, "text/html; charset=utf-8", buildConfigPage()); }

void handleConfigPost() {
  // 先解析进临时结构，校验通过才落 flash —— 脏数据不进 NVS
  Config c;
  c.lat = server.arg("lat").toFloat();
  c.lon = server.arg("lon").toFloat();
  c.refreshSec = static_cast<uint32_t>(server.arg("refresh").toInt());
  const long theme = server.arg("theme").toInt();

  if (!c.valid()) {
    server.send(400, "text/html; charset=utf-8",
                buildConfigPage("<b>校验失败：数值超出范围，什么都没保存</b>"));
    return;
  }
  if (theme < 0 || theme > 65535) {
    server.send(400, "text/html; charset=utf-8",
                buildConfigPage("<b>校验失败：主题色要 0-65535</b>"));
    return;
  }
  c.theme = static_cast<uint16_t>(theme);
  g_cfg = c;
  saveConfig(g_cfg);
  Serial.printf("[配置] 已保存: lat=%.2f lon=%.2f refresh=%lus theme=0x%04X\n",
                static_cast<double>(g_cfg.lat), static_cast<double>(g_cfg.lon),
                static_cast<unsigned long>(g_cfg.refreshSec), g_cfg.theme);
  drawConfigScreen("E4 配置已更新 ✔");
  server.send(200, "text/html; charset=utf-8", buildConfigPage("<b>已保存并生效</b>"));
}

void startConfigServer() {
  server.on("/", []() { server.send(200, "text/html; charset=utf-8", buildConfigPage()); });
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.begin();
  Serial.printf("[配置] 页面 http://%s/config\n", WiFi.localIP().toString().c_str());
}

// ---- 恢复出厂：只擦配置命名空间，Wi-Fi 凭证不动 ----
void factoryReset() {
  Serial.println("[出厂] 长按 BOOT 3s，擦除配置并重启（Wi-Fi 凭证保留）");
  drawConfigScreen("恢复出厂...");
  prefs.begin(kConfigNs, false);
  prefs.clear();
  prefs.end();
  delay(1000);
  ESP.restart();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== E4 配置系统 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  loadConfig();
  drawConfigScreen();

  prefs.begin(kWifiNs, true);
  const String ssid = prefs.getString("ssid", "");
  const String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("[启动] 无凭证 -> 配网模式");
    startPortal();
    return;
  }

  Serial.printf("[启动] 连接 %s ...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(true);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    log_e("连接失败 -> 转配网模式");
    WiFi.disconnect();
    startPortal();
    return;
  }
  Serial.printf("已连接！IP: %s\n", WiFi.localIP().toString().c_str());
  startConfigServer();
}

void loop() {
  if (portalMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    static uint32_t lastScan = 0;
    if (millis() - lastScan > 15000) {
      lastScan = millis();
      WiFi.scanNetworks();
    }
  } else {
    server.handleClient();
  }

  // 长按 BOOT 3 秒恢复出厂（同 W2 的清凭证手势，本课擦的是配置）
  static uint32_t pressStart = 0;
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (pressStart == 0) pressStart = millis();
    if (millis() - pressStart >= kLongPressMs) {
      factoryReset();
    }
  } else {
    pressStart = 0;
  }
  delay(20);
}
