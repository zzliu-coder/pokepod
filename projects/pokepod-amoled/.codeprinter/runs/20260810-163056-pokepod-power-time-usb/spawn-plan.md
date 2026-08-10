# 执行方式

- execution_mode: single_session
- changed_scope: `projects/pokepod-amoled/**`
- parallel_workers: 0
- degraded_reason: 当前环境没有可见任务创建工具；本轮直接在已确认干净的专用 PokePod worktree 内施工并保留完整证据。
- independent_review: 施工完成后进行冻结树只读复核，不把主机测试等同于真机验收。
