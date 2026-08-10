# PokePod AMOLED

PokePod 把 Waveshare ESP32-S3-Touch-AMOLED-1.8 做成两种设备：

- 独立语音胶囊：录制 `16 kHz / 16 bit / mono WAV`，保存为 PokeCapsule
  schema v2，联网后由腾讯云一句话识别生成 `raw.txt`。
- Mac 无线语音终端：BLE 发送实时压缩音频给独立的 PokePod Voice.app；PokePod
  Link v2 可通过 USB CDC 或经过 TLS 与双向 HMAC 认证的局域网通道同步胶囊。
  USB 同时保留首次无线配对、维护和刷写。

固件在启动时通过触摸控制器地址自动识别硬件：

- I2C `0x38`：V1，SH8601 + FT3168。
- I2C `0x15`：V2，CO5300 + CST820。

同一份固件支持 V1/V2。代码使用 AMOLED、触摸、ES8311 麦克风和扬声器、
SD、RTC、QMI8658、AXP2101、Wi-Fi、BLE 和 USB CDC。

## 日常使用

设备有三个横向页面：胶囊列表、首页、设置与设备详情。首页位于中间，每次启动
直接显示首页；左右滑动进入相邻页面，屏幕不保留占空间的底部导航栏。

- 首页点“语音胶囊”开始/停止录音；录音最长 58.5 秒。
- PokePod Voice.app 未就绪时，BOOT 短按开始/停止胶囊。
- PokePod Voice.app 就绪时，按住 BOOT 使用微信语音输入，松开结束；胶囊录音使用屏幕按钮。
- 屏幕“微信语音输入”同样是按住说话、松开结束。
- 胶囊详情可阅读 `final.md > polished.md > raw.txt > title`、播放 WAV、
  收藏、归档和重新转写。
- PWR 短按亮屏/息屏，长按安全关机。
- 设置页可开关 Wi-Fi 自动工作、启动五分钟 WPA2 手机配网页、开关抬起亮屏。

Wi-Fi 平时关闭。录音、待转写或充电产生网络需求时自动连接，最后一项工作
结束三分钟后关闭。失败按 10 秒、30 秒、2 分钟重试，随后保留队列等待下次
录音、手动开启或充电唤醒。

设备采用统一低功耗策略：普通界面使用 80 MHz，录音、无线语音、配网、网络、
存储和 Link 事务临时切到 240 MHz；30 秒无操作后 AMOLED 执行面板休眠，音频
I2S 与 ES8311 只在录音或播放期间上电。屏幕关闭且 USB、供电、BLE 连接和后台
工作均为空闲时，设备进入带 100 ms 定时兜底的短周期 light sleep。当前固定的
Arduino-ESP32 3.3.8 SDK 没有编译自动 DFS 和 BLE controller modem sleep，状态
接口会如实把这两项报告为不支持，不能据此推算固定的续航倍数。

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

PokePod 本机“连接手机”页直接显示扫描、连接、验证、保存、成功或失败状态。
点击“诊断记录”可查看最近 16 条配网阶段、SSID、信号、耗时和 802.11 失败原因；
记录以 CRC 环形 NVS blob 保存，重启后仍可读取。Link v2 同时提供
`get-provisioning-diagnostics` 和 `clear-provisioning-diagnostics`。诊断记录不
保存 Wi-Fi 密码、腾讯密钥、音频或请求正文。

腾讯请求使用 TLS 证书校验和 TC3-HMAC-SHA256。WAV 以两遍流式方式完成
签名与 Base64 上传，不在内存中保存完整音频或完整请求体。转写在后台任务中
运行，屏幕、按键、BLE 和 Link 主循环保持响应；本地录音占用麦克风或文件提交
期间，Mac 的 SD 操作会收到可重试的 `busy`。

## PokePod Link v2

CDC 是纯二进制协议通道，帧包含版本、请求 ID、长度和 CRC32。调试日志写入
调试串口，避免污染 CDC。设备实现：

- hello、状态、身份、元数据指纹；
- 分页文件清单和分块读取；
- 分块暂存写入、原子提交、共享管理命令和结果查询；
- 配置、UTC 校时、录音、停止和重启；
- 可选逐块确认，防止大文件超过 TinyUSB CDC 接收窗口；
- 路径穿越拦截、重复请求拦截、传输超时和忙碌重试。

设置页的“与电脑同步”会打开一个从点击时刻计算、绝对截止为五分钟的局域网
窗口。窗口内设备通过 Bonjour 发布临时实例，TCP 连接先完成 TLS，再使用经 USB
导出的配对密钥做双向 HMAC 认证。截止时监听、Bonjour 和现有无线会话一起关闭，
认证和传输活动都不会延长窗口。

首次配对由 Mac 通过 USB 调用 `pairing-export`。返回的 bundle 使用设备现有永久
`pokepod-...` identity，避免产生第二个资料库身份；该操作在无线通道会被拒绝。
递归文件清单为 `audio.m4a` 和 `audio.wav` 返回小写 64 位 SHA-256，Mac 只有在
UUID、文件名、长度和摘要全部一致时才复用本地音频。

Mac 端通过 `DeviceTransport` 共用镜像、离线队列和 DeepSeek 回写逻辑；
PokePod 使用 `PokePodTransport`，Android/Poke3 使用 `ADBTransport`。设备不启用
USB Mass Storage，避免 Mac 与固件同时写 SD。

### Android / PokePod 胶囊语义

Android、Poke3 和 PokePod 不互相直连。它们各自写入同一套胶囊目录与 schema，
再由 Mac 统一汇总。维护边界固定如下：

| 共享语义 | PokePod 小屏 | Mac / Android |
| --- | --- | --- |
| v1/v2 processing、WAV/M4A、状态和 revision | 读取并保护未知版本 | 完整读取和迁移 |
| 收藏、归档、回收站、恢复 | 单条和长按多选 | 单条和批量 |
| 永久删除 | 仅回收站，二次确认，事务暂存后删除 | 仅回收站，二次确认 |
| 未知 schema 或损坏元数据 | 可浏览，所有写操作只读 | 提示升级或修复 |
| 标签、搜索、复制、文件夹和正文编辑 | 不进入设备界面 | 完整管理 |

永久删除先把整批胶囊移入 `.staging/purge-*`；暂存未全部完成时回滚，全部
完成后才清理文件。掉电后已提交的暂存残留由启动清理处理。

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

日常修改固件后可以复用编译缓存：

```sh
POKEPOD_INCREMENTAL=1 ./firmware/build.sh
```

脚本只在 NimBLE 配置内容真的改变时更新时间戳，避免 ESP32 核心和整套显示库
被误判为需要重编。当前机器上相同源码的重复构建由约 283 秒降到约 46 秒；
正式交付仍使用默认的 clean build。

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

真机连接后，CDC 与板载外设验收：

```sh
./device-acceptance.sh
```

`cdc-status.py` 使用真实 Link v2 帧读取设备状态。真机门检查 V1 显示、触摸、
IO 扩展器、RTC、IMU、PMU、SD、音频和 USB CDC 状态。BLE 音频、BlackHole、
Option+Z 与微信输入法的端到端验收由 PokePod Voice.app 的验收流程完成；最终
文字进入真实输入框仍保留一次人工确认。

## 无线语音

1. 安装 BlackHole 2ch 和 PokePod Voice.app。
2. 把微信语音输入法“按住说话”快捷键设为 Option+Z。
3. 在设备页进入两分钟配对模式，由 PokePod Voice.app 完成安全配对。
4. 菜单栏状态显示“就绪”后，在首页或 BOOT 上按住说话、松开结束。

设备页轻触“无线语音”可开始或取消配对，并显示六位配对码和当前 MTU；长按
该行可忘记已经授权的 Mac。设备只保留一个绑定。

PokePod Voice.app 接收 `16 kHz / mono / IMA ADPCM` 音频，临时把默认输入切换为
BlackHole，并负责 Option+Z 的按下、释放和异常恢复。PokeCapsule 不参与该链路。

## 恢复边界

正式刷写前必须保留两次逐字节一致的原始 16 MB Flash 备份，并记录安全状态。
合并固件从偏移 `0x0` 写入；设备进入 ROM BOOT 模式后可从同一偏移恢复原镜像。
主机测试和 clean build 只能证明软件候选成立，不能替代真实 CDC、BLE、SD、
腾讯返回、扬声器、触摸与电源管理验收。
