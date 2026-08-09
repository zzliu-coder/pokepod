# 规格摘要

## 系统定位

PokePod 同时承担独立语音胶囊与无线 Mac 语音终端。USB 只提供 CDC 同步、维护和刷写；实时微信语音经 BLE 传入独立的 `PokePod Voice.app`，由应用写入 BlackHole 并保持 Option+Z。

## 本轮范围

- PokePod 首页始终显示两个等高入口，无线入口按 BLE Voice 就绪状态启用。
- 转写作为后台任务，浏览、设置和无线输入保持可用。
- 胶囊页增加范围、归档/移回、回收站/恢复、五秒撤销和长按多选。
- 设备页增加无线语音配对、忘记 Mac、连接质量和 USB CDC 状态。
- Mac 菜单栏应用提供蓝牙、BlackHole、辅助功能、重连和麦克风恢复。

## 边界

- 存储 schema、Android/Poke3 和腾讯转写格式保持兼容。
- 永久删除、标签、文件夹编辑、全文搜索和正文编辑继续由 Mac/Android 完成。
- 微信输入法没有状态回执，界面只陈述本系统可以确认的 BLE、音频和按键状态。

## 目标与真实性

`target_level` 为 `verified`。静态 artifact 支持开工；主机测试支持软件层结论；BLE、BlackHole、真实微信输入、屏幕观感和 USB 枚举需要真机证据。
