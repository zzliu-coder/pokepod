# Worklog

- 2026-08-10 17:02 baseline `c5b0d054c508a27b63d18c5d9040a8c18b726a35`, clean。
- Arduino CLI 1.3.1；官方文档确认稳定 build path 可复用对象，`--clean` 强制清空缓存，构建选项变化会自动擦除旧缓存。
- 第一次无源码变化增量：62.66 秒；第二次：58.21 秒。
- 292 个对象中仅 `sketch/PokePodAmoled.ino.cpp.o` 更新，确认厚 `.ino` 是热构建主瓶颈。
- Arduino GFX 1.6.5 原库含 208 个 `.h/.cpp`；PokePod 实际需要清单 21 个文件、编译 9 个对象。
- 第一次最小库冷缓存成功构建：339 秒；此前完整显示库构建运行 6 分 50 秒仍在编译无关驱动，主动终止，不作为成功数据。
- 零改动 fast cache hit：1.63 秒，最终冻结脚本复测 1.32 秒；旧基线为 58.21 秒。
- 单独修改 `PokePodApp.cpp`：29.42 秒；恢复该修改并确认只更新薄 `.ino` 与 `PokePodApp.cpp` 两个对象：37.96 秒。冻结脚本增量：21.74 秒。
- 完整 host suite：60/60 PASS；固定中文字库覆盖 105 个源文件、425 字形。
- 独立 release build path + `--clean`：404.09 秒 PASS；Flash 2,310,963 / 3,145,728 = 73%，RAM 103,644 / 327,680 = 31%，项目源码 warning 0。
- release GFX 对象数 9；release app SHA-256 `484a82963978b5394df0d24b919d026d6088a64ab20e03d74a00e3f4c0189f85`。
- release 后 fast 缓存未受损；最终 fast app SHA-256 `e9f439c7c22644088661e4edfa5760821356bfd6881d26b50f26237441bdddeb`。
- `git diff --check`、shell syntax、输入指纹合同和 diff 凭据扫描 PASS。
- 源码提交：`682edf50`（Optimize PokePod firmware build loop）。
