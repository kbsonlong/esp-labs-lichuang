// C2 · 网络摄像头：手机浏览器看实时画面（HTTP MJPEG 流）。
//
// 这一课学：
// 1. MJPEG = 一串 JPEG 帧塞进 multipart/x-mixed-replace HTTP 响应里连续推——
//    浏览器收到 Content-Type 换帧指令就原地替换图片，这就是最朴素的流媒体。
// 2. 采集与推流解耦：摄像头在独立 FreeRTOS 任务里不停出帧（JPEG），
//    HTTP 任务只在"有新帧"时取走推给浏览器——两者靠队列握手，互不拖慢。
// 3. 缓冲区所有权：帧缓冲只有 fb_count=2 个，队列里丢一个、手上拿一个，
//    用"空闲信号量"保证永远不会既丢引用又超支缓冲。
//
// 复用 W2 全套连网资产：NVS 凭证、captive portal 配网、/save 保存端点。
// ⚠️ 本课代码未经真机验证（GC0308 兼容性同 C1，见 C1 README）
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <atomic>

#include "esp_camera.h"
#include "pins.h"

namespace {

// ---- 配网 / NVS（同 W2）----
constexpr char kApSsid[] = "esp32-cam-setup";
constexpr char kNvsNs[] = "wifi";
constexpr uint32_t kLongPressMs = 3000;
// 最多同时几个浏览器拉流。WebServer 是单线程的：一个流在推时，
// 其它请求要排队，所以这个上限实际上保护的是内存而不是并发。
constexpr int kMaxStreams = 3;

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalMode = false;
std::atomic<int> g_streamClients{0};

// ---- 屏驱动（同 C1/W2）----
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

// ---- IO 扩展器：CS 拉低 + 摄像头上电（同 C1）----
// bit0=LCD_CS  bit1=PA_EN  bit2=DVP_PWDN：最终 output=0x02 让 bit2=0 给摄像头供电。
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

// ---- 摄像头（同 C1 引脚；流媒体换 JPEG 输出）----
// DVP_PWDN = PCA9557 bit2：initIoExpander 的最终 output=0x02 已含 bit2=0（上电），
// 因此 pin_pwdn 填 -1，上电职责在 initIoExpander。
camera_config_t camera_cfg = {
    .pin_pwdn  = -1,
    .pin_reset = -1,
    .pin_xclk  = 5,
    .pin_sccb_sda = 1,
    .pin_sccb_scl = 2,
    .pin_d7 = 9, .pin_d6 = 4, .pin_d5 = 6, .pin_d4 = 15,
    .pin_d3 = 17, .pin_d2 = 8, .pin_d1 = 18, .pin_d0 = 16,
    .pin_vsync = 3,
    .pin_href  = 46,
    .pin_pclk  = 7,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,    // 推流必须 JPEG；GC0308 最高 VGA
    .frame_size   = FRAMESIZE_VGA,     // 640x480
    .jpeg_quality = 14,                // 0-63，越小越清晰、越大越省带宽
    .fb_count = 2,                     // 双缓冲：一个在队列、一个在采集
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

// ---- 采集任务 <-> HTTP 任务 的握手 ----
// 队列深度 1 + 覆盖写：浏览器慢的时候永远只留"最新帧"，旧帧直接被顶掉。
// fbFree 信号量：消费端 esp_camera_fb_return 之后才放行下一帧采集，
// 保证任意时刻最多占用 2 个驱动缓冲（队列里 1 个 + 正在采集 1 个）。
QueueHandle_t g_fbQueue = nullptr;
SemaphoreHandle_t g_fbFree = nullptr;

void cameraTask(void *) {
  for (;;) {
    // 没有观众时也保持采集节奏（XCLK 一直开着，代价可忽略），逻辑最简单。
    if (xSemaphoreTake(g_fbFree, pdMS_TO_TICKS(1000)) != pdTRUE) {
      continue;  // 消费端还没归还缓冲（没人拉流时 fbFree 常驻可用，不会卡这）
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      log_e("取帧失败");
      xSemaphoreGive(g_fbFree);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    xQueueOverwrite(g_fbQueue, &fb);  // 顶掉旧帧前不必归还：旧帧由消费端 return
    // 注意：xQueueOverwrite 顶掉的旧帧指针丢了引用，但驱动缓冲总数 = fb_count，
    // 消费端只 return 队列里这一帧。为防"顶掉即泄漏"，生产节流到消费速度：
    // 见下方 vTaskDelay——保证消费端来得及取走，覆盖写只是兜底而不是常规路径。
    vTaskDelay(pdMS_TO_TICKS(10));    // ~30-50 FPS 上限，给 WiFi 留喘息
  }
}

// ---- MJPEG 推流 ----
// 响应体 = N 个 "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: n\r\n\r\n<jpeg>"，
// 浏览器见到 boundary 就把上一张 <img> 原地替换。核心就是循环 + 写 bytes。
constexpr char kBoundary[] = "frame";

void handleStream() {
  if (g_streamClients.load() >= kMaxStreams) {
    server.send(503, "text/plain", "too many stream clients");
    return;
  }
  g_streamClients.fetch_add(1);

  WiFiClient client = server.client();
  String rsp = "HTTP/1.1 200 OK\r\n"
               "Content-Type: multipart/x-mixed-replace; boundary=";
  rsp += kBoundary;
  rsp += "\r\n\r\n";
  client.write(rsp.c_str(), rsp.length());

  uint32_t frames = 0;
  while (client.connected() && portalMode == false) {
    camera_fb_t *fb = nullptr;
    if (xQueueReceive(g_fbQueue, &fb, pdMS_TO_TICKS(2000)) != pdTRUE) {
      break;  // 2 秒没帧：摄像头挂了，断开让浏览器重连
    }
    String part = "--";
    part += kBoundary;
    part += "\r\nContent-Type: image/jpeg\r\nContent-Length: ";
    part += String(fb->len);
    part += "\r\n\r\n";
    bool ok = client.write(part.c_str(), part.length()) == part.length();
    if (ok) ok = client.write(fb->buf, fb->len) == fb->len;
    if (ok) ok = client.write("\r\n", 2) == 2;
    esp_camera_fb_return(fb);
    xSemaphoreGive(g_fbFree);
    if (!ok) break;
    frames++;
  }
  g_streamClients.fetch_sub(1);
  Serial.printf("[推流] 客户端断开，共推 %lu 帧，剩余 %d\n",
                static_cast<unsigned long>(frames), g_streamClients.load());
}

// ---- 首页：内嵌 <img src="/stream"> ----
void handleRoot() {
  const String ip = WiFi.localIP().toString();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>实战派网络摄像头</title></head>"
                "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee'>"
                "<h2>实战派 · 网络摄像头</h2>"
                "<img src='/stream' style='max-width:100%;border-radius:8px'>"
                "<p>IP: " +
                ip +
                " · 在线观看: " + String(g_streamClients.load()) + "</p>"
                "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ---- 配网（同 W2，含 /save 端点）----
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
                "<title>摄像头配网</title></head><body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
                "<h2>网络摄像头 Wi-Fi 配网</h2><form method='POST' action='/save'>"
                "<label>选择网络</label><br><select name='ssid' style='width:100%;padding:8px'>" +
                (options.length() ? options : "<option value=''>（暂无扫描结果，刷新重试）</option>") +
                "</select><br><br><label>密码</label><br>"
                "<input name='pass' type='password' style='width:100%;padding:8px'><br><br>"
                "<button style='width:100%;padding:12px;font-size:16px'>保存并连接</button></form></body></html>";
  return html;
}

void handlePortalRoot() { server.send(200, "text/html; charset=utf-8", buildPortalPage()); }

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
  server.on("/", handlePortalRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302);
  });
  server.begin();
  Serial.printf("[配网] 热点 %s 已开启，配网页 http://%s/\n", kApSsid, WiFi.softAPIP().toString().c_str());
}

// ---- 屏幕信息面板：IP + 在线客户端数 ----
void drawInfoPanel() {
  tft.fillScreen(0x0000);
  tft.setTextColor(kYellow);
  tft.setTextSize(3);
  tft.setCursor(8, 16);
  tft.print("网络摄像头");

  if (portalMode) {
    tft.setTextColor(kCyan);
    tft.setTextSize(2);
    tft.setCursor(8, 70);
    tft.print("配网模式");
    tft.setTextColor(0xFFFF);
    tft.setCursor(8, 100);
    tft.print(kApSsid);
    tft.setTextColor(kCyan);
    tft.setCursor(8, 130);
    tft.print("浏览器打开:");
    tft.setTextColor(0xFFFF);
    tft.setCursor(8, 154);
    tft.print(WiFi.softAPIP().toString());
    return;
  }

  tft.setTextColor(kCyan);
  tft.setTextSize(2);
  tft.setCursor(8, 80);
  tft.print("浏览器打开:");
  tft.setTextColor(0xFFFF);
  tft.setTextSize(2);
  tft.setCursor(8, 108);
  tft.print("http://" + WiFi.localIP().toString());
  tft.setTextColor(kGreen);
  tft.setCursor(8, 150);
  tft.printf("在线观看: %d 人", g_streamClients.load());
  tft.setTextColor(kGrey);
  tft.setCursor(8, 210);
  tft.printf("RSSI %d dBm", static_cast<int>(WiFi.RSSI()));
  tft.setCursor(8, 234);
  tft.printf("heap %lu KB", static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== C2 网络摄像头 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();  // 含 PCA9557 bit2=0：摄像头上电

  // 背光：显式 LEDC 通道 1，避开摄像头 XCLK 的 timer0/channel0
  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttachChannel(PIN_TFT_BL, 5000, 8, 1);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  // ---- 摄像头 ----
  const esp_err_t ret = esp_camera_init(&camera_cfg);
  if (ret != ESP_OK) {
    Serial.printf("[启动] 摄像头初始化失败: 0x%x（UNVERIFIED：GC0308 兼容性待真机确认）\n", (int)ret);
    // 不 panic：继续跑，推流会因没帧而 2 秒超时断开，屏幕给出提示
  } else {
    sensor_t *s = esp_camera_sensor_get();
    Serial.printf("[启动] 摄像头 OK，PID=0x%04x（GC0308 应为 0x9b）\n", s ? s->id.PID : 0);
  }

  // ---- 采集任务与队列 ----
  g_fbQueue = xQueueCreate(1, sizeof(camera_fb_t *));
  g_fbFree = xSemaphoreCreateCounting(1, 1);  // 初始：允许立刻采第一帧
  xTaskCreate(cameraTask, "cam-capture", 4096, nullptr, 5, nullptr);

  // ---- 连网（同 W2）----
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

  // ---- HTTP 路由 ----
  server.on("/", handleRoot);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/save", HTTP_POST, handleSave);  // 保留配网保存端点（改网后可长按 BOOT 回 portal）
  server.begin();
  Serial.println("[启动] HTTP 服务就绪：/ 首页 · /stream 推流");
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
      drawInfoPanel();
    }
    // 长按 BOOT：清凭证回配网（同 W2）
    static uint32_t pressStart = 0;
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
      if (pressStart == 0) pressStart = millis();
      if (millis() - pressStart >= kLongPressMs) {
        Serial.println("[按键] 清除凭证并重启进配网");
        prefs.begin(kNvsNs, false);
        prefs.clear();
        prefs.end();
        ESP.restart();
      }
    } else {
      pressStart = 0;
    }
    return;
  }

  server.handleClient();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 1000) {
    lastDraw = millis();
    drawInfoPanel();
  }
}
