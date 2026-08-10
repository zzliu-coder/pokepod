# Review Handoff

请对冻结提交只读检查：

1. 三个根页面顶栏不绘制“同步”胶囊且没有隐藏命中区。
2. 设备页“与电脑同步”仍打开既有 WirelessSyncService 五分钟窗口与独立状态页。
3. 右上角 USB 图标仍只表达物理 mounted 状态；Wi-Fi/BLE 图标不受影响。
4. host、字体、release build 证据与冻结源码一致。
5. app-only 刷写仅针对经 USB 身份确认的 PokePod，并有读回校验。

