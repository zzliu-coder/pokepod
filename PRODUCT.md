# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Users

PokeCapsule 是用户自用的语音收件箱。用户会在 BOOX Poke3 或普通 Android 手机上随手录下一段中文想法，稍后在设备或 Mac 上阅读、编辑和整理。

## Product Purpose

让“按一下、说一句、以后再处理”足够可靠。成功意味着录音不会丢失，转写能够在联网时自动完成，文字容易找到和整理，设备断开后仍可在 Mac 上继续管理并在重连后安全同步。

## Positioning

胶囊以普通文件为事实源：原始录音、原始转写、模型校对、用户最终文字和元数据分别保存。任何一层失败都不会覆盖更早的事实，也不要求依赖一个持续在线的云端账户。

## Operating Context

- Poke3：低功耗、黑白墨水屏、慢刷新、无内置扬声器，保留悬浮录音胶囊。
- Android 手机：彩色触屏、移动网络、手机麦克风和扬声器，录音入口位于应用内。
- Mac：通过 USB/ADB 管理一台或多台设备；每台设备有独立镜像、目录和离线操作队列。
- 中文语音由腾讯一句话识别转写；DeepSeek 校对仅在 Mac 上由用户手动触发。

## Capabilities and Constraints

- 单次录音目标上限为 60 秒；腾讯一句话识别要求上传音频严格不超过 60 秒。
- 新录音进入 Inbox，可移动到两级目录，可添加多个标签、收藏、复制、删除和恢复。
- 展示优先级为：用户最终文字、可信校对文字、可信原始转写、处理状态。
- 原始录音永久保留，派生的上传副本可以裁剪或删除。
- 手机和 Poke3 打开应用且联网后处理待转写队列；Android 手机允许移动网络。
- Poke3 的界面必须减少动画、阴影、灰阶层次和不必要的全屏刷新。
- Mac 的离线操作在设备重连后按顺序提交，冲突需要明确呈现。

## Brand Commitments

- 产品名为 PokeCapsule，中文对象名为“胶囊”。
- 语言直接、安静、具体；界面不暴露云厂商错误码、内部状态名或工程术语。
- 胶囊图形是录音入口和应用识别符号。

## Evidence on Hand

- Android、Poke3、Mac 的现有实现与测试位于 `projects/pokecapsule-android` 和 `projects/pokecapsule-mac`。
- 共享文件协议位于 `projects/pokecapsule-protocol`。
- 真实设备截图和诊断材料保存在本机 `work/diagnostics`，不进入 Git。
- 当前没有面向公众的商业声明、客户案例或性能基准；未来界面不得虚构。

## Product Principles

1. 先保住原音，再处理文字。
2. 默认路径只包含录音、转写、整理三个动作。
3. 技术细节留在诊断层，恢复动作留在用户眼前。
4. 三端共享对象与状态语义，交互遵循各自设备能力。
5. 自动化必须可恢复、可追踪、不会静默覆盖。

## Accessibility & Inclusion

所有触控目标至少 48dp；正文支持系统字号。状态不能只依赖颜色表达。Poke3 使用高对比黑白层级，避免依赖动画传递录音或处理状态。
