# B1 · BLE 广播

用 NimBLE 让板子像蓝牙信标一样持续广播：设备名 `lichuang-XXXX`（XXXX = 蓝牙 MAC 尾 4 位十六进制），广播包里带厂商自定义字段（测试公司 ID 0xFFFF + "LCKFB" + 序号）。

⚠️ 本课代码未经真机验证

## 操作

```sh
uv run pio run -d labs/ble-lab-b1 -t upload -t monitor
```

手机装 **nRF Connect**（iOS/Android 均可），打开 Scanner。

## 预期现象

- 屏幕显示设备名 + 「广播中...」+ 已广播秒数。
- nRF Connect 扫描列表出现 `lichuang-XXXX`；点 RAW DATA 能看到厂商字段 `FF 09 FF FF 4C 43 4B 46 42 00`（首 0xFF 是 AD 类型，0x09 是长度，随后小端 0xFFFF 公司 ID + "LCKFB" + 序号）。
- 串口打印 BLE MAC 地址。

## 排错

- **扫描不到设备**：确认手机蓝牙已开、板子在附近；nRF Connect 把同名旧记录过滤掉的，删掉记录重扫。
- **屏全黑**：`initIoExpander()` 必须在 `tft.init()` 之前跑（PCA9557 控 CS），别删。
- **串口没日志**：本课开了 `-DCONFIG_NIMBLE_CPP_LOG_LEVEL=1`，NimBLE 只打 error；正常启动日志来自我们自己的 Serial.printf。
