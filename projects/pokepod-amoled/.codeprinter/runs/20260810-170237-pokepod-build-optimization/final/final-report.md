# PokePod 构建优化报告

CodePrinter run validation: PASS

## 结论

源码已提交；本机测试、日常增量构建与隔离的正式构建证据成立。独立会话复核与真机验证留待后续。

## Git 状态

- baseline commit: `c5b0d054c508a27b63d18c5d9040a8c18b726a35`
- source commit: `682edf50`（`Optimize PokePod firmware build loop`）
- branch: `feature/pokepod-amoled-1.8`
- worktree: `/Users/zheliu/Documents/Codex/.worktrees/pokepod-amoled-1.8`
- source commit 后仅有本次 CodePrinter 证据目录待提交。

## 施工拓扑

- 当前会话：需求压缩、架构审计、实现、集成与本机构建验证。
- 独立会话：本回合没有创建授权，已生成 `handoffs/review.md` 供后续复核。
- 真机：未刷写、未连接、未执行设备验收。

## 变更摘要

1. 默认构建改为 fast 模式；稳定 build path 复用 Arduino 对象缓存。
2. `--release` 使用独立目录执行 clean build，不清除 fast 缓存。
3. 将厚 `.ino` 主体迁入 `PokePodApp.cpp`，让普通源文件改动只重编必要对象。
4. 用确定性输入指纹识别零改动构建，命中后直接复用产物。
5. 从 Arduino GFX 1.6.5 上游生成符号链接最小视图；清单 21 个文件，实际构建 9 个 GFX 对象。
6. host suite 纳入指纹、构建模式、最小 GFX 清单与薄入口合同。

## 性能证据

| 场景 | 基线/结果 | 说明 |
|---|---:|---|
| 旧构建脚本，无源码变化 | 58.21 秒 | 292 个对象中仍重建厚 `.ino` |
| fast，零改动 | 1.32 秒 | 确定性指纹命中 |
| fast，普通源文件增量 | 21.74–37.96 秒 | 只更新薄入口和实际变更对象 |
| fast，首次冷缓存 | 339.44 秒 | 最小 GFX 视图的首次生成与编译 |
| release，隔离 clean build | 404.09 秒 | 发布前真实性门槛 |

## 验证证据

- host suite：60/60 PASS。
- 固定中文字库：扫描 105 个源，16/20/28 三档各覆盖 425 个字形。
- release Flash：`2,310,963 / 3,145,728 = 73%`。
- release RAM：`103,644 / 327,680 = 31%`。
- 项目源码 warning：0；其余 warning 来自上游依赖。
- release app SHA-256：`484a82963978b5394df0d24b919d026d6088a64ab20e03d74a00e3f4c0189f85`。
- final fast app SHA-256：`e9f439c7c22644088661e4edfa5760821356bfd6881d26b50f26237441bdddeb`。
- `git diff --check`、shell syntax、指纹合同与 diff 凭据扫描：PASS。

## 合并状态

- merge status: `applied`
- 源码直接写入目标 worktree，并提交为 `682edf50`。
- CodePrinter 运行证据使用单独提交记录。

## 声明分级

| 能力 | claim_status | 证据 | 限制 |
|---|---|---|---|
| 构建脚本与缓存边界 | done | fast/release 实测、指纹合同 | 当前 Mac 与固定 Arduino 工具链 |
| 主机测试 | done | 60/60 PASS | 不等同于真机验收 |
| release 编译 | done | clean build、资源占用、SHA | 未刷写到设备 |
| 独立验收 | unverified | `handoffs/review.md` | 当前回合无独立会话创建授权 |
| 真机运行 | unverified | 无 | 本轮明确不操作真机 |
