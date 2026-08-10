# Quality gate

- [x] baseline 与 clean worktree
- [x] 当前热构建基线计时
- [x] fast/release 分离
- [x] 薄 `.ino` 入口
- [x] 输入指纹与 cache hit
- [x] 最小 GFX 上游符号链接视图
- [x] host suite 60/60
- [x] 零改动/变更后/release 三类实测
- [x] frozen diff 单会话复核
- [x] 源码提交 `682edf50`
- [ ] 独立会话复核（当前回合无创建授权，完成声明降级）

truth_evidence: 实际计时、host tests、fresh release build、构建回执与 SHA。
claim_limit: 本机 host/build verified；真机 unverified。
