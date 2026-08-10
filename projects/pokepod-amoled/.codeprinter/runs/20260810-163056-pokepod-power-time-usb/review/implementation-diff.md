# Implementation diff

- Wi-Fi：每个连接 generation 只接受一次成功 SNTP 通知并回写 RTC；电池空闲网络保活缩短至 30 秒。
- 电源：自动亮屏统一控制触摸和抬起唤醒；屏幕 12 秒降亮度、30 秒关闭；空闲 CPU 不再被 Wi-Fi 固定在 240 MHz。
- BLE：空闲与语音使用两组连接参数。
- USB：顶栏电脑图标由 TinyUSB mounted 与 PMU VBUS 联合判断；CDC DTR 会话状态保持独立。
- 测试：新增三组纯状态测试和一个跨模块静态合同，完整 host 59/59。
