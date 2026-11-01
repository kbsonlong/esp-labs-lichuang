// W2 天气站。
//
// 这一课学：
// 1. HTTPS GET：TLS 握手比 TCP 多在哪，以及 setInsecure() 的取舍
//    （课程图省事不验证书；正式产品必须校验 CA，否则任何人都能冒充服务器）。
// 2. JSON 解析：ArduinoJson 7 + 文档过滤器——API 返回几 KB，
//    我们只提取 4 个字段，过滤器能让内存占用缩小一个量级。
// 3. WMO 天气代码：气象界通用编码，0=晴 61=雨 95=雷雨，一张表翻译成人话。
// 4. 限速礼貌：免费 API 每 10 分钟取一次就够了，别把人家服务打爆。
//
// 天气源：Open-Meteo（免费、无需 API key）。默认广州坐标，改 kLat/kLon 换城市。
// 复用 W1 的全部资产：NVS 凭证、captive portal 配网、SNTP 对时、屏驱动。
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

// ---- 你在哪：改这两行换城市 ----
constexpr float kLat = 23.13f;  // 广州
constexpr float kLon = 113.26f;

constexpr char kApSsid[] = "esp32-clock-setup";
constexpr char kNvsNs[] = "wifi";
constexpr uint32_t kLongPressMs = 3000;
constexpr uint32_t kWeatherIntervalMs = 10UL * 60 * 1000;  // 10 分钟取一次天气
const char *kApiHost = "api.open-meteo.com";

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalMode = false;

// 当前天气（取回后缓存，两次请求之间屏幕照常刷新）。
struct Weather {
  float tempC = 0;
  float humidity = 0;
  float windKmh = 0;
  int code = -1;            // WMO 天气代码，-1 = 还没取到
  uint32_t fetchedAt = 0;   // millis() 时间戳
  bool valid() const { return code >= 0; }
};
Weather g_weather;

// ---- 屏驱动（同 W1）----
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

// WMO 天气代码 -> 中文。气象界通用编码，全表见 open-meteo 文档。
const char *weatherText(int code) {
  if (code == 0) return "晴";
  if (code <= 2) return "多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code <= 57) return "毛毛雨";
  if (code <= 67) return "雨";
  if (code <= 77) return "雪";
  if (code <= 82) return "阵雨";
  if (code <= 86) return "阵雪";
  return "雷雨";
}

// ---- 配网（同 W1，换 Wi-Fi 长按 BOOT 3 秒）----
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
                "<title>天气站配网</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
                "<h2>天气站 Wi-Fi 配网</h2><form method='POST' action='/save'>"
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
  prefs.begin(kNvsNs, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  Serial.printf("[配网] 已保存: %s，2 秒后重启\n", ssid.c_str());
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
}

// ---- HTTPS 天气请求 + JSON 解析 ----
bool fetchWeather() {
  WiFiClientSecure tls;
  tls.setInsecure();  // 课程从简：跳过 CA 校验。正式产品必须校验，否则可被中间人冒充
  HTTPClient https;
  char url[192];
  snprintf(url, sizeof(url),
           "https://%s/v1/forecast?latitude=%.2f&longitude=%.2f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&timezone=auto",
           kApiHost, kLat, kLon);
  Serial.printf("[天气] GET %s\n", url);
  if (!https.begin(tls, url)) {
    log_e("HTTPS 初始化失败");
    return false;
  }
  https.setTimeout(10000);
  const int httpCode = https.GET();
  if (httpCode != 200) {
    log_e("HTTP %d", httpCode);
    https.end();
    return false;
  }
  const String body = https.getString();
  https.end();

  // 过滤器：声明只关心哪些字段，解析器跳过其余内容，内存省一个量级。
  JsonDocument filter;
  JsonObject cur = filter["current"].to<JsonObject>();
  cur["temperature_2m"] = true;
  cur["relative_humidity_2m"] = true;
  cur["weather_code"] = true;
  cur["wind_speed_10m"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    log_e("JSON 解析失败");
    return false;
  }
  JsonObject c = doc["current"];
  g_weather.tempC = c["temperature_2m"] | 0.0f;
  g_weather.humidity = c["relative_humidity_2m"] | 0.0f;
  g_weather.windKmh = c["wind_speed_10m"] | 0.0f;
  g_weather.code = c["weather_code"] | -1;
  g_weather.fetchedAt = millis();
  Serial.printf("[天气] %.1f°C %s 湿度%.0f%% 风%.0fkm/h\n",
                g_weather.tempC, weatherText(g_weather.code), g_weather.humidity, g_weather.windKmh);
  return true;
}

// ---- 屏幕排版：天气 + 时间 + 信号 ----
void drawPanel() {
  tft.fillScreen(0x0000);

  // 时间（NTP 对上才有意义）
  time_t now = time(nullptr);
  char timeBuf[12] = "--:--";
  if (now > 100000) {
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tm);
  }
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print(timeBuf);
  int32_t rssi = WiFi.RSSI();
  tft.setCursor(200, 10);
  tft.printf("%d", static_cast<int>(rssi));

  if (!g_weather.valid()) {
    tft.setTextColor(kYellow);
    tft.setTextSize(2);
    tft.setCursor(8, 100);
    tft.print("等待天气数据...");
    return;
  }

  // 大字温度
  tft.setTextColor(0xFFFF);
  tft.setTextSize(6);
  tft.setCursor(8, 50);
  tft.printf("%.1f", static_cast<double>(g_weather.tempC));
  tft.setTextSize(3);
  tft.print("C");

  // 天气现象 + 湿度 + 风
  tft.setTextColor(kCyan);
  tft.setTextSize(3);
  tft.setCursor(8, 130);
  tft.print(weatherText(g_weather.code));
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 180);
  tft.printf("湿度 %.0f%%", static_cast<double>(g_weather.humidity));
  tft.setCursor(8, 206);
  tft.printf("风速 %.0f km/h", static_cast<double>(g_weather.windKmh));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== W2 天气站 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  prefs.begin(kNvsNs, true);
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
  configTzTime("CST-8", "ntp.aliyun.com", "cn.pool.ntp.org");

  fetchWeather();  // 上电先取一次，之后每 10 分钟刷
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
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 1000) {
      lastDraw = millis();
      drawPortalScreen();
    }
    return;
  }

  // 定时刷新天气（限速礼貌：10 分钟一次足够）。
  static uint32_t lastFetch = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastFetch >= kWeatherIntervalMs) {
    lastFetch = millis();
    fetchWeather();
  }

  // 每秒重画一次（时间走字；天气数据是缓存的，画上去零成本）。
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    drawPanel();
  }

  // 长按 BOOT 3 秒：清凭证回配网。
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
