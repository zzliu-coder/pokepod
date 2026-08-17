#!/usr/bin/env python3
"""Security source must be trackable while explicit local secrets stay local."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
REPOSITORY = PROJECT.parents[1]
GITIGNORE = PROJECT / ".gitignore"


def ignored(path: str) -> bool:
    with tempfile.TemporaryDirectory(prefix="pokepod-ignore-contract-") as raw:
        root = Path(raw)
        (root / ".gitignore").write_text(source, encoding="utf-8")
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        result = subprocess.run(
            ["git", "-C", str(root), "check-ignore", "--no-index", "-q",
             "--", path]
        )
        return result.returncode == 0


source = GITIGNORE.read_text(encoding="utf-8")
for forbidden in ("*secret*", "*token*", "*credential*", "*password*"):
    assert forbidden not in source

for trackable in (
    "firmware/PokePodAmoled/SecureWipe.h",
    "firmware/tests/test_secure_wipe.cpp",
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
    "local-secrets/tencent.txt",
    "credentials.local.json",
    "tokens.local.json",
    "passwords.local.txt",
):
    assert ignored(private), private

print("PASS security_ignore_contract")
