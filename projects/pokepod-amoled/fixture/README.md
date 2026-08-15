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

普通 OTA 只接受与 BIN 同目录的 `artifact.json`，并自动绑定其中
`imageIdentity.sourceRevision`、`imageIdentity.firmwareVersion` 和
`imageIdentity.appElfSha256`。缺少清洁身份、身份字段不完整或清单与 BIN 的 SHA/大小不一致时，
主机在发送首个数据帧前拒绝更新。直接调用 `cdc-status.py --firmware` 也遵守同一门禁；只有显式
同时提供 `--source-revision`、`--firmware-version`、`--app-elf-sha256` 才能使用没有相邻清单的镜像。

固件先把镜像写入未运行的 OTA 槽，流式计算 SHA-256，并从候选槽的 ESP 应用描述读取
真实 `app_elf_sha256`。设备在 `esp_ota_end` 和切换启动槽之前强制比较
`sourceRevision`、`firmwareVersion` 和候选 ELF SHA；缺字段、全零或不匹配都会中止 OTA，
不改变当前启动槽。提交后的重启属于设备级重启意图，USB 断开发生在返回 OK 后不会取消重启。
Fixture 随后等待应用重新出现，再核验运行分区和三项身份；更新失败会中止 OTA 句柄。

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

## 自动 BOOT/RESET 与故障恢复

`pokepod_fixture_controller.ino` 是参考控制器固件，适合带原生 USB 串口的 RP2040、
ESP32-C3 或兼容 Arduino 小板。控制器用受保护的开漏输出连接 PokePod 的 BOOT 与
RESET；可选的负载开关控制 VBUS。电脑通过 `pokepod-fixture-bridge.py` 发送固定协议，
无需把任何控制逻辑放进 PokePod 机身。

控制器接好后，把 `control-profile.example.json` 复制到私有工作目录，将 backend 改为
`command`，并配置以下 argv 数组：

```json
{
  "schema": "pokepod.fixture.control.v1",
  "backend": "command",
  "actions": {
    "ping": ["python3", "/absolute/project/fixture/pokepod-fixture-bridge.py", "--port", "/dev/cu.usbmodemFIXTURE", "--action", "ping"],
    "assert_boot": ["python3", "/absolute/project/fixture/pokepod-fixture-bridge.py", "--port", "/dev/cu.usbmodemFIXTURE", "--action", "assert-boot"],
    "release_boot": ["python3", "/absolute/project/fixture/pokepod-fixture-bridge.py", "--port", "/dev/cu.usbmodemFIXTURE", "--action", "release-boot"],
    "release_reset": ["python3", "/absolute/project/fixture/pokepod-fixture-bridge.py", "--port", "/dev/cu.usbmodemFIXTURE", "--action", "release-reset"],
    "pulse_reset": ["python3", "/absolute/project/fixture/pokepod-fixture-bridge.py", "--port", "/dev/cu.usbmodemFIXTURE", "--action", "pulse-reset"]
  }
}
```

先检查控制能力，再执行全自动 ROM 救援：

```sh
python3 fixture/pokepod-fixture.py doctor --control-profile work/control.json
python3 fixture/pokepod-fixture.py recover \
  --control-profile work/control.json \
  --port-pattern '/dev/cu.usbmodemPOKEPOD*' \
  --authority work/device-authority.json \
  --expected-device-id pokepod-xxxxxxxxxxxx \
  --mode fast
```

`recover` 会依次拉低 BOOT、脉冲 RESET、释放 BOOT、确认唯一 ROM 端口、执行身份门禁、
备份 app0、刷写、完整回读，再等待同一个 Link deviceId 返回。任何步骤失败都会留下
独立证据并停止写入。

## 自动场景复现

下面两条命令会保留前后身份、状态和运行时诊断。场景失联时，配置了控制器便会自动
进入 ROM 恢复并恢复已验证的 Fast 固件：

```sh
python3 fixture/pokepod-fixture.py exercise --scenario recording \
  --port /dev/cu.usbmodemPOKEPOD --auto-recover \
  --control-profile work/control.json --authority work/device-authority.json

python3 fixture/pokepod-fixture.py exercise --scenario provisioning \
  --port /dev/cu.usbmodemPOKEPOD --auto-recover \
  --control-profile work/control.json --authority work/device-authority.json
```

普通 OTA 仍是优先路径；它不需要 BOOT/RESET。控制器承担 OTA 失败、固件崩溃、USB
Link 无响应时的自动 ROM 救援。仅有普通 USB 线时，电脑无法在电气上拉低 GPIO0 与
RESET，软件会明确返回 `manual`，不会声称已经具备自动救援。

所有命令都会在 `work/fixture-runs/<timestamp>-<operation>/` 保存身份、诊断、控制器
输出、命令和 SHA 证据。`collect` 与 `doctor` 只读；`exercise` 会触发产品功能；
`update`、`recover` 和 `flash` 会写固件。

控制器中的 BOOT 断言带三秒 deadman。即使主机已经拉低 BOOT 但串口 ACK 丢失，
控制器也会自动释放；主机在进入 ROM 流程的任何失败点同样会 best-effort 释放 BOOT。
`doctor` 会真实执行 PING、RESET RELEASE 和 BOOT RELEASE，输出机器可读的
`evidence.json`，不会只根据 profile 文本报告“可用”。
