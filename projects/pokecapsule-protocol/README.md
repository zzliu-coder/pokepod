# PokeCapsule 文件协议 1.0

PokeCapsule 把普通文件作为事实源。Android 与 Mac 端即使数据库损坏，也必须能够仅凭这些文件重建列表。

## 根目录

设备根目录固定为：

```text
/sdcard/PokeCapsule/
├── Inbox/
├── Archive/
├── .staging/
├── .locks/
├── .commands/
├── .trash/
└── 用户目录/
    └── 二级目录/
```

- `Inbox` 和 `Archive` 是保留目录，不能删除或改名。
- 用户目录最多两级。
- 目录名去除首尾空格后长度为 1–80 个 Unicode 字符。
- 目录名禁止 `/`、`\`、NUL、`.`、`..`，也不能以 `.` 开头。
- 非空目录被删除时，其中的胶囊先移动到 `Inbox`。发生同名时为胶囊目录生成新 UUID。

## 胶囊目录

每个胶囊是一个 UUID 命名的目录：

```text
Inbox/
└── 0d95b7c1-7ce9-4a91-aea2-b64707a05c9f/
    ├── capsule.json
    ├── processing.json
    ├── audio.m4a
    ├── raw.txt
    └── polished.md
```

- `capsule.json`、`processing.json` 与 `audio.m4a` 在录音成功后必须存在。
- `raw.txt` 在本地转写成功后出现。
- `polished.md` 在校对成功后出现。
- Android 先写临时文件，再在同一目录内原子改名。
- 复制胶囊时生成新 UUID，并更新 `id`、`createdAt`、`updatedAt`。

## 元数据

`capsule.json` 使用 UTF-8 JSON，字段见 `capsule.schema.json`。它只保存标题、标签、收藏等用户资料。

`processing.json` 保存录音、转写和校对状态。两个文件均包含单调递增的 `revision`。处理任务不能覆盖 `capsule.json`，界面整理操作也不能覆盖 `processing.json`。

时间均为 UTC ISO 8601，例如 `2026-07-28T08:30:00Z`。

状态流：

```text
recording -> recorded -> queued -> transcribing -> raw_ready
                                            \-> failed
raw_ready -> correcting -> ready
                     \-> failed
```

校对失败不覆盖 `raw.txt`，转写失败不删除 `audio.m4a`。

## 并发约定

- Android 内部的所有写操作串行执行。
- 写入者通过原子创建 `.locks/write.json` 获得根写锁，内容包含持有者、事务 UUID、UTC 时间和最后心跳，结束后删除。
- 批量修改先写入 `.staging/<transaction-uuid>/`，校验成功后在同一文件系统内原子改名提交。
- Android 发现锁文件时仍允许录音到 Inbox，但暂停目录移动、复制、删除与转写。
- 锁文件超过 30 分钟且心跳不再变化才视为遗留锁；清除前必须再次确认持有者已离线。
- 扫描器忽略以 `.` 开头的文件和以 `.tmp` 结尾的文件。

## ADB 安全规则

- Mac 端通过 `Process` 的参数数组启动 `adb`，不得把路径拼接进 shell 命令。
- 远端路径只允许位于 `/sdcard/PokeCapsule/` 下。
- 用户输入必须通过上述目录名规则。
- 删除前先重新读取 `capsule.json` 并核对 UUID。
