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
- 设置页保留五行：无线网络行左侧进入五分钟 WPA2 手机配网、右侧控制 Wi-Fi 自动工作；蓝牙行左侧进入配对详情、右侧控制蓝牙总开关；电脑同步、自动亮屏保持独立；底部“关机”先二次确认，再沿安全关机状态机保存录音、收口转写和存储后断电。关闭蓝牙会安全结束正在进行的无线语音、停止广播并断开 Mac，同时保留配对关系。

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
网页保持可用。每次启动配置热点都会生成新的 10 位非混淆随机密码，只显示在设备
屏幕上。配网窗口从用户主动开启时起五分钟内有效；窗口关闭或到期后，该密码
立即失效并从内存中清除。更换或清空已保存的腾讯云密钥时，还必须按设备实体键
确认。
高级设置提供个人设备固定口令模式，启用前必须在配网页面明确确认；默认仍使用随机
口令。固定模式只适合个人设备，不应作为共享设备或公开部署的默认策略。

PokePod 本机“连接手机”页直接显示扫描、连接、验证、保存、成功或失败状态。
点击“诊断记录”可查看最近 16 条配网阶段、SSID、信号、耗时和 802.11 失败原因；
记录以 CRC 环形 NVS blob 保存，重启后仍可读取。Link v2 同时提供
`get-provisioning-diagnostics` 和 `clear-provisioning-diagnostics`。诊断记录不
保存 Wi-Fi 密码、腾讯密钥、音频或请求正文。

电源诊断使用另一个 CRC 环形 NVS blob，记录启动、休眠阻塞、轻睡眠唤醒或
错误、深睡眠意图、安全关机，以及触摸中断、IMU 中断、姿态算法导致的自动
亮屏。记录只在状态变化和受控抽样点写入，不会在主循环持续磨损 Flash，也不
包含凭据或胶囊正文。设备重新接入 Mac 后读取：

```sh
./cdc-status.py --command get-power-diagnostics
```

脚本会把 `blockerMask` 自动解码为 `blockers`。它指出 Wi-Fi、BLE、USB、
VBUS、音频、同步、配网、UI 动画、自动亮屏或等待超时中的真实阻塞项。确认已保存
诊断后可用 `--command clear-power-diagnostics` 清空。

录音、手机配网和无线麦克风还会写入一份独立的跨重启运行诊断环形记录。
它保存最近 12 个阶段快照：阶段开始/完成或失败、关键错误码、内部堆、最大
连续块、PSRAM、运行时间和重启原因；不保存音频、Wi-Fi 密码、腾讯密钥或请求正文。
插回 USB 后可读取：

```sh
./cdc-status.py --command get-runtime-diagnostics
```

确认记录已导出后可用 `--command clear-runtime-diagnostics` 清空。录音故障重点看
`recording_storage_reserve`、`recording_capacity`、`recording_probe_*`、
`recording_metadata` 和 `recording_audio_open`；配网故障重点看
`provisioning_quiesce_*`、`provisioning_mode_*`、`provisioning_softap_*`；
“无线麦克风暂时不可用”重点看 `wireless_router_acquire`、
`wireless_capture_start`、`wireless_session_start`、`audio_capture_status` 和
`wireless_capture_terminal`。无线采集启动后不再同步写 NVS，终态会明确记录读取次数、
I2S 超时、最慢读取、环形队列高水位和丢帧数。

录音存储性能探针只计量 SD 的 4 KiB 写入与 flush；NVS 诊断在计量和临时文件清理
结束后一次性写入。这样“存储卡性能不足”代表 SD 实测结果，不会混入内部 Flash
诊断耗时，也不会在一次探针中反复磨损 NVS。

手机配网进入 AP 前会先结束 BLE 会话、确认物理断开并释放完整 BLE controller 内存，
随后才执行 `WiFi.mode(WIFI_AP)`。配网结束后设备通过统一重启协调器恢复 BLE；用户
退出配网或 USB/Wi-Fi Link 断开不会取消已经接受的重启意图。

腾讯请求使用 TLS 证书校验和 TC3-HMAC-SHA256。WAV 以两遍流式方式完成
签名与 Base64 上传，不在内存中保存完整音频或完整请求体。转写在后台任务中
运行，屏幕、按键、BLE 和 Link 主循环保持响应；本地录音占用麦克风或文件提交
期间，Mac 的 SD 操作会收到可重试的 `busy`。

## PokePod Link v2

### 电脑夹具与无需复位的升级

夹具把设备端的连接做成六类受保护触点：USB D+/D−、GND、受限 VBUS、BOOT 和
RESET。电脑端负责身份核验、诊断导出、备份、升级、回读和证据归档；设备不需要
增加体积较大的调试接口。触点定义和电气约束见 `fixture/pinout.json`。

普通升级直接使用已启动固件的 USB CDC Link v2，不需要用户按 BOOT/RESET：

```sh
python3 fixture/pokepod-fixture.py probe --port /dev/cu.usbmodemXXXX
python3 fixture/pokepod-fixture.py collect --port /dev/cu.usbmodemXXXX
python3 fixture/pokepod-fixture.py update \
  --port /dev/cu.usbmodemXXXX \
  --firmware work/pokepod-build/output/fast/PokePodAmoled.ino.bin
```

这条路径就是设备的应用内 OTA：主机先完成设备身份、镜像长度、SHA-256 和三项构建身份预检，
固件把镜像分块写入未运行的 OTA 槽，最后由设备级重启协调器在 USB 断开后继续完成
收口。准备阶段或任一分块被拒绝时，`cdc-status.py` 会保留设备返回的完整 JSON 错误，
便于区分忙、分区容量、身份和校验失败。首次升级前仍建议用 BOOT/RESET 救援刷写一份
具备 USB OTA 的固件，之后再用这条命令滚动升级。

主机先计算镜像 SHA-256，并发送 `sourceRevision`、`firmwareVersion`、`appElfSha256`。
`cdc-status.py --firmware` 在打开串口前调用正式刷写使用的同一个 artifact validator，
要求 BIN 同目录的 `artifact.json` 和真实 ELF，并校验 lane、工具链、source tree、资源门禁和 ELF 摘要。
显式三项身份参数只能与清单交叉确认，不能代替或覆盖清单。
设备拒绝缺字段的 OTA 请求；固件把镜像流式写入未运行的 OTA 槽，逐块确认后从候选
槽的 ESP 应用描述读取真实 ELF SHA。只有源码版本、固件版本、候选 ELF SHA、长度、
分区和 SHA-256 全部通过后才调用 `esp_ota_end` 并切换启动槽。提交后的重启属于设备级生命周期请求，
USB 断开不会取消它。失败会中止 OTA 句柄并保留当前启动槽。升级要求精确构建产物，
工具会把请求、响应、诊断和摘要保存到 `work/fixture-runs/`。

BOOT/RESET 仍保留给救援刷写。救援路径调用 `flash.sh`，写前备份 app0 并复读抽样，
写后完整回读比较 SHA；身份、芯片、容量、备份或回读任一不满足就停止：

```sh
python3 fixture/pokepod-fixture.py flash \
  --rom-port /dev/cu.usbmodemROM \
  --authority work/device-authority.json \
  --mode fast
```

夹具控制器的 BOOT/RESET 应采用开漏或三态、电平保护和 VBUS 限流。参考控制器固件、
固定串口桥接协议、控制 profile、自动 ROM 恢复和录音/配网场景复现命令位于
`fixture/`，详见 `fixture/README.md`。正常 USB 升级
不依赖控制器，BOOT/RESET 只作为恢复通道。

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
# 只依赖源码审计包、Python、rg 和 C++17 编译器
./firmware/run-source-only-tests.sh

# 完整仓库中的二进制中文字库
./firmware/run-asset-tests.sh

# 已安装且锁定为 3.3.8 的 Arduino-ESP32 / TinyUSB
./firmware/run-toolchain-tests.sh

# 完整仓库回归：依次运行三层，并做源码审计包自举验证
./firmware/run-host-tests.sh
./firmware/build.sh
./firmware/build.sh --release
./verify.sh
```

源码审计 ZIP 故意排除 `assets/cjk20.a4` 和 Arduino SDK，因此它必须能够独立
通过 source-only 门，但不能把缺失资产或工具链伪装成通过。完整仓库的
`run-host-tests.sh` 会无条件运行 source、asset、toolchain 三层；任何一层缺失
都会明确失败。`test-source-audit-package.py` 还会在全新临时 Git 仓库中生成两份
确定性 ZIP，解压后重新运行 source-only 门，并逐文件核对 manifest、SHA-256
和敏感信息扫描结果。

`build.sh` 固定 Waveshare 源码版本，并使用 Arduino-ESP32 3.3.8。快速产物位于
`work/pokepod-build/output/fast`，正式产物位于
`work/pokepod-build/output/release`。两者都带有记录源码版本、输入指纹、工具链、
大小、单槽剩余空间、资源等级和 SHA-256 的 `artifact.json`，并生成
`flash-resource.json`。正式构建还会把 `SOURCE_DATE_EPOCH` 锁定为当前源码 commit
的 Git 时间戳，让编译器的 `__DATE__` / `__TIME__` 在同一 commit 上保持确定；
同一 UTC epoch 还会通过 `POKEPOD_BUILD_EPOCH_UTC` 写入固件，RTC 启动时直接采用
UTC。IDE 手动编译未定义该宏时，继续按中国本地编译时间兼容启动。时间戳缺失或
格式非法会直接阻断构建；Release 还会拒绝调用者传入任何 `--build-property`，
避免覆盖锁定的编译宏。Flash `<75%` 为绿色；`75%–80%` 为黄色，发布前必须检查
map、最大符号、相对基线增量和重复实现；`80%–85%` 冻结非必要功能并专项瘦身；
`>=85%` 构建会阻断发布。两个 3 MiB OTA 槽位分别计算，内部 RAM、PSRAM、连续堆
和任务栈分别验收。快速与正式产物互相不会覆盖。`verify.sh` 运行固件主机测试、干净固件编译、
Mac 测试和 release build、脚本语法检查及 diff 检查。

黄色或橙色候选先在 fast 产物上生成绑定源码 commit 与二进制 SHA-256 的资源审查：

```sh
python3 tools/write-resource-review.py \
  --output work/pokepod-build/resource-review.json \
  --source-revision "$(git rev-parse HEAD)" \
  --binary work/pokepod-build/output/fast/PokePodAmoled.ino.bin \
  --tier yellow \
  --baseline-commit <baseline-commit> \
  --baseline-bytes <baseline-bin-bytes> \
  --nm <xtensa-esp32s3-elf-nm> \
  --elf work/pokepod-build/build-fast/PokePodAmoled.ino.elf \
  --map work/pokepod-build/build-fast/PokePodAmoled.ino.map \
  --duplicate-evidence "legacy duplicate paths removed" \
  --forbidden-symbol-regex "PokePodLinkService::(mutateFavoriteOrTags|trashOperation)"
```

审查文件包含相对基线增量、最大符号和重复实现结论。橙色还必须提供
`--nonessential-features-frozen` 与 `--size-reduction-evidence`。Release 构建和
刷写都会重新校验 commit、二进制摘要、体积和审查内容；证据不匹配时拒绝继续。

日常修改固件直接运行默认的快速构建：

```sh
./firmware/build.sh
```

它使用独立的持久缓存和确定性输入指纹。输入完全相同时直接返回已有产物；源码
变化时只重编受影响对象。Arduino 主 `.ino` 保持为极薄入口，稳定应用主体位于
`PokePodApp.cpp`，避免 Arduino CLI 每次重新生成 `.ino.cpp` 时连带重编整个程序。
构建脚本还会从 Arduino GFX 1.6.5 原始安装自动生成只含 PokePod 所需 21 个文件
的符号链接视图；上游源码仍是唯一来源，构建器不会再扫描和编译两百多个无关
屏幕及总线驱动。

构建机路径由 `arduino-cli` 自己的配置解析，不依赖某个开发者的 home：脚本先读
显式的 `ARDUINO_CLI`，再查找 PATH；macOS 还会通过应用 bundle 标识发现 Arduino
IDE 内置的 CLI。Arduino data/user 目录默认来自 `arduino-cli config get`，GFX
默认位于所解析 user 目录的 `libraries/GFX_Library_for_Arduino`。CI 或非标准安装可
明确覆盖：

```sh
ARDUINO_CLI=/opt/arduino/bin/arduino-cli \
ARDUINO_DATA_DIR=/srv/arduino-data \
ARDUINO_USER_DIR=/srv/arduino-user \
GFX_LIBRARY=/srv/arduino-user/libraries/GFX_Library_for_Arduino \
./firmware/build.sh --fast
```

产品构建只接受已经验证的 Arduino-ESP32 `3.3.8`。其他版本仅用于显式矩阵验证，
必须同时给出开关和版本，产物清单会标记为 `matrix`。Release 构建和烧录前门禁
都会拒绝 `matrix` 产物：

```sh
POKEPOD_CORE_MATRIX=1 POKEPOD_ESP32_CORE_VERSION=3.3.11 \
./firmware/build.sh --fast
```

产物清单继续记录 FQBN、core、vendor、输入指纹和二进制 SHA-256，并明确记录
`0x10000` app-only 写入偏移。构建保持 `app3M_fat9M_16MB` 分区、single-NimBLE
overlay、fast/release 独立缓存；宿主文件大小和 SHA-256 由 Python 标准库生成，
因此 macOS 与 Linux 使用同一条路径。

正式交付运行：

```sh
./firmware/build.sh --release
```

发布构建使用另一套 build path 并强制 `--clean`，不会清掉日常增量缓存。
`./verify.sh` 始终调用这条发布路径。需要忽略指纹、主动刷新日常缓存时使用
`./firmware/build.sh --force`。

交给外部 AI 审查前，从 clean HEAD 生成可重复的仅源码压缩包：

```sh
python3 tools/package-source-audit.py \
  --repo ../.. \
  --output work/source-audits/PokePod-source-audit.zip
```

打包器按 Git commit 读取源码和测试，写入逐文件 SHA-256 清单；固件、构建缓存、
录音、设备备份、日志、内部 `.codeprinter` 施工记录和二进制字库不会进入压缩包。
PokePod 路径存在任何 tracked 或 untracked 改动时，命令会拒绝生成，以免审查对象
和后续构建候选脱节。

设备正常运行并通过 USB 连接时，刷写还必须提供之前只读验明并由使用者确认的
私有设备身份授权文件。授权文件保存在 `work/` 或其他非 Git 目录，至少绑定 Link
`deviceId`、板型、ESP32-S3 eFuse MAC 和 16 MiB Flash；刷写脚本会在备份和首次
写入前重新读取 ROM 身份并逐项回绑：

```sh
./flash.sh --identity-authority work/hardmac-authorities/my-pokepod.json
```

默认命令只接受带有效清单的正式产物。日常迭代明确使用：

```sh
./firmware/build.sh --fast
./flash.sh --fast \
  --identity-authority work/hardmac-authorities/my-pokepod.json
```

授权文件使用 `pokepod.flash-identity-authority` v1，属于单台设备的私有证据，不能
提交到仓库或从当前待刷设备临时自生成。首次建立该文件时，先在应用仍可运行的
只读验明流程中保存 Link identity、板型和 eFuse MAC，再由使用者确认目标设备。
应用已损坏、只能进入 ROM 的救援流程还必须显式传入已确认的端口，例如：

```sh
./flash.sh --release \
  --identity-authority work/hardmac-authorities/my-pokepod.json \
  --rom-port /dev/cu.usbmodemXXXX
```

救援流程不会自动选择“唯一的 usbmodem”，也不会把当前接入设备读到的 MAC 当成
授权依据。缺少授权、MAC 不同、Flash 不是 16 MiB、板型不符或出现多个可读
PokePod 时，都会在设备备份和写入前终止。

脚本确认 ESP32-S3 身份后，每次刷写都会在任何写入前现场读取完整 3 MiB app0
区域（偏移 `0x10000`），保存 `current-app0.bin`、SHA-256 和
`restore-plan.json`。备份或首、中、末抽样复读失败会在任何写入前终止；恢复计划
绑定设备身份，并要求重新确认同一设备的 ROM 端口。标准恢复只按该计划把这份
app0 备份写回 `0x10000`。

刷写固定使用 16 KiB 分块和 115200 波特率，每块最多尝试三次，最后对本次写入的
完整应用二进制逐块回读并比对 SHA-256。项目本地的通用部署配置位于
`.hardmac/workflow.json`；其中不保存当前串口、设备 ID、Wi-Fi 或密钥。

应用 CDC 在线时，脚本会用 1200 波特率自动进入 ROM 下载器；写入和校验后使用
ESP32-S3 原生 USB 所需的 watchdog 系统复位自动回到应用。日常刷写无需按键。
只有应用 CDC 与 ROM 端口都未出现时，才使用一次 `按住 BOOT → 短按 RESET → 松开
BOOT` 作为救援入口，然后重新运行同一个脚本。

从一台已通过 ADB 连接、且 Android PokeCapsule 已配置腾讯云的设备安全迁移密钥：

```bash
./provision-pokepod.py --port /dev/cu.usbmodemXXXXXXXX
```

迁移过程只在内存和 Android 临时暂存文件中处理密钥，完成后立即删除暂存文件，终端只输出配置状态。若同时配置 Wi-Fi，使用 `--wifi-ssid`，密码通过交互输入或 `POKEPOD_WIFI_PASSWORD` 环境变量提供，避免密码进入命令历史。

密钥迁移脚本只通过正在运行的固件执行 Link v2 `configure`，不会进入 ROM 下载器，
也不会刷写固件。固件更新仍单独运行 `flash.sh`；它只在 `0x10000` 写入应用二进制，
因此保留 NVS、分区表、板载 FAT 分区和 SD 卡。若旧固件已损坏，`flash.sh` 会提示
人工进入 ROM 的动作：按住 BOOT，短按一次 RESET，松开 BOOT，再重跑脚本。

真机连接后，CDC 与板载外设验收：

```sh
./device-acceptance.sh \
  --expected-variant v1 \
  --evidence-root /path/to/evidence \
  --port /dev/cu.usbmodemXXXXXXXX \
  --artifact /path/to/PokePodAmoled.ino.bin
```

必须显式指定 V1 或 V2、证据目录和串口；脚本不会自动选择串口，也不会刷机。
`cdc-status.py` 使用真实 Link v2 帧读取组件预检状态。预检只证明组件存在，行为
场景继续记录为 `unverified`，直到对应 V1/V2 完成本地录音、Link USB/Wi-Fi、
BLE、SD 故障、启动恢复、功耗、显示和触摸证据。两个板型保存独立 evidence。

## 无线语音

1. 安装 BlackHole 2ch 和 PokePod Voice.app。
2. 把微信语音输入法“按住说话”快捷键设为 Option+Z。
3. 在设备页先打开蓝牙总开关，再进入两分钟配对模式，由 PokePod Voice.app 完成安全配对。
4. 菜单栏状态显示“就绪”后，在首页或 BOOT 上按住说话、松开结束。

设备页“蓝牙”行右侧是总开关，轻触左侧进入配对详情。配对详情可开始或取消配对，并显示六位配对码和当前 MTU；“忘记 Mac”才会删除已经授权的 Mac。设备只保留一个绑定。

PokePod Voice.app 接收 `16 kHz / mono / IMA ADPCM` 音频，临时把默认输入切换为
BlackHole，并负责 Option+Z 的按下、释放和异常恢复。PokeCapsule 不参与该链路。

## 标准刷写与恢复边界

正式 app-only 刷写的恢复证据是本次操作现场生成并验证的 3 MiB app0 备份、
SHA-256、设备身份和 `restore-plan.json`。标准流程只写或恢复偏移 `0x10000` 的
app0，不改动 NVS、分区表、引导区、板载 FAT 分区或 SD 卡。

完整 16 MiB Flash 双备份属于历史留档或特殊恢复流程。当前 `flash.sh` 不生成、
不写入也不恢复这种全盘镜像；涉及引导区、分区布局、NVS 或整片 Flash 的恢复必须
另行授权并使用独立工具，不能混入标准发布步骤。

主机测试和 clean build 只能证明软件候选成立，不能替代真实 CDC、BLE、SD、
腾讯返回、扬声器、触摸与电源管理验收。
