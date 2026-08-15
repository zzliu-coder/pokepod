# PokePod 夹具

夹具把身份识别、日志采集、备份、升级、回读校验和复位控制放在电脑端。设备端只需要
USB D+/D−、GND、受限 VBUS、BOOT、RESET 六类电气触点；USB 正常工作时 BOOT/RESET
保持释放。

## 两条工作路径：Normal upgrade / Rescue upgrade

普通升级走已启动固件的 USB CDC Link v2，不需要用户按 BOOT/RESET：

```sh
python3 fixture/pokepod-fixture.py probe --port /dev/cu.usbmodemXXXX
python3 fixture/pokepod-fixture.py collect --port /dev/cu.usbmodemXXXX
python3 fixture/pokepod-fixture.py update \
  --port /dev/cu.usbmodemXXXX \
  --firmware work/pokepod-build/output/fast/PokePodAmoled.ino.bin
```

固件先把镜像写入未运行的 OTA 槽，流式计算 SHA-256，校验完成后才切换启动槽并
提交设备级重启意图。USB 断开发生在返回 OK 后不会取消重启。更新失败会中止 OTA
句柄，不改变当前启动槽。

救援路径保留 BOOT/RESET：

```sh
python3 fixture/pokepod-fixture.py flash \
  --rom-port /dev/cu.usbmodemROM \
  --authority work/device-authority.json \
  --mode fast
```

该路径调用现有 `flash.sh`，在任何写入前读取并保存 app0 backup（备份）、SHA 和恢复计划，写入
后完整回读并比较 SHA，再等待应用 CDC 重新出现。身份不唯一、芯片不是 ESP32-S3、
不是 16 MiB、authority 不匹配或备份/回读失败都会在写入前终止。

## 夹具控制器边界

本仓库的 Python 夹具工具不假设某个 GPIO 桥接芯片，也不会自动把 BOOT/RESET 当作
串口信号驱动。夹具硬件应提供受电平保护的开漏 BOOT/RESET 控制器；控制器接入后，
只需把触点定义与 `fixture/pinout.json` 对齐。当前正常升级路径完全不依赖该控制器，
BOOT/RESET 仅作为 ROM 救援通道。

所有命令都会在 `work/fixture-runs/<timestamp>-<operation>/` 保存身份、诊断、命令、
输出和 SHA 证据。`collect` 只读设备；`update` 和 `flash` 是明确的写操作。
