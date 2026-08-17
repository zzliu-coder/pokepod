# PokeCapsule Android

面向 BOOX Poke3 和普通 Android 手机的本地优先语音胶囊应用。保留文石原厂系统，不修改分区和全局息屏设置；手机端支持移动数据和手机扬声器。

## 产品线与权限边界

工程输出两条明确的产品线：

- `poke3Legacy`：`targetSdk 28`，保留 Poke3 的共享存储行为和 `/sdcard/PokeCapsule` 资料库兼容性。
- `phoneModern`：`targetSdk 34`，使用现代 Android 权限模型；首次打开前必须在系统设置授予“所有文件访问”权限，未授权时不会启动资料库扫描、录音、转写或电脑命令处理。

两条产品线共用资料库协议和业务代码，权限声明放在各自的 manifest source set 中。Release 构建不绑定 debug key；正式签名只能通过外部 Gradle 属性注入：

```text
releaseStoreFile=/path/to/release.keystore
releaseStorePassword=...
releaseKeyAlias=...
releaseKeyPassword=...
requireReleaseSigning=true   # 发布流水线建议开启
```

使用 `build.sh` 时也可以通过 `POKECAPSULE_RELEASE_STORE_FILE`、
`POKECAPSULE_RELEASE_STORE_PASSWORD`、`POKECAPSULE_RELEASE_KEY_ALIAS` 和
`POKECAPSULE_RELEASE_KEY_PASSWORD` 注入；入口会在不打印值的情况下转换成
Gradle 参数。命令行显式的 `-P` 参数优先于同名环境变量。

## 可复现构建

仓库固定使用 Gradle `8.14.5`。本机先准备 Android SDK 34 和 Java 17，再执行：

```bash
JAVA_HOME=/path/to/jdk-17 \
POKECAPSULE_GRADLE_BIN=/path/to/gradle-8.14.5/bin/gradle \
./build.sh
```

不传任务时，入口会依次执行两条产品线的 unit test、lint、debug assemble 和 release assemble。需要单独构建时直接把 Gradle 任务传给 `build.sh`，入口会先拒绝错误 Gradle 版本。

在 macOS 上，`artifacts/安装-PokeCapsule-1.7.command` 会相对自身目录寻找 APK；旁边存在多个变体时必须显式传入 APK 路径，避免把 phoneModern 包装进 Poke3。安装前会确认 ADB、在线设备、Poke3 身份并备份资料库与旧 APK。

## 已实现

- 统一资料库语义：Inbox、全部、收藏、待转写、转写失败、两级目录、标签和回收站。
- 手机与 Poke3 共用同一个 APK，由 `DeviceCapabilities` 自动选择录音入口、播放提示、移动网络和墨水屏刷新策略。
- 顶部菜单打开资料库抽屉，长按进入多选；移动、标签、收藏、复制和删除集中在底部操作栏。
- 胶囊详情：音频播放、原始转写、校对文本、标题、标签和收藏。
- 长按进入多选；批量移动、复制、回收、标签和收藏。
- Poke3 使用胶囊形悬浮录音按钮：拖动吸附左右边缘、记忆位置、临时隐藏、彻底关闭、重启恢复。
- 普通 Android 手机在主界面右下角显示应用内胶囊录音按钮，不申请或运行悬浮窗服务。
- 麦克风前台服务：点击即录、半秒刷新真实音量环、静音提示、提前停止、60 秒硬停止。
- 普通安卓手机自动使用 `MIC`、系统 AGC、低电平人声增益和软限幅；Poke3 保留轻量录音参数，无需用户选择模式。
- 录音先进入 `.staging`；停止并检查大小和时长后提交到 Inbox。
- 录音保存、后台转写和电脑端管理提交后，正在显示的列表与详情会立即刷新；不使用定时轮询。
- `capsule.json` 与 `processing.json` 分离，使用 `AtomicFile` 写入。
- 文件扫描恢复；转写中断后回到 `queued`；未知协议只读。
- 打开应用后才提交转写任务；Poke3 和普通手机只要网络可用即可处理队列。任务不跨重启持久化。
- 同一次后台唤醒会连续处理当时已有的待转写胶囊，不再把“队列里还有下一条”误判为失败并触发指数退避。
- 腾讯一句话识别 `16k_zh`：M4A 原音直接上传，密钥一次性导入后由 Android Keystore 加密保存，明文暂存文件立即删除。
- 同一套腾讯账号可通过受 ADB 权限保护的临时迁移命令复制到另一台自有设备；目标设备会用自己的 Android Keystore 重新加密，迁移暂存文件随即清理。
- 短音频与幻觉保护：不足 2 秒不自动转写；输出字数超过录音时长的合理上限时拦截，不进入 API 校对。
- 固定 JSON 命令入口，供 Mac 端提交经过设备端校验的移动、复制、删除、目录、标签和收藏事务。

## 文件位置

```text
/sdcard/PokeCapsule/
├── Inbox/
├── Archive/
├── .staging/
├── .locks/
├── .commands/
├── .trash/
└── 用户目录/二级目录/
```

每个正式胶囊目录包含 `capsule.json`、`processing.json` 与 `processing.audioFile` 指向的音频；Android/Poke3 新录音仍为 `audio.m4a`。转写成功后增加 `raw.txt`，校对成功后增加 `polished.md`。旧 schema 1 与新 schema 2 都可读取，未知 schema 仅只读显示。

## 转写

录音先进入 `queued`。用户打开应用后，只要网络可用就处理队列。录音结束、开机或 Mac 同步本身不会启动转写。失败会保留原始录音并按错误类型重试或进入失败列表。DeepSeek 校对只在 Mac 端点击按钮后调用。

## 本机构建

```bash
cd projects/pokecapsule-android
JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
../../work/toolchains/gradle@8/8.14.5/libexec/bin/gradle \
  --offline --no-daemon assembleDebug testDebugUnitTest
```

Debug APK：

```text
app/build/outputs/apk/debug/app-debug.apk
```

当前 48 项本地单测覆盖共享展示 fixture、智能清单、搜索排序、设备能力、命令兼容、协议 UUID 规范化、音频 basename 与 v1/v2 描述、腾讯 M4A/WAV 格式声明、路径规则、损坏转写状态保护、回收站目录绑定、状态跳转、XML 自定义控件构造和 100 次复制 UUID 唯一性。另有 4 项 Android 仪器测试，其中新增“新录音写 schema 2 M4A 描述”用例。测试依赖使用工作区 Gradle 发行包自带的 JUnit，避免依赖本机代理。

## Mac 命令入口

Mac 把 `<commandId>.json` 推送到 `/sdcard/PokeCapsule/.commands/`，再通过显式广播调用：

```text
com.zheliu.pokecapsule.PROCESS_COMMAND
extra: commandFile=<commandId>.json
component: com.zheliu.pokecapsule/.command.CommandReceiver
```

接收器要求调用者持有 Android 的 `DUMP` 权限，ADB shell 可调用，普通第三方 App 不可调用。结果写入 `.commands/results/<commandId>.json`。动态路径不进入 shell 字符串；设备端重新校验 UUID、目录深度和名称。

支持目录、移动、复制、删除、收藏、标签、导入提交、校对提交、腾讯配置迁移和重新扫描。相同事务 UUID 的重试只返回原结果，不会重复执行。

## 当前验证

- 1.7.0（versionCode 22）当前通过 48 项本地单元测试、资源编译、Java 全量编译、DEX 打包、Release/Debug 构建和 Lint `No issues found`。此前 3 项文件系统仪器测试已在 Vivo X Fold3 通过；新增的 schema 2 M4A 仪器用例仍需 Android/Poke3 真机复核。APK 内无 native Whisper/模型。
- 1.7.0 将查询选择交给 `LibraryController`，异步扫描和写操作交给 `LibraryRepository`，侧栏与锚定菜单交给 `LibraryMenuCoordinator`，列表行交给 `CapsuleListAdapter`，设置与整理迁入独立页面；`CapsuleStore` 只作为稳定写入门面。
- Release APK 1.4.6 已由用户在 Vivo X Fold3 真机确认录音结束后新胶囊会立即出现。
- Poke3 1.4.6 真机诊断确认：电量 100%、Wi‑Fi 已连接且通过联网验证，但文石系统未给网络附加 `NOT_METERED` 标记，导致旧调度条件一直不满足。1.4.7 改为接受任意可联网网络；Poke3 无蜂窝数据，实际仍通过 Wi‑Fi 转写。
- 1.4.8 修复多条队列被 30/60/120 秒指数退避拖慢的问题；腾讯明确返回空文本或输出保护命中时标记为“转写失败”，停止无意义的自动重复请求，用户仍可在详情中手动重试。
- 1.4.9 在应用进程启动时清理上个进程遗留的写锁；系统回收、崩溃或覆盖升级中断转写后，不再等待 30 分钟才恢复。
- Poke3 已覆盖安装 1.4.9 并完成真机验收：联网任务约束满足，遗留锁自动清理，待转写与转写中队列均为 0；两条 5 秒测试录音已生成 `raw.txt` 并实时显示在 Inbox 顶部。
- 1.5.0 改为“打开应用后才转写”；手机取消悬浮胶囊并把录音按钮放入主界面，Poke3 悬浮胶囊的位置与行为保持不变。
- 1.5.1 去掉手机与 Poke3 的自动转写电量门槛；打开应用且联网即可处理队列。
- 1.5.2 将手机端应用内胶囊的默认位置调整到右下角，并保留应用内拖动。
- 1.6.0 统一手机、Poke3 和 Mac 的胶囊信息层级；详情页优先展示播放、状态和最佳文字，技术错误不再直接暴露。新录音在安全边界停止，接近 60 秒的旧录音会保留原音并生成临时 59 秒副本转写，避免腾讯严格时长限制导致循环失败。
- 用户 8 秒测试录音已在 Poke3 上通过腾讯 `16k_zh` 真机转写为“福斯特建筑事务所商务。”。
- 重复事务 UUID 已在真机验证为幂等；测试目录创建一次、重复命令被消费、清理成功。
- API 校对由 Mac 端负责，本 Android 工程在 `raw_ready` 等待 Mac。
- 长期待机耗电、100 条真实胶囊批量操作和断线压力测试仍需持续观察。
