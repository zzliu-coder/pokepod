# PokePod CI 与仓库治理合同

## 源码内强制门

- `tools/bootstrap-ci.sh --install` 是 GitHub-hosted Ubuntu 的唯一宿主依赖入口；本机和外审环境使用 `--verify`，不会隐式安装软件。
- C++ 每个 compile/run 和 Python 每个测试默认分别限时 60 秒。首个真实错误会打印测试名、阶段、命令、退出码或 timeout，再停止同一门内其余进程。
- Source、asset、ASan/UBSan、repository audit、锁定的 Arduino-ESP32 3.3.8 和 forced clean Fast 依次执行。
- Fast 构建本身编译并链接全部生产翻译单元，包括 `BleVoiceService.cpp` 和 `CapsuleLibrary.cpp`；CI 随后检查其 `.o`、最终 ELF/MAP 和 exact-SHA evidence。
- 上传包必须包含 BIN、ELF、MAP、build log、artifact、Flash/RAM 与资源审查、largest symbols、candidate summary、candidate manifest 和 SHA-256 清单。任何缺件、dirty source、SHA 不一致或资源红线都会失败。
- 第三方 Action 使用完整 commit SHA，并在行尾保留可读版本注释。workflow 权限保持 `contents: read`，不执行串口、烧录、Release 或仓库写操作。

## GitHub 外部状态

以下状态不能由源码提交证明。集成提交推送后，仓库管理员必须在 GitHub 设置中验证并保存外部证据：

1. `main` branch protection 已启用。
2. 合并前必须通过 `Source, asset, sanitizer, audit package` 与 `Locked toolchain and clean forced Fast build`。
3. 禁止绕过 required checks；直接 push 权限按仓库策略收紧。
4. 最新 exact commit 的 workflow 绿色，artifact 可下载，重新计算的 SHA-256 与包内清单一致。
5. 提交签名状态单独记录，未验证时保持 `unverified`。

源码合同检查只能报告这些要求存在，不能声明 GitHub 外部设置已经完成。
