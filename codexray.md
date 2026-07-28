# 架构透视报告：PokeCapsule

## 结论摘要

- 仓库：`/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3`
- 本次范围：PokeCapsule Android、Mac、共享文件/命令协议；未分析文石系统固件和第三方云服务内部实现。
- 核心判断：当前架构适合“Poke3 为事实源、Mac 为管理端”的在线单设备模型；进入回收站、离线队列、搜索和人工定稿前需要定向重构，保留现有文件事实层、ADB 命令通道和原子写保护。
- 最大风险：Mac 离线修改缺少持久操作日志，普通元数据命令缺少期望版本，重连后无法安全判断冲突。
- 最值得学习的代码：`CapsuleStore` 的原子文件行为、`DeviceCommandClient` 的命令回执、`MirrorSynchronizer` 的镜像切换。
- 是否建议进入 CodePrinter 施工：建议，先改协议与核心服务，再接两端 UI。

## 1. 代码库概览

### 技术栈

| 类型 | 发现 | 证据 |
|---|---|---|
| 语言/框架 | Android Java + 原生 View；macOS SwiftUI | `projects/pokecapsule-android/app/src/main/java`、`projects/pokecapsule-mac/Sources/PokeCapsule` |
| 包管理器 | Gradle 8 / Android Gradle Plugin；Swift Package Manager | `projects/pokecapsule-android/build.gradle`、`projects/pokecapsule-mac/Package.swift` |
| 入口 | Android `MainActivity`、录音/转写 Service；Mac `PokeCapsuleApp` | `AndroidManifest.xml`、`PokeCapsuleApp.swift` |
| 数据层 | `/sdcard/PokeCapsule` 下的目录与 JSON/音频/文本文件 | `PokePaths.java`、`CapsuleStore.java`、`protocol.json` |
| 测试 | Android JUnit；Mac XCTest | 两端 `Tests` / `src/test` |
| 外部依赖 | ADB、腾讯 ASR、DeepSeek 兼容接口、macOS Keychain | `ADB.swift`、`TencentAsrClient.java`、`Correction.swift` |

### 目录职责

| 目录/文件 | 责任 | 判断证据 | 风险或备注 |
|---|---|---|---|
| `projects/pokecapsule-protocol` | 跨端文件与命令契约 | JSON Schema 与示例 | 新功能必须先更新这里 |
| `android/.../storage` | Poke3 事实写入、锁、原子操作 | `CapsuleStore`、`RootWriteLock` | `CapsuleStore` 已达 756 行 |
| `android/.../command` | 接收 Mac 命令并返回结果 | `CommandReceiver` | 命令分派和业务规则耦合 |
| `android/.../service` | 录音、后台转写、悬浮按钮 | 各 Service | 边界基本清楚 |
| `android/.../ui` | Poke3 管理界面与详情 | `MainActivity`、`CapsuleDetailActivity` | `MainActivity` 已达 564 行 |
| `mac/PokeCapsuleCore` | ADB、协议、扫描、校对适配 | `ADB.swift` 等 | ADB 文件承担过多服务 |
| `mac/PokeCapsule` | SwiftUI 与应用状态 | `AppModel.swift`、`PokeCapsuleApp.swift` | 离线队列不应塞进 AppModel |

## 2. 架构地图

### 主流程

| 步骤 | 代码位置 | 输入 | 输出/交付物 | 状态变化 | 保护措施 |
|---|---|---|---|---|---|
| 录音 | `RecordingService.java` | 麦克风、60 秒上限 | `audio.m4a` 暂存 | recording | 前台服务、时长/音频校验 |
| 提交胶囊 | `CapsuleStore.commitRecording` | 完整音频 | Inbox 胶囊目录 | recording → queued | UUID、原子落盘、目录提交 |
| 云端转写 | `TranscriptionJobService.java` | queued 胶囊 | `raw.txt` | queued → transcribing → raw_ready | 插电/Wi-Fi 调度、幻觉长度守卫 |
| Mac 同步 | `MirrorSynchronizer.refresh` | Poke3 根目录 | Mac 只读镜像与 `CapsuleIndex` | 无设备状态写入 | staging 拉取、原镜像回滚 |
| Mac 管理 | `AppModel.perform` → `DeviceCommandClient` → `CommandReceiver` | 移动/标签/收藏/删除等命令 | Poke3 文件事实变化 | capsule revision 递增 | maintenance lock、回执、超时 |
| 校对 | `AppModel.correct` / `commitCorrection` | raw text、期望 revision | `polished.md` | raw_ready → ready | Keychain、期望版本、缓存重试 |

### 次主流程或失败流程

| 流程 | 触发条件 | 代码位置 | 最终交付物 | 风险点 |
|---|---|---|---|---|
| 中断恢复 | 录音/转写进程异常 | `CapsuleStore.recoverInterruptedWork` | 重排队或中断标记 | 尚无用户可见恢复中心 |
| 删除 | 用户删除胶囊 | `CapsuleStore.deleteCapsules` | `.trash` 隐藏目录 | 未保存原目录/删除时间，无恢复命令 |
| 命令重试 | Mac 等待回执超时 | `DeviceCommandClient.submit` | 错误状态 | 重试前需重新同步确认 |
| 离线管理 | ADB 不可用 | `AppModel.perform` | 当前直接拒绝 | 没有操作日志或冲突模型 |

### 四层视图

| 层 | 代码位置 | 它负责什么 | 主要风险 |
|---|---|---|---|
| 事实层 | `CapsuleStore`、协议 JSON/文本/音频 | 保存原音、元数据、处理状态和派生文字 | 回收与人工定稿事实缺失 |
| 行为层 | Android Store/CommandReceiver；Mac AppModel/Client | 执行录音、转写、移动、标签、校对 | 大类继续膨胀 |
| 保护层 | `AtomicFiles`、`RootWriteLock`、`MaintenanceSession`、revision | 原子性、互斥、幂等回执、版本检查 | 版本检查只覆盖校对 |
| 边界审计层 | `ADBTransport`、`TencentAsrClient`、`CorrectionAdapter` | 隔离设备和 API | 缺统一操作审计日志 |

### 变化点

| 变化点 | 当前接法 | 扩展成本 | 证据 | 判断 |
|---|---|---|---|---|
| ASR 提供商 | Android client/config 独立 | 中 | transcription 包 | 可继续抽象但本轮不必 |
| 校对模型 | Mac configuration/adapter | 低 | `Correction.swift` | 合理 |
| 数据组织 | 文件夹 + JSON sidecar | 中 | protocol / store | 当前规模无需数据库 |
| 设备命令 | JSON command + broadcast + response | 中 | `DeviceCommandClient` / `CommandReceiver` | 可扩展，需补版本前置条件 |
| 搜索 | 当前无 | 低 | `CapsuleIndex` 已在内存 | 先做内存索引，暂不引入 SQLite |

## 3. 关键代码切片

| 优先级 | 代码位置 | 架构概念 | 先看哪里 | 为什么值得读 |
|---|---|---|---|---|
| 1 | `projects/pokecapsule-android/app/src/main/java/com/zheliu/pokecapsule/storage/CapsuleStore.java` | 事实层/行为层 | `commitRecording`、`mutateCapsules` | 核心文件事实如何形成 |
| 2 | `projects/pokecapsule-android/app/src/main/java/com/zheliu/pokecapsule/storage/RootWriteLock.java` | 保护层 | `acquire`、`close` | 跨动作互斥和陈旧锁恢复 |
| 3 | `projects/pokecapsule-android/app/src/main/java/com/zheliu/pokecapsule/command/CommandReceiver.java` | 命令边界 | `process`、`execute` | Mac 写操作如何落到设备 |
| 4 | `projects/pokecapsule-mac/Sources/PokeCapsuleCore/ADB.swift` | 外部适配/保护 | `DeviceCommandClient` | 命令、回执、maintenance 会话 |
| 5 | `projects/pokecapsule-mac/Sources/PokeCapsuleCore/ADB.swift` | 失败恢复 | `MirrorSynchronizer.refresh` | 镜像替换失败时如何保留旧数据 |
| 6 | `projects/pokecapsule-mac/Sources/PokeCapsuleCore/ProtocolStorage.swift` | 读取模型 | `CapsuleScanner.scan` | Mac 如何从文件生成可查询索引 |
| 7 | `projects/pokecapsule-mac/Sources/PokeCapsuleCore/Models.swift` | 领域模型 | Capsule/Processing/Command | 当前状态与版本承载 |
| 8 | `projects/pokecapsule-mac/Sources/PokeCapsule/AppModel.swift` | 应用行为层 | `sync`、`perform`、`correct` | UI、设备和后台任务的交汇处 |
| 9 | `projects/pokecapsule-android/app/src/main/java/com/zheliu/pokecapsule/core/ProcessingState.java` | 状态机 | `canTransitionTo` | 防止处理状态乱跳 |
| 10 | `projects/pokecapsule-android/app/src/main/java/com/zheliu/pokecapsule/transcription/TranscriptionGuard.java` | 边界保护 | duration/output guard | 短录音幻觉如何被拦截 |
| 11 | `projects/pokecapsule-android/app/src/main/java/com/zheliu/pokecapsule/ui/MainActivity.java` | Poke3 UI | filter/render/actions | 墨水屏交互现状与膨胀风险 |

## 4. 架构评价

### 好在哪里

| 判断 | 证据 | 价值 |
|---|---|---|
| 原始音频与派生文字分开 | 协议文件集合 | AI 错误不会污染原始事实 |
| Poke3 是明确事实源 | Mirror 只读、命令写回 | 双端职责清楚 |
| 写入有保护 | 原子写、root lock、maintenance lock | 降低断电和并发损坏 |
| 外部 API 基本隔离 | ASR/Correction 单独模块 | 后续更换服务成本可控 |
| 镜像替换可回滚 | `MirrorSynchronizer` | 拉取失败不破坏上次可用数据 |

### 风险在哪里

| 优先级 | 风险 | 证据 | 后果 | 当前判断 |
|---|---|---|---|---|
| P0 | 离线命令无持久日志和版本前置条件 | `AppModel.perform` 直接要求 transport；普通命令无 expected revisions | 重连后无法安全自动应用 | 必须修 |
| P0 | 回收站缺删除事实 | `.trash` 仅重命名目录 | 无法可靠恢复原目录和区分多次删除 | 必须修 |
| P1 | 人工定稿缺正式事实文件 | 仅 raw/polished | 用户修改会覆盖来源或无处保存 | 必须修 |
| P1 | 核心类继续膨胀 | Store 756 行、MainActivity 564 行、ADB 457 行 | 新功能难测、错误边界模糊 | 定向拆分 |
| P1 | 缺统一审计记录 | 只有命令结果散落文件 | 难回答“谁何时改了什么” | 值得改 |
| P2 | 搜索尚无索引 | CapsuleIndex 仅筛选 | 胶囊增长后难找 | 内存索引足够 |

### 证据不足

| 问题 | 缺什么证据 | 怎么补 |
|---|---|---|
| 大规模搜索性能 | 真实胶囊数量与文本规模 | 用 1,000 条合成胶囊压测 |
| 30 天回收空间 | 实际平均音频大小 | 从设备统计后决定自动清理阈值 |
| 断线冲突频率 | 用户是否会同时在两端修改 | 先实现保守冲突暂停并记录 |

## 5. 优化建议

### 必须修

| 建议 | 证据 | 影响 | 改法 | 验收方式 |
|---|---|---|---|---|
| 增加操作日志和期望版本 | 普通命令无 revision check | 防止离线覆盖新数据 | 持久 pending queue，每条命令携带胶囊 revision 快照 | 断线排队、无冲突自动提交、有冲突暂停测试 |
| 增加 `trash.json` 与恢复协议 | `.trash` 仅目录名 | 可追溯、可恢复 | 记录 originalFolder/deletedAt/trashId，新增 restore/purge | 删除、重启、恢复、彻底删除真机测试 |
| 增加 `final.md` | 仅 raw/polished | 保留人工最终事实 | 编辑写 final，显示优先 final > polished > raw | 双端编辑、版本冲突、原文不变测试 |

### 值得改

| 建议 | 证据 | 影响 | 改法 | 验收方式 |
|---|---|---|---|---|
| 拆分 Android Store | 756 行 | 降低回收/搜索耦合 | `TrashStore`、`SearchIndex`、现有 Store 保留胶囊 CRUD | 单元测试和构建 |
| 拆分 Mac ADB/Core | 457 行 | 队列和传输独立测试 | `OfflineQueue`、`SyncCoordinator`、`BackupManager` | 无设备单测 |
| 内存全文搜索 | 当前 Index 已在内存 | 少依赖、低功耗 | 标准化文本后 contains + 状态筛选 | 1,000 条性能测试 |

### 暂时观察

| 项目 | 现状 | 观察条件 |
|---|---|---|
| SQLite/FTS | 当前胶囊量小 | 10,000 条或内存搜索超过 200ms |
| 云同步/账户 | 单人、USB 管理 | 出现第二台电脑或远程访问需求 |
| 协作权限 | 单用户设备 | 出现多人共同编辑需求 |

## 6. 学习路线

### 第一遍：建立主流程体感

| 阅读顺序 | 文件/函数 | 带着什么问题看 |
|---|---|---|
| 1 | `RecordingService` | 一段音频何时成为胶囊？ |
| 2 | `CapsuleStore.commitRecording` | 哪些文件共同构成事实？ |
| 3 | `TranscriptionJobService` | 派生文字怎样改变处理状态？ |
| 4 | `MirrorSynchronizer` | Mac 怎样得到一致快照？ |

### 第二遍：看状态、数据和保护层

| 阅读顺序 | 文件/函数 | 带着什么问题看 |
|---|---|---|
| 1 | `ProcessingState` | 哪些状态跳转会被拒绝？ |
| 2 | `AtomicFiles` / `RootWriteLock` | 断电和并发怎样被保护？ |
| 3 | `DeviceCommandClient` | 命令何时能被视为完成？ |
| 4 | `CommandReceiver` | 设备如何拒绝重复或非法命令？ |

### 第三遍：看变化如何被接住

| 阅读顺序 | 文件/函数 | 带着什么问题看 |
|---|---|---|
| 1 | `TencentAsrClient` | 外部服务变化隔离在哪里？ |
| 2 | `CorrectionAdapter` | 模型/地址怎样配置？ |
| 3 | `CapsuleScanner` | 新 sidecar 怎样兼容旧胶囊？ |
| 4 | 两端测试 | 哪些完成声明有真实证据？ |

## 7. 下一步

- 最小验证动作：先更新协议与纯核心单元测试，不碰 UI。
- 如果要重构，第一刀：增加删除事实、最终文字和期望版本模型，同时拆出离线队列。
- 如果只是学习，下一次该看：一次 Mac 标签操作从按钮到设备文件变更的完整链路。

## 8. CodePrinter 交接包

### 是否启动施工

| 判断 | 依据 | 声明边界 |
|---|---|---|
| 建议启动 | 用户明确要求实现阶段一、二；现有保护层可复用 | 每个设备能力必须有连接 Poke3 的真实验收 |

### 施工目标

| 优先级 | 目标 | 对应风险 | 证据 |
|---|---|---|---|
| P0 | 安全离线队列 | 断线操作与冲突覆盖 | `AppModel.perform` |
| P0 | 可恢复回收站 | 删除不可见、原目录丢失 | `deleteCapsules` |
| P1 | 人工定稿和检索 | 文字不可最终确认、难查找 | 协议与 UI |

### 施工切片

| 顺序 | 切片 | 建议写入范围 | 依赖 | 验收方式 |
|---|---|---|---|---|
| 1 | 协议与领域模型 | `pokecapsule-protocol`、两端 Models | 无 | Schema/模型测试 |
| 2 | Android 核心行为 | storage、command | 1 | JVM 单测 |
| 3 | Mac 核心服务 | PokeCapsuleCore | 1 | XCTest |
| 4 | Android UI | ui | 2 | 构建、真机 |
| 5 | Mac UI | AppModel/App | 3 | 构建、真实点击 |
| 6 | 集成 | 两端 | 1-5 | 断线/重连/冲突/恢复端到端 |

### 质量红线

| 红线 | 为什么阻断 | 验收证据 |
|---|---|---|
| 原始音频不可被编辑或派生任务覆盖 | 原音是唯一不可重建事实 | 哈希前后相同 |
| 离线队列不得静默覆盖新 revision | 防数据丢失 | 冲突测试 |
| 彻底删除必须二次确认 | 不可恢复动作 | 两端 UI 真测 |
| 失败同步保留旧镜像和队列 | 防双重数据损失 | 注入失败测试 |

### 不建议本轮施工

| 项目 | 原因 | 观察条件 |
|---|---|---|
| 数据库迁移 | 当前文件模型足够且更利于备份 | 搜索规模达到阈值 |
| 云端账户 | 增加隐私和同步复杂度 | 明确远程多设备需求 |
| AI 问答/RAG | 偏离快速记录主流程 | 检索功能稳定后再评估 |
