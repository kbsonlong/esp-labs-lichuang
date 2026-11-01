# C2 · 网络摄像头

板子连上 Wi-Fi 后变成一台网络摄像头：独立 FreeRTOS 任务不停采集 JPEG 帧，WebServer 用 multipart/x-mixed-replace 把帧连续推给浏览器，手机/电脑打开 `http://<板子IP>/` 就能看实时画面。屏幕实时显示 IP 和在线观看人数。配网复用 W2 全套（NVS 凭证 + captive portal + `/save` 端点）。

> ⚠️ **本课代码未经真机验证**
> **UNVERIFIED：传感器兼容性待真机确认** —— GC0308 驱动（PID 0x9b）编译在框架的 esp32-camera 预编译库里，代码能编译通过；但"摄像头真的能出图"还没在实战派真机上跑过（详见 C1 README 排错表）。

## 操作命令

```sh
uv run pio run -d labs/cam-lab-c2 -t upload -t monitor
```

前提：W1 已配过网（凭证在 NVS 里直接沿用）；没配过就先连热点 `esp32-cam-setup` 走配网流程。

## 预期现象

1. 串口打出 `摄像头 OK，PID=0x9b` 和 `已连接！IP: x.x.x.x`
2. 屏幕显示 **网络摄像头 / 浏览器打开 http://x.x.x.x / 在线观看: N 人**
3. 手机或电脑浏览器打开 `http://<板子IP>/`，页面内嵌实时画面（`<img src="/stream">`），在线人数实时 +1
4. 关掉浏览器，在线人数回落；串口打 `客户端断开，共推 N 帧`
5. 换 Wi-Fi：长按 BOOT 键 3 秒清凭证 → 重启回配网模式

## 排错

| 现象 | 原因 | 处理 |
| ---- | ---- | ---- |
| 页面一直转圈没画面 | 摄像头初始化失败（GC0308 兼容性） | 看串口 `0x20001/0x20004` 错误码，按 C1 README 排错表处理 |
| 画面卡顿 / 延迟高 | JPEG 质量过高或 Wi-Fi 信号差 | 调大 `jpeg_quality`（如 20）或降 `frame_size` 为 `FRAMESIZE_CIF` |
| 第二个浏览器打不开页面 | WebServer 单线程：一个流在推时其它请求排队 | 正常现象；流断开后立即恢复 |
| 屏幕不亮 | 漏了 `initIoExpander()`（CS 没拉低） | 确认它在 `tft.init()` 之前执行 |
| 503 too many stream clients | 超过 3 个并发拉流 | 关掉多余的浏览器标签 |

## 代码走读（`src/main.cpp`）

- **MJPEG 的本质**：HTTP 响应 `Content-Type: multipart/x-mixed-replace; boundary=frame`，之后不断写 `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: n\r\n\r\n<jpeg 字节>`。浏览器见到 boundary 就把 `<img>` 原地替换成新帧——没有任何黑科技，就是"循环写 bytes"。
- **采集与推流解耦**：`cameraTask` 独立跑 `esp_camera_fb_get()`，把帧指针放进**深度 1 的队列**（`xQueueOverwrite`）；HTTP 任务从队列取帧推给浏览器。推流慢时队列里永远只有"最新帧"，不积压。
- **缓冲所有权**：帧缓冲只有 `fb_count=2` 个。`fbFree` 计数信号量做握手——消费端 `esp_camera_fb_return()` 之后才放行下一次采集，保证队列里 1 个 + 采集中 1 个，永不超支、永不泄漏。
- **Wi-Fi 全套是 W2 的**：`Preferences`（NVS）存凭证、AP + DNS 劫持 captive portal 配网、`/save` POST 保存并重启。连不上自动回落配网模式。

## 已知限制

- WebServer 是单线程同步模型：一路推流时其它 HTTP 请求会排队（首页刷新慢），不是崩溃。真正多客户端并发要换 AsyncWebServer（W3 课程内容）。
- 无人观看时采集任务空转（每帧等信号量超时），功耗略高；低功耗场景应在无客户端时暂停采集。
