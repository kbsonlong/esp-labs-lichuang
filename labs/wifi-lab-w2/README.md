# W2 · 天气站

W1 的时钟面板升级成天气面板：温度大字 + 天气现象 + 湿度 + 风速 + 时钟 + 信号条。数据来自 Open-Meteo（免费无 key），每 10 分钟自动刷新。详见 docs/course.md 的 W2 章节。

## 完整操作

```sh
uv run pio run -d labs/wifi-lab-w2 -t upload -t monitor
```

前提：板子已配过网（W1 的凭证存在 NVS 里，直接沿用；没配过就再走一遍 W1 的手机配网流程）。

换城市：编辑 `src/main.cpp` 顶部的 `kLat` / `kLon`（纬度/经度）。
