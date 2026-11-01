# 第 05 课：I2C 总线扫描

扫描板载 I2C 总线（SDA=1, SCL=2），找到 ES8311 / PCA9557 / FT5x06 / ES7210 四个板载芯片。详见 [docs/course.md 第 05 课](../../docs/course.md#第-05-课i2c-总线扫描labs05-i2c-scan)。

## 完整操作

```sh
cd esp-labs-lichuang        # 仓库根目录（首次先跑一次 uv sync）
uv run pio run -d labs/05-i2c-scan -t upload -t monitor
```

现象：串口每 5 秒打一张"I2C 在线设备"表，应看到 0x18 / 0x19 / 0x38 / 0x41 中的若干个（部分批次不含触摸，缺 0x38 属正常）。`Ctrl+C` 退出监视器。

想刷回小智：仓库根目录跑 `./tools/flash-xiaozhi.sh`。
