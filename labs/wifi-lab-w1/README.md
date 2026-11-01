# W1 · 连网与时钟

扫描周边热点 → 连接 Wi-Fi → SNTP 对时 → 屏幕变成实时时钟面板（时间 + 日期 + RSSI 信号条）。详见 docs/course.md 的 W1 章节。

## 完整操作

```sh
cd labs/wifi-lab-w1
cp credentials.example.h credentials.h   # 填入你的 Wi-Fi 名和密码
cd ../..
uv run pio run -d labs/wifi-lab-w1 -t upload -t monitor
```

现象：屏幕依次显示 连接中 → 大字时钟（每秒走字）→ 日期 + RSSI 信号条；串口同步打印。断电重启会自动重连（凭证在 NVS 里）。

> `credentials.h` 已被 .gitignore 排除，不会把密码提交到 git。

没填凭证也能玩：自动进入扫描模式，屏幕每 10 秒列出周围热点。
