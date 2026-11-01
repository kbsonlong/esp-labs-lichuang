// E1 OTA：网页上传固件，在线升级。
//
// 这一课学：
// 1. OTA 是什么：Over-The-Air，不插 USB 也能换固件。ESP32 的 flash 分区表里有
//    两个 app 分区（ota_0 / ota_1），新固件写进"另一个"分区，校验通过后重启换着跑
//    ——这就是为什么 OTA 不会把正在运行的系统刷成砖。
// 2. Update.h：Arduino-ESP32 的 OTA 封装。begin() 占住备用分区，
//    write() 一块块喂 .bin 数据，end(true) 校验 MD5 并标记下次从这里启动。
// 3. WebServer 的 multipart 上传：表单 enctype='multipart/form-data' 时，
//    文件体不是一次到的，而是一小块一小块触发 upload 回调——我们边收边写 flash。
// 4. 安全提醒：本课的 /update 没有任何鉴权，同一 Wi-Fi 里任何人都能刷你的板子。
//    正式产品必须加密码 + HTTPS。
//
// 玩法：烧录本课（最后一次用 USB），连上网后在浏览器开板子的 IP，
// 选 .bin 文件上传，屏上进度走到 100% 后板子自动重启，跑进新固件。
//
// ⚠️ 本课代码未经真机验证
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

constexpr char kApSsid[] = "esp32-ota-setup";
constexpr char kNvsNs[] = "wifi";
constexpr uint32_t kLongPressMs = 3000;

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalMode = false;
bool otaDone = false;  // 上传完成待重启

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

constexpr uint16_t kCyan = 0x07FF, kYellow = 0xFFE0, kGreen = 0x07E0, kGrey = 0x7BEF, kRed = 0xF800;

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

// ---- 屏显 OTA 进度 ----
void drawProgress(int percent, const char *label) {
  tft.fillScreen(0x0000);
  tft.setTextColor(kCyan);
  tft.setTextSize(3);
  tft.setCursor(8, 30);
  tft.print("OTA 升级");
  tft.setTextColor(0xFFFF);
  tft.setTextSize(2);
  tft.setCursor(8, 80);
  tft.print(label);

  // 进度条
  tft.drawRect(8, 130, 304, 30, kCyan);
  tft.fillRect(10, 132, 3 * percent, 26, kGreen);

  tft.setTextSize(3);
  tft.setCursor(110, 180);
  tft.printf("%d%%", percent);
}

void drawError(const char *msg) {
  tft.fillScreen(0x0000);
  tft.setTextColor(kRed);
  tft.setTextSize(3);
  tft.setCursor(8, 60);
  tft.print("OTA 失败");
  tft.setTextColor(kYellow);
  tft.setTextSize(2);
  tft.setCursor(8, 120);
  tft.print(msg);
}

// ---- 配网（同 W2：NVS 凭证，没有就开 captive portal）----
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
                "<title>OTA 配网</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
                "<h2>OTA 板 Wi-Fi 配网</h2><form method='POST' action='/save'>"
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

// ---- OTA 上传页 + 处理 ----
String buildUpdatePage() {
  return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>OTA 升级</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
         "<h2>OTA 固件升级</h2>"
         "<p>当前固件: E1 " __DATE__ " " __TIME__ "</p>"
         "<form method='POST' action='/update' enctype='multipart/form-data'>"
         "<input type='file' name='firmware' accept='.bin' style='width:100%;padding:8px'>"
         "<br><br><button style='width:100%;padding:12px;font-size:16px'>上传并升级</button></form>"
         "<p style='color:#b00'>上传期间不要断电或离开页面。</p></body></html>";
}

void handleUpdatePage() { server.send(200, "text/html; charset=utf-8", buildUpdatePage()); }

// multipart 文件体一块块进来：START 开分区 -> WRITE 边收边写 -> END 校验
void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();
  static size_t total = 0;

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] 开始升级: %s\n", upload.filename.c_str());
    total = 0;
    // UPDATE_SIZE_UNKNOWN = 让 Update 自己按分区表决定上限
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      drawError("begin 失败，看串口");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      drawError("写入 flash 失败");
      return;
    }
    total += upload.currentSize;
    // 进度：没拿到总大小时按分区大小估算（默认 16MB 布局的 ota 分区 = 6553600 字节）
    const size_t otaSize = 6553600;
    int percent = static_cast<int>(total * 100 / otaSize);
    if (percent > 100) percent = 100;
    drawProgress(percent, "正在写入固件...");
    Serial.printf("[OTA] 已写入 %u 字节 (%d%%)\n", static_cast<unsigned>(total), percent);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {  // true = 校验通过后设为下次启动分区
      Serial.printf("[OTA] 升级成功，共 %u 字节，重启...\n", static_cast<unsigned>(total));
      drawProgress(100, "升级成功，重启中...");
      otaDone = true;
    } else {
      Serial.printf("[OTA] 校验失败: %s\n", Update.errorString());
      drawError(Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[OTA] 上传中断，已回滚");
    drawError("上传被中断");
  }
}

void handleUpdateResult() {
  if (otaDone) {
    server.send(200, "text/html; charset=utf-8", "<h3>升级成功，板子重启中…</h3>");
    delay(1500);
    ESP.restart();
  } else {
    server.send(500, "text/html; charset=utf-8", "<h3>升级失败，看板子屏幕/串口</h3>");
  }
}

void startUpdateServer() {
  // /update 分两步注册：POST 完成回调 + upload 流式回调
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
  server.on("/", handleUpdatePage);
  server.begin();
  Serial.printf("[OTA] 升级页 http://%s/\n", WiFi.localIP().toString().c_str());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== E1 OTA ===");

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

  tft.setTextColor(kGreen);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("E1 OTA 待命");
  tft.setTextColor(0xFFFF);
  tft.setTextSize(3);
  tft.setCursor(8, 50);
  tft.print(WiFi.localIP().toString());
  tft.setTextColor(kGrey);
  tft.setTextSize(2);
  tft.setCursor(8, 110);
  tft.print("浏览器打开上面 IP");

  startUpdateServer();
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
    return;
  }
  server.handleClient();
  delay(2);
}
