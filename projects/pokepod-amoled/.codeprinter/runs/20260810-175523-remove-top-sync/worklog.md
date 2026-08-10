# Worklog

- 2026-08-10 17:55 baseline `c2d5adf5bccaf1bec679aff7cf976af60e00543b`, clean。
- `c5b0d054c508a27b63d18c5d9040a8c18b726a35` is ancestor of baseline。
- 当前 macOS 未枚举 USB 设备或 PokePod 串口；刷写闸门保持关闭。
- 用户截图确认删除目标为三个根页面共享顶栏的“同步”胶囊；设备页“与电脑同步”保留。
- 源码提交：`673089c65542998cafc7e74b8f4343d5220fe411`。
- host suite：60/60 PASS；CJK 固定字库覆盖 105 sources、425 glyphs x 16/20/28。
- release build：208 秒；Flash 2,310,583/3,145,728=73%；RAM 103,644/327,680=31%；项目源码 warning 0。
- release app：2,310,832 bytes；SHA-256 `38cee3b6a34247b1b2c36cb13b950c0926668b685ae5ffc9d56b73ac3259af77`。
- USB 重新枚举：PokePod V1，PokeCapsule，VID/PID 303A:1001，serial `6C192890A994`；CDC status `ok`。
- ROM 身份：ESP32-S3 rev 0.2、MAC `94:a9:90:28:19:6c`、16 MB Flash、8 MB PSRAM。
- 当前 app0 新备份：3,145,728 bytes；SHA-256 `a5497fc141ec7ef2d14d3c1426e12da28c78c49f2c17cb44a775eb14fd6e4edf`；首尾 64 KB 独立重读哈希一致。
- stub 模式长读失败后改用 ROM no-stub 读取成功；残缺读取没有作为备份。
- 第一轮 flash 在任何写入前因 no-stub 状态拒绝 command 0x8；重新建立 stub 后 app-only write、内部 hash 和独立 verify-flash 均成功。
- 刷写回执：`PASS pokepod_flash_verified`，app SHA 与 release 产物一致。
- 刷写后 CDC：`status=ok`，variant `V1 SH8601/FT3168`，display/touch/audio/SD/RTC/IMU/PMU 均 true，pendingCapsules=1。
