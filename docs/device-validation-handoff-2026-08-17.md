# PokePod 真机验证与未关闭问题交接（2026-08-17）

## 1. 结论

当前源码候选能够通过主机门禁、ESP32-S3 编译、受控 ROM 写入、完整回读和
运行身份校验。USB Link 在大量独立打开/关闭 CDC 会话后仍会永久停止响应，
因此日常免按键 OTA、录音、配网和无线语音均不能判定为真机验收通过。

本报告冻结在以下边界：

- 产品代码提交：`9f0847aa5f0e9fff76e9de5cae40c8c1e8a8772b`
- 固件版本：`2.0.0`
- Fast BIN：`2,478,880` bytes（`78.8015%`，yellow）
- BIN SHA-256：
  `2091a420129a673e5f62f0f47b46ea8dcdbc0ca6fa6d284d7784b3d83ea0a0a5`
- App ELF SHA-256：
  `99f89a58166f0c50008871f2d5090085cd286c64d1d67e30e2e5e11bdd92a226`
- Arduino-ESP32：`3.3.8` production profile
- 工作树在写本报告前 clean；报告提交只增加文档和 README 入口。

## 2. 已完成且有证据的事项

### 2.1 主机与构建

- `firmware/run-source-only-tests.sh`：PASS
  - C++ host：115/115
  - source Python contracts：64/64
- ESP32-S3 Fast 强制编译：PASS
- 资源审查绑定 `9f0847aa`、BIN、ELF、map 和上述 SHA：PASS
- 本轮项目代码没有新增编译 warning；日志 warning 来自锁定的
  Arduino-ESP32/ESP_I2S、TinyUSB 和 Arduino_GFX vendor 源码。

### 2.2 ROM 写入闭环

受控 ROM 刷写完成以下步骤：

1. 识别 ESP32-S3、16 MiB flash 和私有设备 authority；
2. 写前备份 app0 3 MiB 区域并抽样复读；
3. 写入 152 个分块；
4. 回读 152 个分块；
5. 回读 BIN SHA 与候选完全一致；
6. 重启后运行分区为 `app0`；
7. 运行态 `sourceRevision`、`firmwareVersion`、`appElfSha256` 与候选完全一致。

本地证据目录（默认被 Git 忽略）：

- `projects/pokepod-amoled/work/fixture-runs/20260817-120604-flash/`
- `projects/pokepod-amoled/work/hardmac-runs/20260817-200604-flash.UauFDO/`

### 2.3 启动与能力事实

刷入 `9f0847aa` 后，首次 `identity`、`status`、`link-probe` 均成功。状态显示：

- SD ready：true
- CapsuleLibrary ready：true
- Recorder ready：true
- ASR worker ready：true
- Audio ready：true
- USB ready：true
- PSRAM free：约 7.5 MiB
- 首次大状态响应后的 internal heap free：22,076 bytes
- 首次大状态响应后的 largest internal block：8,692 bytes

这些事实证明本次启动没有落入“SD 未挂载”“胶囊索引未建立”或“PSRAM
降级启动”。它们不证明一次真实录音事务、SoftAP 配网或 BLE 音频会话成功。

## 3. 已实施但未能根治的 USB Link 修复

当前分支依次包含以下 USB/Link 收敛工作：

- `a87954eb`：使用 Arduino-ESP32 自有 `tx_lock` 的有界 CDC 写入；
- `d5743676`：删除跨任务直接清 TinyUSB RX FIFO；
- `25b6ac97`：无存储时仍保留 USB 诊断、补 DTR/RTS 会话收口；
- `c789eeb1`：把 USB Link 启动提前到本地存储恢复之前，之后原位 attach storage；
- `9f0847aa`：复用一块 4 KiB Link status 缓冲，删除每次状态查询的大块
  `String` 分配与转义临时副本。

这些改动带来真实改善：早期固件在屏幕和 USB 端口已经出现时，Link 仍可能
因为存储恢复尚未结束而完全不可达；`c789eeb1` 刷入后，启动阶段身份、状态和
探针立即可用。重复会话永久失联仍然存在。

## 4. 首要未关闭问题：USB Link 重复会话后永久失联

### 4.1 可重复现象

测试每轮依次启动四个独立主机进程：

1. `hello`
2. `identity`
3. `status`
4. `link-probe`

每个进程都会独立打开 CDC、断言 DTR/RTS、完成一条 Link v2 请求、撤销
DTR/RTS 并关闭端口。

`9f0847aa` 真机结果：

- 20 轮 / 80 次独立会话：PASS
- 第 27 轮前三条请求成功
- 第 27 轮 `link-probe` 超时
- 首次失败发生在第 107 次独立会话
- 等待两秒后重新执行 `identity` 与 `link-probe` 仍超时
- USB 产品端口持续枚举，屏幕和主循环仍在运行

失败前观测值：

- internal heap free 最低值始终为 22,076 bytes
- largest internal block 最低值始终为 8,692 bytes
- 前 80 次会话累计 USB write attempts：1,502
- write progress：与 attempts 同步
- write would-block：0
- write disconnected：0

上一版 `c789eeb1` 曾在约 131 次独立会话后同样永久失联。`9f0847aa` 没有提高
稳定性，所以 status `String` 碎片只能保留为已消除风险，不能作为根因结论。

### 4.2 当前最高优先级根因候选

Arduino-ESP32 3.x 的 `USBCDC::_onLineState()` 同时承担两项职责：

- 维护普通 CDC `connected`；
- 当 `enableReboot(true)` 时，把 DTR/RTS 的多步组合解释为进入 bootloader 的
  控制序列。

在 reboot 状态机离开 idle 的中间阶段，core 不会发布普通
`ARDUINO_USB_CDC_LINE_STATE_EVENT`。macOS 打开/关闭串口时，DTR 与 RTS 的实际
到达顺序可以经过 `!DTR+RTS`、`DTR+RTS` 等中间状态。当前代码存在以下交叉点：

- `UsbLinkBridge::begin()` 对 Link CDC 调用 `cdc_.enableReboot(true)`；
- `cdc-status.py` 每条请求都显式同时设置/清除 DTR 与 RTS；
- `UsbCdcSessionState` 只用 DTR 建立 generation，不要求 DTR 与 RTS 同时为真；
- `UsbLinkBridge` 同时订阅 DISCONNECTED 与 LINE_STATE，单次物理关闭可能产生
  两类异步事实；
- App 主循环和 Link parser 又各自执行一次旧 epoch 收口。

因此需要优先审查：正常串口打开序列是否偶发进入 Arduino reboot 控制状态机，
使新会话的 LINE_STATE 被吞掉，`hostSessionActive()` 永久保持 false。这个候选
符合“端口存在、主循环活着、没有 write would-block、后续所有请求都无响应”。

这仍是候选，需要用事件级真机遥测证明。禁止直接把它写成已确认根因。

### 4.3 下一轮建议的最小实验矩阵

外部审查应先给出状态机裁决，再修改。建议按以下顺序，只改变一个变量：

1. 在 RAM 环形诊断中记录每个 CDC event：event id、DTR、RTS、core connected、
   local generation、closed generation、Link-bound generation；
2. 保持当前 firmware，只跑 identity-only 200 次，保存首次异常前后的事件序列；
3. host 不主动操作 modem lines，仅依赖 fd open/close，跑同样 200 次；
4. device 对专用 Link CDC 禁用 `enableReboot(true)`，跑同样 400 次；
5. device session active 改为 `DTR && RTS`，再跑同样 400 次；
6. 每个候选必须额外通过一次大文件 OTA、一次 OTA 后再次 OTA；
7. 若 Link 仍失联，保留屏幕亮着的现场，通过 JTAG/独立日志端读取 USB event
   ring，避免 RESET 清除证据。

日常免按键 OTA 的通过条件：

- 连续 400 次独立 Link 会话全部成功；
- 静置超过自动省电/屏幕超时后 Link 可再次唤醒；
- 一次运行态 OTA 成功并验证新分区身份；
- OTA 后再完成第二次运行态 OTA；
- 任何一步失败都不能宣称“最后一次手动 ROM”。

自动 ROM 救援需要电脑侧夹具真正控制 BOOT、RESET 和电源。当前普通 USB
`1200 baud` 尝试没有让本机进入 ROM，不能作为自动救援能力。

## 5. 其他仍未关闭的真机问题

### 5.1 本地语音胶囊录音

用户可见现象：

- 点击录音先显示“正在检查存储”；
- 随后出现“录音失败，内容未提交”或存储性能不足；
- 曾出现“录音存储不可用”，稍后又恢复为“轻触录音”。

当前已知：本次启动 status 中 SD、CapsuleLibrary、Recorder 全部 ready。这个事实
只证明启动能力，不证明录音准入的容量查询、写性能资格、I2S capture、异步
finalize 和胶囊事务成功。`9f0847aa` 尚未完成一次用户等价的真机录音闭环。

下一轮必须保存：

- storage qualification decision、mount generation、probe throughput/tail；
- capture start terminal、I2S error/timeout/early-zero；
- recorder queue、short write、close/finalize；
- transaction commit/rollback/cleanup；
- 最终 WAV 可播放、胶囊索引可见、重启后仍存在。

### 5.2 手机配网 / SoftAP

用户可见现象：进入手机配网后设备立即重启或界面失去响应，退出按钮不可用；
固定口令设置曾仍显示随机口令。

源码已经有配网阶段日志、运行 trace、跨重启 terminal ring、固定/随机口令配置。
`9f0847aa` 尚未在当前真机上重复一次完整 SoftAP 开启、页面访问、保存、退出流程。
需要从自然应用态采集“点击前 → mode AP → softAP → DNS/HTTP → 退出/重启”的
连续证据，并区分 watchdog、panic、brownout 和主动 reboot。

### 5.3 BLE / Mac 微信语音输入

用户可见现象：Mac 显示蓝牙、BlackHole 2ch、辅助功能均 ready；按住微信语音
输入时麦克风只闪一下，随后报“音频帧超时”。历史状态曾显示 BLE connected、
MTU 517、app ready，但 Mac 收到并解码帧仍为 0。

这说明 UI ready 事实不足以证明音频帧实际送达。`9f0847aa` 刷入后的首次状态中
BLE 尚未连接，本轮没有执行真实按住说话会话。下一轮需要把设备 capture、BLE
notify、Mac receive/decode、BlackHole write 和 StopAck 统一到同一 session id
时间线；至少收到并解码非零帧，才能关闭该问题。

### 5.4 BOOT 按键与自动升级

源码有 BOOT 单击/长按策略和夹具控制接口。当前恢复过程仍多次依赖用户手工
BOOT+RESET。普通应用 Link 失联时，现有软件没有可靠通道让电脑让设备进入 ROM；
因此“插线即可自动恢复”尚未实现。应用 Link 健康时的 OTA 与应用已经卡死时的
ROM 救援必须分别验收。

## 6. 审查者需要回答的问题

1. `enableReboot(true)` 是否应该从专用 Link CDC 移除，并把 ROM 救援完全交给
   夹具 BOOT/RESET？
2. session active 是否必须严格绑定 `DTR && RTS`，并且只消费一种关闭事实？
3. DISCONNECTED 与 LINE_STATE 的双事件如何携带不可混淆的 generation？
4. App reconcile 与 parser magic reconcile 是否存在双重 teardown；能否归并为
   一个单所有者状态机？
5. host 是否应保持一个长期 CDC 连接，把多条请求复用在同一 transport session，
   同时把“频繁 open/close”保留为独立可靠性测试？
6. 如何在不依赖已卡死 Link 的情况下导出最后 128 条 USB event / runtime trace？
7. 录音、配网、BLE 三条真机矩阵应如何在 Link 修复后自动串联，并在任一重启后
   自动回收跨重启诊断？

## 7. 当前发布与验收边界

- 可以审查源码、主机测试、Fast 编译和 ROM 写入身份闭环。
- 当前固件不能作为 Release 或真机验收通过件。
- 当前不能承诺日常升级无需按键。
- 当前不能声称本地录音、SoftAP 配网或 Mac 无线语音已解决。
- 下一次产品代码修改应先解决并证明 USB Link 会话生命周期，再恢复完整设备矩阵。

