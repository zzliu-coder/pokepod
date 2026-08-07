# PokePod AMOLED

PokePod 把 Waveshare ESP32-S3-Touch-AMOLED-1.8 做成两种设备：

- 独立语音胶囊：录制 `16 kHz / 16 bit / mono WAV`，保存为 PokeCapsule
  schema v2，联网后由腾讯云一句话识别生成 `raw.txt`。
- Mac 有线语音终端：USB 同时提供 48 kHz 单声道麦克风、长按 Option+Z
  键盘输入和 PokePod Link v2 数据同步。

固件在启动时通过触摸控制器地址自动识别硬件：

- I2C `0x38`：V1，SH8601 + FT3168。
- I2C `0x15`：V2，CO5300 + CST820。

同一份固件支持 V1/V2。代码使用 AMOLED、触摸、ES8311 麦克风和扬声器、
SD、RTC、QMI8658、AXP2101、Wi-Fi、USB CDC/UAC/HID；BLE 暂未启用。

## 日常使用

设备有三个横向页面：胶囊列表、首页、设置与设备详情。首页位于中间，每次启动
直接显示首页；左右滑动进入相邻页面，屏幕不保留占空间的底部导航栏。

- 首页点“语音胶囊”开始/停止录音；录音最长 58.5 秒。
- 未连接 Mac 时，BOOT 短按开始/停止胶囊。
- 连接 Mac 时，按住 BOOT 使用微信语音输入，松开结束；胶囊录音使用屏幕按钮。
- 屏幕“微信语音输入”同样是按住说话、松开结束。
- 胶囊详情可阅读 `final.md > polished.md > raw.txt > title`、播放 WAV、
  收藏、归档和重新转写。
- PWR 短按亮屏/息屏，长按安全关机。
- 设置页可开关 Wi-Fi 自动工作、启动五分钟 WPA2 手机配网页、开关抬起亮屏。

Wi-Fi 平时关闭。录音、待转写或充电产生网络需求时自动连接，最后一项工作
结束三分钟后关闭。失败按 10 秒、30 秒、2 分钟重试，随后保留队列等待下次
录音、手动开启或充电唤醒。

## 存储和密钥

逻辑根目录为 SD 卡 `/PokeCapsule`。新录音先进入 `.staging`，WAV 头、
`capsule.json` 和 `processing.json` 完成后通过目录改名提交到 `Inbox`。
掉电后会恢复可验证的完整 WAV；现有胶囊不会被批量迁移。

手机配网页保存 Wi-Fi 和腾讯 SecretId/SecretKey 到 ESP32 NVS。已保存的
SecretKey 不会在网页、Link 状态、日志或 SD 中读回。Link v2 的 `configure`
操作可作为 USB 救援配置通道。首版接受物理拆机读取 Flash 的个人设备风险，
建议使用权限受限、可随时吊销的腾讯云子账号密钥。

配网页会异步扫描附近的 2.4 GHz 网络，按信号强度排序并合并同名热点；用户
点击选择后只需输入密码。隐藏 SSID 仍可通过手工输入框配置，扫描期间热点和
网页保持可用。配置热点密码固定为 `88888888`，只在用户主动开启后的五分钟内
有效。

腾讯请求使用 TLS 证书校验和 TC3-HMAC-SHA256。WAV 以两遍流式方式完成
签名与 Base64 上传，不在内存中保存完整音频或完整请求体。转写在后台任务中
运行，屏幕、按键和 Link 主循环保持响应；录音、转写或 UAC 工作期间，Mac 的
SD 操作会收到可重试的 `busy`。

## PokePod Link v2

CDC 是纯二进制协议通道，帧包含版本、请求 ID、长度和 CRC32。调试日志写入
调试串口，避免污染 CDC。设备实现：

- hello、状态、身份、元数据指纹；
- 分页文件清单和分块读取；
- 分块暂存写入、原子提交、共享管理命令和结果查询；
- 配置、UTC 校时、录音、停止、Option+Z 按下/释放、重启；
- 可选逐块确认，防止大文件超过 TinyUSB CDC 接收窗口；
- 路径穿越拦截、重复请求拦截、传输超时和忙碌重试。

Mac 端通过 `DeviceTransport` 共用镜像、离线队列和 DeepSeek 回写逻辑；
PokePod 使用 `PokePodTransport`，Android/Poke3 使用 `ADBTransport`。设备不启用
USB Mass Storage，避免 Mac 与固件同时写 SD。

首次准备 SD 卡时，把完整中文字库通过 Link v2 安装到设备：

```sh
./cdc-status.py --install-font assets/cjk20.a4
./cdc-status.py --command reboot
```

固件会校验 PKF2 文件头、20 px 原生字号、4-bit 灰阶、字形数量和总长度，再原子
替换 `/PokeCapsule/.system/fonts/cjk20.a4`。Flash 内始终保留 16 / 20 / 28 /
36 px 固定界面字形；完整字库负责显示腾讯云和 DeepSeek 返回的任意中文正文。
字形由 OFL 授权的 Noto Sans CJK SC Medium 生成，授权文件位于 `assets/OFL.txt`。
最终页面组合预览位于 `design/ui-v3-compositions.svg`。

## 构建和自动测试

```sh
./firmware/run-host-tests.sh
./firmware/build.sh
./verify.sh
```

`build.sh` 固定 Waveshare 源码版本，并使用 Arduino-ESP32 3.3.8。产物位于
`work/pokepod-build/output`。`verify.sh` 运行固件主机测试、干净固件编译、
Mac 测试和 release build、脚本语法检查及 diff 检查。

设备正常运行并通过 USB 连接时，刷写只需一个命令：

```sh
./flash.sh
```

应用 CDC 在线时，脚本会用 1200 波特率自动进入 ROM 下载器；写入和校验后使用
ESP32-S3 原生 USB 所需的 watchdog 系统复位自动回到应用。日常刷写无需按键。
只有应用 CDC 与 ROM 端口都未出现时，才使用一次 `按住 BOOT → 短按 RESET → 松开
BOOT` 作为救援入口，然后重新运行同一个脚本。

从一台已通过 ADB 连接、且 Android PokeCapsule 已配置腾讯云的设备安全迁移密钥：

```bash
./provision-pokepod.py --port /dev/cu.usbmodemXXXXXXXX
```

迁移过程只在内存和 Android 临时暂存文件中处理密钥，完成后立即删除暂存文件，终端只输出配置状态。若同时配置 Wi-Fi，使用 `--wifi-ssid`，密码通过交互输入或 `POKEPOD_WIFI_PASSWORD` 环境变量提供，避免密码进入命令历史。

脚本会通过 CDC 自动进入 ESP32-S3 ROM 下载器，只刷新 `0x10000` 的应用分区，
验证 Flash 内容后回到应用。它不会覆盖 NVS、分区表、SD 卡或原始 16 MB 备份。
若旧固件已损坏，脚本会提示唯一的人工恢复动作：按住 BOOT，短按一次 RESET，
松开 BOOT，再重跑脚本。

真机连接后：

```sh
./device-acceptance.sh
./end-to-end-acceptance.sh
```

`cdc-status.py` 使用真实 Link v2 帧读取设备状态。真机门检查 V1 显示、触摸、
IO 扩展器、RTC、IMU、PMU、SD、音频和 USB 状态，
UAC 48 kHz 单声道连续采集、麦克风非静音和 USB/I2S 计数器。端到端门长按
Option+Z，确认微信输入法在按住期间打开 UAC，并在释放后关闭。最终文字进入
真实输入框仍保留一次人工确认。

## Mac 设置

1. 把微信语音输入法“按住说话”快捷键设为 Option+Z。
2. 选择 **TinyUSB UAC1**（制造商 **PokeCapsule**，48 kHz）作为输入麦克风。
3. macOS 系统听写使用另一组快捷键，例如连按两下 Control，避免抢占 Option+Z。

Option+Z 的按下和释放由 PokePod HID 直接发送，PokeCapsule 不参与快捷键中转，不需要辅助
功能或输入监控权限。

## 恢复边界

正式刷写前必须保留两次逐字节一致的原始 16 MB Flash 备份，并记录安全状态。
合并固件从偏移 `0x0` 写入；设备进入 ROM BOOT 模式后可从同一偏移恢复原镜像。
主机测试和 clean build 只能证明软件候选成立，不能替代真实 CDC、UAC、SD、
腾讯返回、扬声器、触摸与电源管理验收。
