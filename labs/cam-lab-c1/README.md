# C1 · 拍照上屏

GC0308 摄像头实时画面直接刷上板载 320×240 屏：esp32-camera 库出 RGB565 帧，零格式转换 `pushImage` 到 ST7789。BOOT 键冻结/恢复实时画面。详见 docs/course.md 的 C1 章节。

> ⚠️ **本课代码未经真机验证**
> **UNVERIFIED：传感器兼容性待真机确认** —— GC0308 的驱动（PID 0x9b，SCCB 地址 0x21）确实编译在本仓库使用的 esp32-camera 预编译库里，代码能编译通过；但"摄像头真的能出图"这一点还没在实战派真机上跑过。若初始化失败，串口会打出明确错误码（见下方排错表）。

## 操作命令

```sh
uv run pio run -d labs/cam-lab-c1 -t upload -t monitor
```

## 预期现象

1. 串口依次打出：扩展器就绪 → `摄像头初始化 OK，PID=0x9b` → `Screen ready: 320x240`
2. 屏幕出现摄像头实时画面（约 10-20 FPS，串口每秒打一次 FPS）
3. 按 BOOT 键：左上角出现红色 `FREEZE` 角标，画面定格；再按一下恢复实时

## 排错

| 现象 | 原因 | 处理 |
| ---- | ---- | ---- |
| 串口报 `0x20001`（NOT_DETECTED） | SCCB 无应答：摄像头没上电 / 排线问题 | 确认 `initIoExpander()` 在 `esp_camera_init` 之前执行（PCA9557 bit2=0 才给摄像头供电） |
| 串口报 `0x20004`（NOT_SUPPORTED） | 库不认这个传感器 PID | 把串口打出的 PID 报回来；GC0308 预期 0x9b |
| 屏全黑但摄像头初始化 OK | CS 没拉低 / 背光没开 | 确认先跑 `initIoExpander()` 再 `tft.init()`；背光 GPIO42 倒极性 |
| 画面颜色怪（红蓝互换） | RGB/BGR 顺序 | 把 camera_cfg 注释掉的 `swap` 或 `tft.setColorOrder` 调一下 |
| 帧率为 0，`fb_get 返回空帧` | PSRAM 未启用或被占满 | 确认板型 `esp32-s3-devkitc1-n16r8`（8MB PSRAM），QVGA RGB565 一帧约 150KB |

## 代码走读（`src/main.cpp`）

- **上电顺序是本课的隐形考点**：`initIoExpander()`（写 PCA9557：CS 拉低 + **bit2=0 给摄像头上电**）→ 背光（显式 LEDC 通道 1，**避开摄像头 XCLK 占用的 timer0/channel0**）→ `tft.init()` → `esp_camera_init()`。
- **零拷贝显示**：摄像头配置 `PIXFORMAT_RGB565` + `FRAMESIZE_QVGA`（320×240），与 `setRotation(1)` 后的屏尺寸完全一致，`pushImage` 直接把帧缓冲推给 SPI，无缩放无转换。
- **双缓冲**：`fb_count=2` + `CAMERA_GRAB_LATEST`——采集与显示各拿一个缓冲，永远显示"最新一帧"而不是"最老一帧"，延迟最低。
- **冻结的实现**：ST7789 自带 GRAM，不刷就保持。冻结时干脆不调 `esp_camera_fb_get()`，角标只画一次。
