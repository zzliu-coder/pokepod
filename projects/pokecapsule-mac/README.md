# PokeCapsule Mac

PokeCapsule Mac 是 Poke3 与 Android 手机共用的 USB 管理器。每台设备拥有独立资料库、镜像、备份和离线队列；普通文件继续作为事实源，Mac 缓存损坏不会影响设备中的录音。

## 当前能力

- 自动寻找 ADB，识别未连接、未授权、离线和多设备状态，不硬编码设备序列号。
- 当 macOS 已识别到 Android USB、但 ADB 暂不可用时，明确显示“已插入，等待 ADB”；App 在前台每 10 秒自动重试。离线镜像和待同步队列持续可用。
- 将 `/sdcard/PokeCapsule/` 拉取为只读本地镜像。
- 应用活跃且 Poke3 已连接时，每 5 秒读取一次轻量元数据指纹；只有检测到新录音、转写、标签或目录变化才重新同步完整镜像。
- 设备选择器只切换独立资料库，不合并不同设备的数据。
- 浏览 Inbox、资料库、收藏、待转写、转写失败、两级目录、标签、回收站和分层文字版本。
- 多选移动、复制、删除、收藏、标签；修改统一提交为固定位置 JSON 命令。
- 设备确认维护状态后才允许修改；超时保持只读。
- 批量导出完整胶囊，并逐文件执行 SHA-256 校验。
- 批量导入完整胶囊；同 UUID 同内容跳过，不同内容明确报冲突。
- OpenAI 兼容校对适配器；endpoint、model、提示词和自动校对开关可配置。
- 默认使用 DeepSeek `https://api.deepseek.com/chat/completions`、`deepseek-v4-flash`，并显式关闭 thinking。
- API 结果在写回失败时保存在本机待提交缓存，重试不会再次请求模型。
- ADB 始终通过 `Process.arguments` 执行，目录名不会进入 shell。

## 构建和测试

```bash
swift test
swift build -c release
```

运行开发版：

```bash
swift run PokeCapsule
```

## ADB

应用按下列顺序查找 ADB：

1. 设置中指定的路径；
2. App 资源目录内的 `platform-tools/adb`；
3. `/opt/homebrew/bin/adb`；
4. `/usr/local/bin/adb`；
5. 当前 `PATH`。

Poke3 需要开启 USB 调试并接受 Mac 的 RSA 授权。管理器在事务开始前读取
`stay_on_while_plugged_in`，仅当 USB 位未开启时临时加入 USB 位，并在成功、失败和超时路径恢复原值。
当前 Poke3 现场值是 `7`，已经包含 USB 位，因此正常事务不会改写这个设置。

## 事务边界

浏览和导出属于只读操作。移动、复制、删除、目录、标签和收藏操作遵循：

1. Mac 向 `.commands/` 提交 `beginMaintenance`；
2. 等待 Android 明确确认；
3. 提交一个或多个事务命令；
4. 提交 `endMaintenance`；
5. 重新拉取只读镜像。

动态目录名只存在 JSON 内容中。远端命令文件名全部由 UUID 生成。Android 端命令服务尚未安装或未响应时，Mac 在 20 秒后停止，设备文件不会被直接修改。

## 数据安全

- 未知 `schemaVersion` 的胶囊只读显示。
- 重复 UUID、缺音频和损坏的 `processing.json` 会显示警告。
- 导出目标中已有同 UUID 目录时停止，禁止静默覆盖。
- API 校对失败不覆盖 `raw.txt`。
- API 密钥保存在 `~/Library/Application Support/PokeCapsule/Secrets/correction-api-key`，权限为当前用户只读写。首次发现 `~/Desktop/api.txt` 时会自动迁移第一条非空且以 `sk-` 开头的内容；迁移后可删除桌面文件。旧钥匙串仅作为兼容回退，正常使用不会触发授权窗口。其余粘贴文档不会进入请求或设备。
- 校对结果先原子写入 `~/Library/Application Support/PokeCapsule/PendingCorrections/`，设备确认提交后才删除缓存。

## 1.7.0 架构与界面

- 三端共用“最佳文字优先”的信息顺序，Mac 详情不再把原始转写、校对和最终文字并列堆满首屏。
- 云端错误码会转换为可理解的原因与恢复方向，原始录音始终保留。
- 原始转写、校对版本和最终文字仍可在展开区逐项查看与复制。
- `AppModel` 保留顶层协调；设备路径与注册、同步引擎、播放和校对分别由 `DeviceWorkspace`、`DeviceSyncEngine`、`CapsulePlaybackController` 与 `CorrectionWorkflow` 承担，资料库查询、乐观更新和强类型命令位于可测试组件。
- SwiftUI 页面拆为应用壳、设备侧栏、胶囊列表、设置和批量操作组件；批量操作只在选中胶囊后出现。
- DeepSeek 继续由用户手动触发，不参与设备端自动转写。

## 已完成的真机验收

- 43 项 Swift 测试、Release 构建、应用打包和代码签名校验通过；响应文件尚未生成会继续等待，ADB 断线或其他读取错误会立即结束等待并进入连接状态复核。
- 已自动识别 USB 连接的 Poke3，并建立只读镜像。
- 两条真机录音已从 `raw_ready` 自动调用 DeepSeek，写回 `polished.md` 后变为 `ready`。
- 维护握手、目录创建、目录删除、校对提交和重复事务幂等已在真机通过。
- 设备原有 `stay_on_while_plugged_in=7` 在事务前后保持不变。

仍需长期或大样本验证：100 条真实胶囊批量整理、USB 在事务中途断开、长时间待机耗电和极端并发压力。
