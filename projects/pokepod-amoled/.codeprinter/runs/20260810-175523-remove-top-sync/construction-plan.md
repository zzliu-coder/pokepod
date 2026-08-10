# PokePod 顶栏同步入口精简

## 施工目标

- 删除三个根页面顶栏中红框所示的“同步”胶囊入口及其触摸命中区。
- 保留设备页“与电脑同步”入口、独立同步状态页和五分钟窗口逻辑。
- 保留右上角 USB 物理连接图标、Wi-Fi 与蓝牙状态图标。
- 基于当前分支继续迭代；`c5b0d054c508a27b63d18c5d9040a8c18b726a35` 必须是当前 HEAD 的祖先。
- 主机验证后，只向已确认身份的 PokePod 执行 app-only 刷写。

## Git Bootstrap

- workspace: `/Users/zheliu/Documents/Codex/.worktrees/pokepod-amoled-1.8`
- baseline: `c2d5adf5bccaf1bec679aff7cf976af60e00543b`
- branch: `feature/pokepod-amoled-1.8`
- 开工状态：clean。
- 祖先核对：`c5b0d054...` 是 baseline 祖先。

## 施工切片

1. 删除顶栏胶囊的绘制与专用标签函数。
2. 删除根页面顶栏的同步命中区和废弃布局常量。
3. 更新 UI policy/theme/无线同步静态合同。
4. 运行 host suite、字体覆盖、release build。
5. 重新枚举 USB，确认 PokePod 身份与 app0 路径，再刷写和读回。

## 质量门

- 顶栏不再绘制或响应“同步”胶囊。
- 设备页“与电脑同步”仍一击开启既有窗口并进入状态页。
- USB/Wi-Fi/BLE 三个状态图标语义不变。
- 固定中文字库零缺字。
- host suite 全绿，release Flash/RAM 低于 75%，项目源码 warning 0。
- 端口身份不明确时禁止写入。

## 真实性闸门

- truth_evidence: git 祖先关系、源码合同、host tests、fresh release build、USB 描述符、app-only 写入与读回哈希、刷写后串口。
- claim_limit: 主机证据不能替代真机显示验收；未枚举到 PokePod 时刷写状态只能标记 blocked。

