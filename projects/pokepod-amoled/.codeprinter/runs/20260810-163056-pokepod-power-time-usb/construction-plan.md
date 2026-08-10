# PokePod 电源、校时与 USB 状态迭代

## 目标

- 每次 Wi-Fi 重新连接并完成 SNTP 后刷新系统时钟与 RTC。
- 将现有 `raiseToWake` 兼容字段统一解释为“自动亮屏”，同时控制抬起与触摸唤醒；关闭后仅实体键可亮屏。
- 空闲 Wi-Fi 不再把 CPU 固定在 240 MHz，电池空闲保活缩短为 30 秒；息屏 light sleep 降低定时唤醒频率。
- AMOLED 在无操作后先降亮度，再按原 30 秒策略息屏。
- BLE 连接空闲使用低功耗连接参数，语音会话切回低延迟参数。
- USB 顶栏图标同时要求 TinyUSB mounted 与 PMU VBUS 在位，拔线后可靠变灰；CDC DTR 会话释放逻辑保持独立。

## 非目标

- 不更改胶囊存储协议、Link v2、Mac、Android。
- 不启用未经当前预编译 ESP-IDF SDK 支持的 BLE modem sleep 配置。
- 不引入深睡眠重启式待机，避免尚未真机验证的唤醒源破坏日常操作。

## 验收

- 完整 host suite 通过，新增校时 generation、自动唤醒、电源策略、BLE 参数与 USB 物理状态测试。
- 固定中文字库重新生成并零缺字。
- fresh clean build；Flash/RAM 均低于 75%，项目源码零 warning。
- 独立只读复核后提交；真机未连接时明确保留刷写/功耗实测边界。

## Truth Gate / 真实性闸门

| 核心能力 | truth_evidence | claim_limit |
|---|---|---|
| 每轮 Wi-Fi 校时 | generation/revision host test、SNTP callback 静态合同、fresh build | host-verified；未在真实热点重复断连验证 |
| 自动亮屏与省电策略 | power policy、runtime wake contract、fresh build | host-verified；真实待机电流 unverified |
| USB 图标拔线熄灭 | TinyUSB mounted + PMU VBUS policy test、SDK 静态合同 | host-verified；真机拔插 unverified |
| 固件可刷写性 | fresh clean build、资源与产物哈希 | build-verified；未刷入设备 |

claim_limit: 本轮可以声称代码与主机候选已产出；不能声称真机功耗、抬起/触摸唤醒或 USB 图标已经验收。
