// C1 · 拍照上屏
// GC0308 + esp32-camera → 320×240 LGFX ST7789
// BOOT 键：冻结/恢复实时画面
//
// ⚠️ 本课代码未经真机验证
// ⚠️ GC0308 传感器兼容性由 esp32-camera 库决定，若编译/运行报找不到/错误请检查引脚与传感器支持

#include "Arduino.h"
#include "esp_camera.h"
#include "pins.h"
#include <LovyanGFX.hpp>
#include <Wire.h>

// === GC0308 DVP 引脚（官方 config.h 定义）===
// XCLK=5, SIOD=1, SIOC=2, D7=9, D6=4, D5=6, D4=15, D3=17, D2=8, D1=18, D0=16
// VSYNC=3, HREF=46, PCLK=7, XCLK 20MHz
// DVP_PWDN（摄像头电源）= PCA9557 bit2：必须置 0 才上电。
// initIoExpander() 的最终 output=0x02 已含 bit2=0，天然满足；因此
// pin_pwdn 填 -1（不由本驱动控制电源），上电职责在 initIoExpander。
static camera_config_t camera_cfg = {
    .pin_pwdn  = -1,          // 电源由 PCA9557 bit2 控制（见 initIoExpander）
    .pin_reset = -1,          // 复位与整板复位共用
    .pin_xclk  = 5,
    .pin_sccb_sda = 1,        // SIOD
    .pin_sccb_scl = 2,        // SIOC
    .pin_d7 = 9, .pin_d6 = 4, .pin_d5 = 6, .pin_d4 = 15,
    .pin_d3 = 17, .pin_d2 = 8, .pin_d1 = 18, .pin_d0 = 16,
    .pin_vsync = 3,
    .pin_href  = 46,
    .pin_pclk  = 7,
    .xclk_freq_hz = 20000000,     // 20MHz
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,  // 与屏同格式，帧可直接 pushImage
    .frame_size   = FRAMESIZE_QVGA,    // 320x240，横屏 setRotation(1) 后尺寸一致
    .jpeg_quality = 12,                // 仅 JPEG 时生效；RGB565 忽略
    .fb_count = 2,                     // 双缓冲：采集与显示互不等待
    .fb_location = CAMERA_FB_IN_PSRAM, // QVGA RGB565 ≈150KB，放 PSRAM
    .grab_mode = CAMERA_GRAB_LATEST,   // 永远取最新帧，延迟最低
};

// === LGFX：实战派 2.0\" ST7789 ===
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
 public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_TFT_SCLK;   // 41
      cfg.pin_mosi = PIN_TFT_MOSI;   // 40
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_TFT_DC;       // 39
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    { auto cfg = _panel.config();
      cfg.pin_cs = PIN_TFT_CS;       // -1：由 PCA9557 控制
      cfg.pin_rst = PIN_TFT_RST;     // -1：共用整板复位
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

// === 状态 ===
static volatile bool g_frozen = false;   // BOOT 键：true=冻结，false=实时
static bool g_cameraOk = false;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();

  // --- I2C 扩展器：唤醒摄像头（PCA9557 bit2=0 为上电） ---
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  // output寄存器：bit0=1(LCD_CS), bit1=1(PA_EN), bit2=0(DVP_PWDN 摄像头上电)
  Wire.beginTransmission(0x19);
  Wire.write(0x01); Wire.write(0x03);
  Wire.endTransmission();
  // config寄存器：bit0-2 设为输出
  Wire.beginTransmission(0x19);
  Wire.write(0x03); Wire.write(0xF8);
  Wire.endTransmission();
  // output重写：bit2=0 确保摄像头有电（其余位保持不变）
  Wire.beginTransmission(0x19);
  Wire.write(0x01); Wire.write(0x02);
  Wire.endTransmission();
  Serial.println("[C1] initIoExpander Done（PWDN bit2=0）");

  // --- 背光：显式指定 LEDC 通道 1，避开摄像头 XCLK 占用的 timer0/channel0 ---
  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttachChannel(PIN_TFT_BL, 5000, 8, 1);
  ledcWrite(PIN_TFT_BL, 255 - 120);  // 倒极性：120 亮度

  // --- 摄像头初始化 ---
  esp_err_t ret = esp_camera_init(&camera_cfg);
  if (ret != ESP_OK) {
    // GC0308 兼容性未真机验证。典型错误：
    //   0x20001 ESP_ERR_CAMERA_NOT_DETECTED（SCCB 无应答：电源/排线/地址）
    //   0x20004 ESP_ERR_CAMERA_NOT_SUPPORTED（驱动不认这个 PID）
    Serial.printf("[C1] 摄像头初始化失败: 0x%x（UNVERIFIED：GC0308 兼容性待真机确认）\n", (int)ret);
    g_cameraOk = false;
  } else {
    sensor_t *s = esp_camera_sensor_get();
    Serial.printf("[C1] 摄像头初始化 OK，PID=0x%04x（GC0308 应为 0x9b）\n", s ? s->id.PID : 0);
    g_cameraOk = true;
  }

  // --- 屏幕初始化（必须在 initIoExpander 之后，CS 已拉低） ---
  tft.init();
  tft.setRotation(1);           // 横屏：320×240
  tft.fillScreen(0x0000);
  Serial.printf("[C1] Screen ready: %dx%d\n", (int)tft.width(), (int)tft.height());

  // --- BOOT 键：输入上拉（loop 里轮询消抖） ---
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
}

void loop() {
  // --- BOOT 键：轮询 + 消抖，按下瞬间翻转冻结状态 ---
  static uint32_t lastPress = 0;
  if (digitalRead(PIN_BOOT_BTN) == LOW && millis() - lastPress > 300) {
    lastPress = millis();
    g_frozen = !g_frozen;
    Serial.printf("[C1] %s\n", g_frozen ? "画面已冻结" : "恢复实时");
  }

  // --- 摄像头不可用时：黑屏 + 提示，别空转 ---
  if (!g_cameraOk) {
    static bool drawn = false;
    if (!drawn) {
      drawn = true;
      tft.fillScreen(0x0000);
      tft.setTextColor(0xFFE0);  // 黄
      tft.setTextSize(2);
      tft.setCursor(8, 90);
      tft.print("Camera init failed!");
      tft.setTextColor(0x7BEF);
      tft.setCursor(8, 120);
      tft.print("check serial log");
    }
    delay(100);
    return;
  }

  // --- 实时模式：取帧 -> pushImage 上屏（冻结时不动屏，ST7789 GRAM 自带保持） ---
  static uint32_t frames = 0;
  static uint32_t lastFpsAt = 0;
  if (!g_frozen) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      // 零拷贝 pushImage：RGB565 像素数据强转 uint16_t* 直接推送
      //  camera 320×240，旋转后的屏也是 320×240，尺寸天然对齐
      tft.pushImage(0, 0, fb->width, fb->height, (const uint16_t*)fb->buf);
      esp_camera_fb_return(fb);
      frames++;
      if (millis() - lastFpsAt >= 1000) {
        Serial.printf("[C1] FPS: %lu\n", static_cast<unsigned long>(frames));
        frames = 0;
        lastFpsAt = millis();
      }
    } else {
      log_e("esp_camera_fb_get() 返回空帧");
      delay(100);
    }
  }

  // --- 冻结角标（只画一次，避免反复覆盖画面） ---
  static bool freezeBadgeShown = false;
  if (g_frozen && !freezeBadgeShown) {
    freezeBadgeShown = true;
    tft.fillRect(0, 0, 100, 24, 0xF800);  // 红底
    tft.setTextColor(0xFFFF);
    tft.setTextSize(2);
    tft.setCursor(6, 4);
    tft.print("FREEZE");
  } else if (!g_frozen) {
    freezeBadgeShown = false;  // 下一帧画面会自然覆盖角标区域
  }
}