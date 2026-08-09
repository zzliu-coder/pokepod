# UI 验收脚本

## 核心任务验收

| Surface ID | Case | Steps | Expected | Target | Evidence |
|---|---|---|---|---|---|
| pod-home | 横滑不误触 | 从任一卡片中部横滑超过阈值 | 页面切换，录音与 BLE 会话均未启动 | ESP32 368x448 | evidence/artifacts/pod-home.json |
| pod-home | 无线按住 | Voice ready 后按住微信卡或 BOOT，再松开 | 状态进入正在输入并正常结束；断线显示恢复 | ESP32 368x448 | evidence/artifacts/pod-home.json |
| pod-capsules | 多选删除撤销 | 长按条目，选三条，删除，再点撤销 | 条目进回收站并可恢复，滚动保持可用 | ESP32 368x448 | evidence/artifacts/pod-capsules.json |
| pod-capsules | 转写非阻塞 | 一条处于转写时翻页并查看旧条目 | 页面可操作，忙条目不可变更 | ESP32 368x448 | evidence/artifacts/pod-capsules.json |
| pod-device | 配对隔离 | 打开无线语音配对，再点底层设置坐标 | 底层动作不触发；可见返回/取消有效 | ESP32 368x448 | evidence/artifacts/pod-device.json |
| voice-menu | 异常恢复 | 建立会话后模拟 400 ms 无帧 | Option+Z 释放，原麦克风恢复，错误可见 | macOS 360x520 | evidence/artifacts/voice-menu.json |

## 实现后回验

主机状态测试、目标尺寸渲染、固件 clean build 和两个 Release app 均需通过；真机再执行 BLE 配对、30 次按住/松开、USB CDC 枚举、触摸和实际微信输入。
