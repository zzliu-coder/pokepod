# PokePod 构建优化施工图

## 目标

- 日常构建默认复用稳定缓存，零改动时直接命中输入指纹。
- 把 Arduino 每轮必重编的厚 `.ino` 收缩为薄入口，稳定主体改为普通 `.cpp` 对象。
- 发布构建使用独立目录和 `--clean`，不破坏日常增量缓存。
- 保持产物路径、FQBN、单 BLE 连接 SDK overlay、Flash/RAM/warning 门槛与刷写流程不变。

## 已确认事实

- baseline: `c5b0d054c508a27b63d18c5d9040a8c18b726a35`，开工时 clean。
- 当前无源码变化的增量构建为 58.21 秒；292 个对象中只有自动生成的 `PokePodAmoled.ino.cpp.o` 被重编。
- 当前脚本默认强制 `--clean`；发布 clean 会删除日常缓存。
- Arduino CLI 1.3.1 会在 `--clean` 或构建选项变化时清空 build path。

## 施工切片

1. 建立 `fast` 与 `release` 两个稳定 build path。
2. 将 `.ino` 收缩为 Arduino 入口，主体移动到 `PokePodApp.cpp`。
3. 加入确定性输入指纹、零改动 cache hit、强制刷新与构建回执。
4. 从 Arduino GFX 1.6.5 生成只含 PokePod 所需文件的符号链接视图，保留上游为唯一源码。
5. 将 `verify.sh` 固定到 release clean；更新文档和静态合同。
6. 测量零改动、单模块变化和发布 clean 三类耗时。

## A 级质量门

- 现有 host suite 全通过；所有读 `.ino` 的合同转向真实应用主体。
- fast/release 不共享对象目录；release 必须包含 `--clean`。
- 输入指纹覆盖产品源、构建脚本、FQBN、SDK 配置、Arduino core、GFX 和额外参数。
- 产物 SHA、资源占用、项目源码 warning 与单连接配置继续验证。
- 不引入 Ninja/Make 自维护的第二套 Arduino 链接规则。
- 最小 GFX 视图必须继续引用固定的 1.6.5 上游文件，并由清单测试约束。

## Truth Gate / 真实性闸门

| 能力 | truth_evidence | claim_limit |
|---|---|---|
| 零改动快速返回 | 实际连续构建计时、指纹测试 | 仅当前 Mac/toolchain |
| 修改后增量编译 | 触碰独立模块后的真实计时与产物 | 不等于 release clean |
| 发布可靠性 | 独立 release build path、fresh clean build、host suite | host/build verified；真机未刷写 |

claim_limit: 只按实测报告本机加速；不把 fast build 当作发布级 clean build。
