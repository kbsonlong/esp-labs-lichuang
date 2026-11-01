# B2 · BLE GATT 开关灯

自定义 GATT 服务（0xFFF0）挂一个读+写特征值（0xFFF1）：手机写一个字节 0-255 当色环位置，板载 RGB LED 取色环颜色并点亮；读 0xFFF1 返回当前颜色的 R,G,B 三个字节。

⚠️ 本课代码未经真机验证

## 操作

```sh
uv run pio run -d labs/ble-lab-b2 -t upload -t monitor
```

nRF Connect -> Scanner -> 连接 `lichuang-ble` -> 找 Unknown Service 0xFFF0 -> 0xFFF1 特征值：

- **写**：点上传箭头，选 BYTE ARRAY，发 1 字节（如 `00` / `55` / `AA` / `FF`）。
- **读**：点下载箭头，返回 3 字节 R,G,B。

## 预期现象

- 每次写入，RGB LED 立刻变成色环对应颜色（0=纯红，85=绿，170=蓝，255≈红）。
- 屏幕显示最近写入值 `0xXX`、当前 RGB 三个分量、右下角色块同步变色。
- 串口打印写入值和发起写入的手机 MAC。
- 断开连接后板子自动重新开始广播。

## 排错

- **连不上/立刻断**：板子距手机太远或 BLE 缓存了旧服务表——手机端「刷新服务」或取消配对重连。
- **写不进去**：确认写的是 0xFFF1 且特征值带 WRITE 属性；nRF Connect 里默认发的是 UTF-8，切 BYTE ARRAY。
- **颜色对不上预期**：色环是 红→绿→蓝→红 三段渐变，中间值本来就是过渡色。
- **NimBLE 2.x 回调签名**与网上 1.x 教程不同（多了 `NimBLEConnInfo`），本课已按 2.5.1 编译验证；真机行为未验证，有问题对照库源码 `~/.platformio/libdeps`（UNVERIFIED）。
