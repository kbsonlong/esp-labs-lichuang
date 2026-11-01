// 立创·实战派 ESP32-S3 —— 全板引脚的唯一事实来源。
// 引脚定义依据 xiaozhi-esp32 官方板级配置（main/boards/lckfb/szpi-esp32s3），逐脚核对过。
// 改一个数字（或编译时传 -DPIN_xxx=N），全部课程跟着变。
//
// 实战派是"一体化"板子，大量引脚已被板载外设占用，别拿来插线做实验：
//   0              BOOT 键（也是 strapping 脚）
//   1, 2           I2C 总线（SDA/SCL：PCA9557、ES8311、ES7210、FT5x06 触摸都挂在这）
//   12,13,14,38,45 音频 I2S（DIN/WS/BCLK/MCLK/DOUT）
//   39,40,41,42    屏幕（DC/MOSI/SCLK/背光）
//   48             板载 RGB LED（WS2812）
//   3,4,5,6,7,8,9,
//   15,16,17,18,46 摄像头 DVP（进阶篇用）
//
// 完全空闲、可以外接实验的 GPIO：10、11、21、47
// 以下引脚永远别动：
//   19, 20         原生 USB D- / D+
//   26-32          接 flash 芯片
//   33-37          接八线 PSRAM（N16R8 的 "R8"）
//   43, 44         UART0 TX/RX：串口监视器
#pragma once

// 板载 WS2812 RGB LED。 ws2812: Arduino 里用 neopixelWrite(pin, r, g, b) 驱动。
#ifndef PIN_RGB
#define PIN_RGB 48
#endif

// 板上唯一的按键：BOOT。开机时按住它进烧录模式；程序运行后就是普通输入，按下 = LOW。
#ifndef PIN_BOOT_BTN
#define PIN_BOOT_BTN 0
#endif

// 课程 03：板载 2.0" IPS 屏，320x240，ST7789 驱动，SPI3。
// 引脚来自 xiaozhi lichuang_dev_board.cc：
//   mosi=40, sclk=41, dc=39；CS 板内直接接地（固件里 CS=NC），RST 与供电复位共用。
#ifndef PIN_TFT_MOSI
#define PIN_TFT_MOSI 40
#endif
#ifndef PIN_TFT_SCLK
#define PIN_TFT_SCLK 41
#endif
#ifndef PIN_TFT_DC
#define PIN_TFT_DC 39
#endif
// CS 已接地：库层传 -1 = 不管理 CS，正配硬件。
#ifndef PIN_TFT_CS
#define PIN_TFT_CS -1
#endif
#ifndef PIN_TFT_RST
#define PIN_TFT_RST -1
#endif
// 背光 PWM，低电平点亮（倒极性）。亮度 255 = 熄灭，0 = 最亮。
#ifndef PIN_TFT_BL
#define PIN_TFT_BL 42
#endif
// 面板原生 240x320，横屏使用 320x240。
#ifndef TFT_WIDTH
#define TFT_WIDTH 240
#endif
#ifndef TFT_HEIGHT
#define TFT_HEIGHT 320
#endif

// I2C 总线：板载所有低速外设的公共通道。
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 1
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 2
#endif
