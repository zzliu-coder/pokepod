#!/usr/bin/env python3
"""Security source must be trackable while explicit local secrets stay local."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
GITIGNORE = ROOT / ".gitignore"


def ignored(path: str) -> bool:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "check-ignore", "--no-index", "-q", "--", path]
    )
    return result.returncode == 0


source = GITIGNORE.read_text(encoding="utf-8")
for forbidden in ("*secret*", "*token*", "*credential*", "*password*"):
    assert forbidden not in source

for trackable in (
    "projects/pokepod-amoled/firmware/PokePodAmoled/SecureWipe.h",
    "projects/pokepod-amoled/firmware/tests/test_secure_wipe.cpp",
    "docs/security-token-lifecycle.md",
    "fixtures/credential-redaction.json",
    "fixtures/password-policy.txt",
):
    assert not ignored(trackable), trackable

for private in (
    ".env",
    ".env.production",
    "device.pem",
    "device.key",
    "secrets/cloud.txt",
    ".secrets/pairing.bin",
    "projects/pokepod-amoled/local-secrets/tencent.txt",
    "projects/pokepod-amoled/credentials.local.json",
    "projects/pokepod-amoled/tokens.local.json",
    "projects/pokepod-amoled/passwords.local.txt",
):
    assert ignored(private), private

print("PASS security_ignore_contract")
