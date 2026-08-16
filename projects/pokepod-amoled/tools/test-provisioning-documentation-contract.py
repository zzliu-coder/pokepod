#!/usr/bin/env python3
"""Keep provisioning documentation aligned with the production security policy."""

from pathlib import Path
import re


project_dir = Path(__file__).parents[1]
firmware_dir = project_dir / "firmware" / "PokePodAmoled"

readme = (project_dir / "README.md").read_text(encoding="utf-8")
design = (project_dir / "design" / "ui-v3-compositions.svg").read_text(
    encoding="utf-8"
)
policy = (firmware_dir / "ProvisioningPolicy.h").read_text(encoding="utf-8")
portal = (firmware_dir / "ProvisioningPortal.cpp").read_text(encoding="utf-8")

# First bind the documentation contract to the production facts it describes.
assert "constexpr size_t kProvisioningPasswordLength = 10;" in policy
alphabet_match = re.search(
    r'kProvisioningPasswordAlphabet\[\]\s*=\s*"([^"]+)"', policy
)
assert alphabet_match is not None
password_alphabet = alphabet_match.group(1)
assert password_alphabet == "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
assert "constexpr uint32_t kPortalLifetimeMs = 5UL * 60UL * 1000UL;" in portal

# The written operating instructions must state every user-visible security fact.
for expected in (
    "\u6bcf\u6b21\u542f\u52a8\u914d\u7f6e\u70ed\u70b9\u90fd\u4f1a\u751f\u6210\u65b0\u7684 10 \u4f4d\u975e\u6df7\u6dc6\u968f\u673a\u5bc6\u7801",
    "\u53ea\u663e\u793a\u5728\u8bbe\u5907\n\u5c4f\u5e55\u4e0a",
    "\u4e94\u5206\u949f\u5185\u6709\u6548",
    "\u7a97\u53e3\u5173\u95ed\u6216\u5230\u671f\u540e",
    "\u7acb\u5373\u5931\u6548\u5e76\u4ece\u5185\u5b58\u4e2d\u6e05\u9664",
    "\u66f4\u6362\u6216\u6e05\u7a7a\u5df2\u4fdd\u5b58\u7684\u817e\u8baf\u4e91\u5bc6\u94a5",
    "\u5fc5\u987b\u6309\u8bbe\u5907\u5b9e\u4f53\u952e",
    "个人设备固定口令模式",
    "启用前必须在配网页面明确确认",
    "固定模式只适合个人设备",
):
    assert expected in readme

assert "88888888" not in readme
assert "\u914d\u7f6e\u70ed\u70b9\u5bc6\u7801\u56fa\u5b9a" not in readme

# The UI composition is illustrative, while its sample still obeys the real
# 10-character alphabet and labels the five-minute/physical-confirmation rules.
provisioning_design = design.split("\u624b\u673a\u914d\u7f51 \u00b7 \u5b50\u9875", 1)[1]
sample_match = re.search(
    r'<text class="display"[^>]*>([^<]+)</text>', provisioning_design
)
assert sample_match is not None
sample_password = sample_match.group(1)
assert len(sample_password) == 10
assert all(character in password_alphabet for character in sample_password)
assert "\u968f\u673a\u5bc6\u7801 \u00b7 \u6bcf\u6b21\u66f4\u65b0" in provisioning_design
assert "5 \u5206\u949f\u5185\u6709\u6548 \u00b7 \u53ea\u663e\u793a\u5728\u8bbe\u5907\u4e0a" in provisioning_design
assert "\u817e\u8baf\u5bc6\u94a5\u53d8\u66f4\u9700\u6309\u5b9e\u4f53\u952e" in provisioning_design
assert "88888888" not in provisioning_design

print("PASS test_provisioning_documentation_contract")
