// E3 FreeRTOS 双核。
//
// 这一课学：
// 1. ESP32-S3 有两个核：core 0（协议核，Wi-Fi/蓝牙协议栈住这）和
//    core 1（应用核，Arduino 的 loop() 默认跑这）。xTaskCreatePinnedToCore()
//    的最后一个参数就是把任务"钉"到哪个核——钉住了就不会被调度器搬来搬去。
// 2. 队列（Queue）：任务之间传数据的标准姿势。生产者任务往里塞，
//    消费者任务往外取，队列自带线程安全 + 阻塞语义（满了塞不进/空了取不到就睡），
//    不需要自己加锁。
// 3. 栈水位（stack high water mark）：uxTaskGetStackHighWaterMark() 返回
//    这个任务历史上剩过的最小栈空间。数值大 = 分配宽裕；接近 0 = 差点爆栈，
//    该加栈了。每个任务的栈是独立的小内存，开太大浪费、太小崩溃，水位就是标尺。
// 4. 看门狗：ESP32 的中断看门狗盯着每个核，任务里如果没有 delay/延时操作，
//    IDLE 任务饿死，看门狗以为你死机了直接重启。任务循环里 vTaskDelay()
//    一行，既是"让出 CPU"，也是"喂狗"。
//
// 结构：采集任务（core 0）模拟传感器采样 -> 队列 -> 渲染任务（core 1）画滚动波形；
// 统计任务（core 0）每秒串口报各任务栈水位 + uptime。
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include "pins.h"

namespace {

constexpr size_t kQueueLen = 16;      // 队列深度：攒 16 个样本
constexpr uint32_t kSampleMs = 50;    // 采集周期
constexpr size_t kWavePoints = 320;   // 波形横向一屏的点数

// 屏驱动（同 03-display / W2）
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

// ---- 任务间共享 ----
QueueHandle_t g_sampleQueue = nullptr;

// 各任务句柄：统计任务用它们查水位、核归属
TaskHandle_t g_hSensor = nullptr, g_hRender = nullptr, g_hStats = nullptr;

// 波形缓冲：渲染任务私有（只有它读写，不需要锁）
uint16_t g_wave[kWavePoints] = {0};
size_t g_wavePos = 0;

// ---- 采集任务（core 0）：模拟传感器 -> 队列 ----
void sensorTask(void *) {
  uint32_t tick = 0;
  for (;;) {
    // 模拟"传感器"：两个正弦叠加，比随机数好看
    const int sample = 120 + static_cast<int>(80 * sinf(tick * 0.11f) + 30 * sinf(tick * 0.37f));
    // xQueueSend：塞队列。第 3 参 0 = 队列满就不等，立刻返回（塞不下丢这个样本）
    if (xQueueSend(g_sampleQueue, &sample, 0) != pdTRUE) {
      // 队列满说明渲染跟不上：真实项目在这里计数/告警，别默默丢
    }
    tick++;
    vTaskDelay(pdMS_TO_TICKS(kSampleMs));  // 喂狗 + 让出 CPU，一举两得
  }
}

// ---- 渲染任务（core 1）：队列 -> 滚动波形 ----
void renderTask(void *) {
  int sample = 120;
  for (;;) {
    // xQueueReceive：取队列。第 3 参 portMAX_DELAY = 没数据就睡到有为止（不空转）
    if (xQueueReceive(g_sampleQueue, &sample, portMAX_DELAY) == pdTRUE) {
      g_wave[g_wavePos] = static_cast<uint16_t>(sample);
      g_wavePos = (g_wavePos + 1) % kWavePoints;

      // 整屏重画一条波形：把环形缓冲按时间顺序连成折线
      tft.fillScreen(0x0000);
      tft.setTextColor(kGrey);
      tft.setTextSize(2);
      tft.setCursor(8, 6);
      tft.print("E3 双核波形");
      tft.drawFastHLine(0, 120, 320, 0x2104);  // 中线
      tft.setTextColor(kCyan);
      tft.setCursor(8, 214);
      tft.printf("最新: %d", sample);
      for (size_t i = 1; i < kWavePoints; i++) {
        const uint16_t y1 = g_wave[(g_wavePos + i) % kWavePoints];
        const uint16_t y0 = g_wave[(g_wavePos + i - 1) % kWavePoints];
        tft.drawLine(i - 1, y0, i, y1, kGreen);
      }
    }
  }
}

// ---- 统计任务（core 0）：每秒报栈水位 + uptime ----
void printWatermark(const char *name, TaskHandle_t h) {
  // 高水位单位是"字"（StackType_t = 4 字节），乘 4 换算成字节
  const UBaseType_t words = uxTaskGetStackHighWaterMark(h);
  Serial.printf("  %-10s 核%u 栈余 %4lu 字节\n", name,
                static_cast<unsigned>(xTaskGetCoreID(h)),
                static_cast<unsigned long>(words * sizeof(StackType_t)));
}

void statsTask(void *) {
  for (;;) {
    Serial.printf("[统计] uptime %.1fs\n", millis() / 1000.0);
    printWatermark("sensor", g_hSensor);
    printWatermark("render", g_hRender);
    printWatermark("stats", g_hStats);
    Serial.printf("  空闲堆: %lu 字节，队列积压: %u\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned>(uxQueueMessagesWaiting(g_sampleQueue)));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000) delay(10);
  Serial.println();
  Serial.println("=== E3 FreeRTOS 双核 ===");

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  initIoExpander();

  pinMode(PIN_TFT_BL, OUTPUT);
  ledcAttach(PIN_TFT_BL, 5000, 8);
  ledcWrite(PIN_TFT_BL, 255 - 120);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0x0000);

  g_sampleQueue = xQueueCreate(kQueueLen, sizeof(int));

  // 最后一个参数是核号。采集/统计在 core 0（和 Wi-Fi 同核，竞争时让位），
  // 渲染钉 core 1（屏刷新卡顿不拖累采集）。
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 3, &g_hSensor, 0);
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, &g_hRender, 1);
  xTaskCreatePinnedToCore(statsTask, "stats", 4096, nullptr, 1, &g_hStats, 0);

  Serial.println("[启动] 三任务已就绪：sensor@core0 / render@core1 / stats@core0");
}

void loop() {
  // loop() 自己也在 core 1 跑，这里没什么可干的——活都在三个任务里。
  // 空着也有 delay，不抢渲染任务的 CPU。
  vTaskDelay(pdMS_TO_TICKS(1000));
}
