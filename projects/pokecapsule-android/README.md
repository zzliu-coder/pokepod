# PokeCapsule Android

面向 BOOX Poke3（Android 10、arm64-v8a）的本地优先语音胶囊应用。保留文石原厂系统，不修改分区和全局息屏设置。

## 已实现

- 黑白静态主界面：Inbox、两级目录、标签、收藏、设置。
- 胶囊详情：音频播放、原始转写、校对文本、标题、标签和收藏。
- 长按进入多选；批量移动、复制、回收、标签和收藏。
- 胶囊形悬浮录音按钮：拖动吸附左右边缘、记忆位置、临时隐藏、彻底关闭、重启恢复。
- 麦克风前台服务：点击即录、半秒刷新真实音量环、静音提示、提前停止、60 秒硬停止。
- 录音先进入 `.staging`；停止并检查大小和时长后提交到 Inbox。
- `capsule.json` 与 `processing.json` 分离，使用 `AtomicFile` 写入。
- 文件扫描恢复；转写中断后回到 `queued`；未知协议只读。
- 同时满足“插电 + 非计费 Wi‑Fi”才运行的持久化单任务转写队列；无需打开应用，重启后继续等待。
- 腾讯一句话识别 `16k_zh`：M4A 原音直接上传，密钥一次性导入后由 Android Keystore 加密保存，明文暂存文件立即删除。
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

每个正式胶囊目录包含 `audio.m4a`、`capsule.json`、`processing.json`；转写成功后增加 `raw.txt`，校对成功后增加 `polished.md`。

## 转写

录音先进入 `queued`。设备插电且 Wi‑Fi 可用时，Android 系统唤醒任务并调用腾讯语音识别；失败会保留原始录音并重新排队。DeepSeek 校对只在 Mac 端点击按钮后调用。

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

单测覆盖协议路径规则、状态跳转和 100 次复制 UUID 唯一性。测试依赖使用工作区 Gradle 发行包自带的 JUnit，避免依赖本机代理。

## Mac 命令入口

Mac 把 `<commandId>.json` 推送到 `/sdcard/PokeCapsule/.commands/`，再通过显式广播调用：

```text
com.zheliu.pokecapsule.PROCESS_COMMAND
extra: commandFile=<commandId>.json
component: com.zheliu.pokecapsule/.command.CommandReceiver
```

接收器要求调用者持有 Android 的 `DUMP` 权限，ADB shell 可调用，普通第三方 App 不可调用。结果写入 `.commands/results/<commandId>.json`。动态路径不进入 shell 字符串；设备端重新校验 UUID、目录深度和名称。

支持目录、移动、复制、删除、收藏、标签、导入提交、校对提交和重新扫描。相同事务 UUID 的重试只返回原结果，不会重复执行。

## 当前验证

- Release APK、单元测试和 `lintRelease` 已通过，APK 内无 native Whisper/模型。
- Release APK 1.1.0 已覆盖安装到 Poke3；原有胶囊、书籍和系统设置保留，设备 tiny 模型已删除。
- 用户 8 秒测试录音已在 Poke3 上通过腾讯 `16k_zh` 真机转写为“福斯特建筑事务所商务。”。
- 重复事务 UUID 已在真机验证为幂等；测试目录创建一次、重复命令被消费、清理成功。
- API 校对由 Mac 端负责，本 Android 工程在 `raw_ready` 等待 Mac。
- 长期待机耗电、100 条真实胶囊批量操作和断线压力测试仍需持续观察。
