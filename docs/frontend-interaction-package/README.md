# PokePod Wireless Voice 前端交互包

本包冻结 PokePod 368×448 AMOLED 与 `PokePod Voice.app` 菜单栏界面的核心任务、输入仲裁、状态、视觉语法、组件边界和验收证据。实现范围以用户确认的“无线语音与胶囊管理重构”为准。

核心 Surface：

- `pod-home`：本地语音胶囊与无线微信语音输入。
- `pod-capsules`：范围、详情、多选、归档、回收站与恢复。
- `pod-device`：BLE 语音、USB 同步、Wi-Fi 与配对状态。
- `voice-menu`：Mac BLE、BlackHole、辅助功能与异常恢复。

机器可读入口：`surface-registry.json`、`frontend-contract.json`、`frontend-readiness.json`。
