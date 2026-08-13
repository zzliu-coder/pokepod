#!/usr/bin/env python3
"""Keep the documented release and recovery policy aligned with flash.sh."""

from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]
REPOSITORY = PROJECT.parents[1]
project_readme = (PROJECT / "README.md").read_text(encoding="utf-8")
root_readme_path = REPOSITORY / "README.md"
root_readme = (
    root_readme_path.read_text(encoding="utf-8")
    if root_readme_path.is_file()
    else None
)

for required in (
    "每次刷写都会在任何写入前现场读取完整 3 MiB app0",
    "偏移 `0x10000`",
    "current-app0.bin",
    "restore-plan.json",
    "标准恢复只按该计划把这份\napp0 备份写回 `0x10000`",
    "保留 NVS、分区表、板载 FAT 分区和 SD 卡",
    "完整 16 MiB Flash 双备份属于历史留档或特殊恢复流程",
    "不能混入标准发布步骤",
    "密钥迁移脚本只通过正在运行的固件执行 Link v2 `configure`",
    "--identity-authority work/hardmac-authorities/my-pokepod.json",
    "pokepod.flash-identity-authority",
    "不能\n提交到仓库或从当前待刷设备临时自生成",
    "--rom-port /dev/cu.usbmodemXXXX",
    "不会自动选择“唯一的 usbmodem”",
    "都会在设备备份和写入前终止",
):
    assert required in project_readme, f"release documentation contract missing: {required}"

for obsolete in (
    "正式刷写前必须保留两次逐字节一致的原始 16 MB Flash 备份",
    "合并固件从偏移 `0x0` 写入",
    "完整\n应用分区执行回读校验",
    "原始 16 MB 备份",
):
    assert obsolete not in project_readme, f"obsolete release policy remains: {obsolete}"

# The source-audit ZIP deliberately starts at projects/pokepod-amoled, so the
# repository README is absent there. Validate the repository handoff whenever
# the test runs from the complete checkout; the project recovery contract above
# remains mandatory in both environments.
if root_readme is not None:
    for required in (
        "2278bb11af4c97f83e71e6e3529b440e80810419",
        "feature/pokepod-amoled-1.8",
        "codex/pokepod-audit-remediation-integration",
        "590b549cb49b9a02064846673b51d26c585ee8a7",
        "native Git\nhistory",
    ):
        assert required in root_readme, (
            f"repository handoff contract missing: {required}"
        )

    for obsolete in (
        "imported root",
        "`main` branch preserves the imported audit baseline",
        "agent/audit-remediation",
    ):
        assert obsolete not in root_readme, (
            f"obsolete repository handoff remains: {obsolete}"
        )

print("PASS release_documentation_contract")
