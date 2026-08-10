# Worklog

- 2026-08-10 16:30: baseline `9cf9031795e1ab536ef5d3b39e7da3b0cd9726fc`, clean worktree.
- 确认 TinyUSB mounted 与 CDC DTR 已分层；USB 图标仍缺少 PMU VBUS 硬门槛。
- 确认 SNTP 每轮启动，但 RTC 仅在每次开机首次同步后写入。
- 确认屏幕关闭时触摸中断可单独触发亮屏，现有“抬起亮屏”开关只控制 IMU。
- 确认空闲 Wi-Fi 仍把 CPU 固定在 240 MHz，light sleep 每 100 ms 定时唤醒。
- Arduino-ESP32 文档确认 SNTP notification callback 与 Wi-Fi MIN_MODEM 接口；当前预编译 SDK 未启用 BLE modem sleep，故不伪造该能力。
- 实现每次 Wi-Fi 连接 generation 的 SNTP/RTC 更新、自动亮屏统一开关、屏幕两级亮度、空闲 CPU/Wi-Fi 策略与 BLE 会话参数切换。
- USB UI 物理状态增加 PMU VBUS 门槛；CDC DTR 会话释放链保持独立。
- host suite 59/59 PASS；固定字库扫描 103 源，16/20/28 各 425 glyphs、缺字 0。
- fresh clean build PASS：Flash 2,325,895/3,145,728=73%，RAM 103,652/327,680=31%，项目源码 warning 0。
- app 2,326,144 bytes，SHA256 `6afff5a3a1f24d89256722c46fb3f694711faea603d345524d30d4c47491cb8b`。
- merged 16,777,216 bytes，SHA256 `682518c1d5bc53d58500e33c768e6641911bc14606ff9ee9b4693322063fa924`。
- 设备当前未枚举；未刷机，未测真实待机电流、自动唤醒和拔线图标。
