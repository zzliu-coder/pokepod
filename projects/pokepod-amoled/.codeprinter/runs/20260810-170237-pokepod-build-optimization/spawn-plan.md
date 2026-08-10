# Spawn plan

- execution_mode: single_session
- scope: `projects/pokepod-amoled/**`
- baseline: `c5b0d054c508a27b63d18c5d9040a8c18b726a35`
- degraded_reason: 当前回合没有创建可见施工/验收任务的授权；总控在专用 PokePod worktree 直接施工，最终声明保持 host/build 范围。
- independent_review: 保存 handoff，后续可由独立任务复核；本轮冻结自复核不能替代它。
