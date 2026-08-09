# 状态矩阵

## 页面状态

| Surface | 状态 | 事实来源 | 可见变化 | 可用动作 | 恢复 |
|---|---|---|---|---|---|
| pod-home | voice-disconnected | BleVoiceService | 微信卡显示等待 Mac | 本地胶囊、翻页 | 打开 Voice.app/配对 |
| pod-home | voice-ready | VoiceSessionController | 微信卡青紫高亮 | 两卡、翻页、BOOT hold | 断线回等待状态 |
| pod-home | local-transcribing | TencentWorker | 顶部小型转写脉冲 | 浏览、设置、无线输入 | 成功/失败落到条目 |
| pod-home | wireless-streaming | VoiceSessionController | 呼吸环、计时、连接质量 | 松开结束 | 超时/断线自动结束 |
| pod-capsules | inbox/filters | CapsuleBrowserState | 标题范围与条目 | 滚动、详情、长按 | 保留范围和位置 |
| pod-capsules | multi-select | CapsuleBrowserState | 已选 N、底部三动作 | 选择、滚动、批量动作 | 退出或操作完成 |
| pod-capsules | undo | CapsuleRepository | 五秒撤销提示 | 撤销、继续浏览 | 超时自动消失 |
| pod-device | pairing | BleVoiceService | 六位码和倒计时 | 取消配对 | 超时回设备页 |
| voice-menu | setup | VoiceRuntimeModel | 三项检查逐项显示 | 授权/安装/连接 | 满足后进入 ready |
| voice-menu | listening | VoiceSessionRuntime | 输入状态、帧/丢包 | 停止/恢复麦克风 | 正常尾排空或异常恢复 |
| voice-menu | error | RecoveryCoordinator | 明确错误与恢复按钮 | 重连、重新配对、恢复麦克风 | 成功后 ready |

## 互斥

- 本地 WAV 与 BLE 音频会话互斥。
- 新本地录音、扬声器播放与腾讯工作中的同一胶囊互斥。
- BLE Voice 具有实时优先级；转写和 UI 调度不得阻断触摸。
