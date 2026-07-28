# PokeCapsule Mac

PokeCapsule Mac 是 Poke3 语音胶囊的 USB 管理器。它保留普通文件作为事实源，Mac 上的数据库或缓存损坏不会影响设备中的录音。

## 当前能力

- 自动寻找 ADB，识别未连接、未授权、离线和多设备状态，不硬编码设备序列号。
- 将 `/sdcard/PokeCapsule/` 拉取为只读本地镜像。
- 浏览 Inbox、Archive、两级目录、标签、收藏、原始转写和校对文本。
- 多选移动、复制、删除、收藏、标签；修改统一提交为固定位置 JSON 命令。
- 设备确认维护状态后才允许修改；超时保持只读。
- 批量导出完整胶囊，并逐文件执行 SHA-256 校验。
- 批量导入完整胶囊；同 UUID 同内容跳过，不同内容明确报冲突。
- OpenAI 兼容校对适配器；endpoint 和 model 可配置，API 密钥仅存 macOS 钥匙串。
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

1. Mac 向 `.commands/inbox/` 提交 `beginMaintenance`；
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

## 真机验收

Release 构建和自动测试只证明 Mac 端代码可构建及本地协议行为。以下项目必须连接安装了 PokeCapsule Android 命令服务的 Poke3 后验证：

- 维护握手和重新扫描；
- 100 个胶囊的移动、复制、删除与导出；
- USB 中途断开后的恢复；
- Android 与 Mac 同时操作时的单写者保护；
- 设备端导入提交和 UUID 冲突提示；
- API 的真实服务调用（需要用户提供密钥）。
