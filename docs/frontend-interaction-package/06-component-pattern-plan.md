# 组件与设计系统

## design-system-contract

- Typography：CJK 正文字形使用现有 A4 灰度资源；状态 16/20，正文 20，标题 28，计时 36；不缩放点阵字体。
- Spacing：4/8/12/16/24/32；页面水平 24，行内 12，主要分组 20。
- Color：AMOLED 黑 `#000000`；surface `#0B1514`；primary `#42E8C3`；voice `#A9B8FF`；warning `#FFC64A`；danger `#FF6B77`；主文字 `#F4F7F6`；次文字 `#8EA09C`。
- Touch：可见控件最小 48×48；危险动作与相邻动作至少 12 px；边缘手势带 24 px 安全区。
- Motion：120–180 ms 状态过渡；录音/无线输入用声音包络驱动波形和 1.6 s 呼吸环；减少动态时只保留幅度反馈。
- Slots：设置/列表统一 `leading/content/accessory/feedback`，同一槽位不堆叠两个对象。

## 组件清单

- `StatusRail`：电量、转写、BLE、USB；只读共享运行状态。
- `ActionCard`：首页两张等高动作卡；输入仲裁由 `UiInputArbiter` 统一负责。
- `VoicePulse`：本地/无线共用包络和呼吸呈现，不持有业务状态。
- `CapsuleRow`：标题、时间、状态、收藏、选择；只读 `CapsuleBrowserState`。
- `ScopePicker`：范围覆盖层，出现时隔离底层命中。
- `SelectionBar`：按范围提供收藏、归档/恢复、删除/恢复。
- `SettingsRow`：固定四槽布局，图标和标题基线统一。
- `ReadinessCheck`：Mac 三项依赖状态和动作。
- `RecoveryActions`：重连、重新配对、恢复麦克风；调用平台适配层。

## 状态所有权

固件业务状态由 `VoiceSessionController`、`CapsuleRepository`、`CapsuleBrowserState` 持有；渲染层不直接移动文件或发送 BLE。Mac 由 `VoiceRuntimeModel` 组合 BLE、音频、快捷键和恢复协调器。
