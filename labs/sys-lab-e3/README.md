# E3 · FreeRTOS 双核

三个任务各司其职：采集任务（core 0，模拟传感器采样入队列）、渲染任务（core 1，屏上画滚动波形）、统计任务（core 0，每秒串口报各任务栈水位 + uptime + 队列积压）。

⚠️ 本课代码未经真机验证

## 操作

```sh
uv run pio run -d labs/sys-lab-e3 -t upload -t monitor
```

## 预期现象

- 屏幕标题「E3 双核波形」，绿色正弦叠加波形从右往左滚动（每 50ms 一个采样点），下方显示最新采样值。
- 串口每秒一组统计：

```
[统计] uptime 12.0s
  sensor     核0 栈余 2000 字节
  render     核1 栈余 4600 字节
  stats      核0 栈余 2400 字节
  空闲堆: 29xxxx 字节，队列积压: 0
```

## 知识点对照

- **队列传递**：`xQueueSend`（采集，塞不进就丢）↔ `xQueueReceive`（渲染，`portMAX_DELAY` 睡到有数据），自带线程安全，无需加锁。
- **栈水位**：`uxTaskGetStackHighWaterMark()` = 该任务历史剩过的最小栈空间（字节）。数值大说明分配浪费，接近 0 就要爆栈了——调栈大小看它，不要拍脑袋。
- **喂狗**：任务循环里必须有 `vTaskDelay()`（或其他阻塞点），否则 core 的 IDLE 任务饿死、任务看门狗直接重启芯片。把 stats 任务的 `vTaskDelay` 注释掉跑 5 秒试试，你就永远记住看门狗了。

## 排错

- **屏幕黑/波形不动**：`initIoExpander()` 必须先跑（PCA9557 控 CS）。
- **`Stack canary watchpoint triggered` 崩溃**：某任务栈太小，看崩溃日志里的任务名，把 `xTaskCreatePinnedToCore` 的栈参数翻倍。
- **波形卡顿**：渲染任务每帧整屏重画（fillScreen + 320 条线），本课故意写得朴素；进阶可用 LovyanGFX 的 Sprite 只重画变化区域。
