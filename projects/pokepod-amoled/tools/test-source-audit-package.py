#!/usr/bin/env python3
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


SCRIPT = Path(__file__).with_name("package-source-audit.py")


def run(*args: str, cwd: Path, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, text=True, capture_output=True, check=check)


def write(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


with tempfile.TemporaryDirectory(prefix="pokepod-source-audit-") as raw:
    repo = Path(raw)
    run("git", "init", "-q", cwd=repo)
    run("git", "config", "user.name", "PokePod Test", cwd=repo)
    run("git", "config", "user.email", "pokepod@example.invalid", cwd=repo)
    write(repo / "projects/pokepod-amoled/README.md", "source\n")
    write(repo / "projects/pokepod-amoled/firmware/main.cpp", "int main() { return 0; }\n")
    write(repo / "projects/pokepod-amoled/tools/check.py", "#!/usr/bin/env python3\n")
    (repo / "projects/pokepod-amoled/assets").mkdir(parents=True)
    (repo / "projects/pokepod-amoled/assets/font.bin").write_bytes(b"binary")
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-qm", "fixture", cwd=repo)

    first = repo / "first.zip"
    second = repo / "second.zip"
    for output in (first, second):
        run(sys.executable, str(SCRIPT), "--repo", str(repo), "--output", str(output), cwd=repo)

    assert hashlib.sha256(first.read_bytes()).digest() == hashlib.sha256(second.read_bytes()).digest()
    with zipfile.ZipFile(first) as archive:
        names = archive.namelist()
        assert any(name.endswith("/firmware/main.cpp") for name in names)
        assert any(name.endswith("/_audit/SOURCE_MANIFEST.json") for name in names)
        assert not any(name.endswith("font.bin") for name in names)

    write(repo / "projects/pokepod-amoled/untracked.txt", "dirty\n")
    failed = run(
        sys.executable,
        str(SCRIPT),
        "--repo",
        str(repo),
        "--output",
        str(repo / "dirty.zip"),
        cwd=repo,
        check=False,
    )
    assert failed.returncode == 2
    assert "requires a clean" in failed.stderr
    (repo / "projects/pokepod-amoled/untracked.txt").unlink()

    write(repo / "projects/pokepod-amoled/.env", "TOKEN=secret\n")
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-qm", "sensitive fixture", cwd=repo)
    sensitive = run(
        sys.executable,
        str(SCRIPT),
        "--repo",
        str(repo),
        "--output",
        str(repo / "sensitive.zip"),
        cwd=repo,
        check=False,
    )
    assert sensitive.returncode == 6
    assert "sensitive files refuse" in sensitive.stderr

    run("git", "rm", "-q", "projects/pokepod-amoled/.env", cwd=repo)
    (repo / "projects/pokepod-amoled/firmware/private.der").write_bytes(b"der-private-key")
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-qm", "binary key fixture", cwd=repo)
    binary_key = run(
        sys.executable,
        str(SCRIPT),
        "--repo",
        str(repo),
        "--output",
        str(repo / "binary-key.zip"),
        cwd=repo,
        check=False,
    )
    assert binary_key.returncode == 6
    assert "private.der" in binary_key.stderr

print("PASS test-source-audit-package")
