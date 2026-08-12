#!/usr/bin/env python3
"""Round-trip a real source audit archive through its source-only gate."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


SOURCE_PROJECT = Path(__file__).resolve().parents[1]
PROJECT_RELATIVE = Path("projects/pokepod-amoled")
IGNORED_COPY_NAMES = {
    ".cache",
    ".git",
    ".pio",
    "__pycache__",
    "build",
    "output",
    "work",
}


def run(
    *args: str,
    cwd: Path,
    check: bool = True,
    timeout: int = 300,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        args,
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
        timeout=timeout,
        env=env,
    )
    if check and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(args)}\n"
            f"stdout:\n{completed.stdout[-8000:]}\n"
            f"stderr:\n{completed.stderr[-8000:]}"
        )
    return completed


def copy_project(destination: Path) -> None:
    def ignore(_: str, names: list[str]) -> set[str]:
        return {name for name in names if name in IGNORED_COPY_NAMES}

    shutil.copytree(SOURCE_PROJECT, destination, ignore=ignore)


def write(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_extracted_package(extract_root: Path) -> Path:
    roots = [path for path in extract_root.iterdir() if path.is_dir()]
    assert len(roots) == 1, f"unexpected archive roots: {roots}"
    project = roots[0]
    audit = project / "_audit"
    manifest_path = audit / "SOURCE_MANIFEST.json"
    checksums_path = audit / "SOURCE_MANIFEST.sha256"
    assert manifest_path.is_file() and checksums_path.is_file()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["schema"] == "pokepod.source-audit.v1"
    assert manifest["sourceOnly"] is True
    assert manifest["sensitiveScan"]["status"] == "pass"
    records = manifest["files"]
    assert manifest["fileCount"] == len(records)
    expected_lines: list[str] = []
    for record in records:
        relative = Path(record["path"])
        payload = project / relative
        assert payload.is_file(), f"manifest file missing: {relative}"
        assert payload.stat().st_size == record["bytes"], (
            f"size mismatch: {relative}: {payload.stat().st_size} != {record['bytes']}"
        )
        actual_sha = sha256(payload)
        assert actual_sha == record["sha256"], (
            f"SHA mismatch: {relative}: {actual_sha} != {record['sha256']}"
        )
        expected_lines.append(f"{record['sha256']}  {relative.as_posix()}\n")
    assert checksums_path.read_text(encoding="utf-8") == "".join(expected_lines)
    assert not (project / "assets/cjk20.a4").exists()
    assert (project / "firmware/run-source-only-tests.sh").is_file()
    return project


with tempfile.TemporaryDirectory(prefix="pokepod-source-audit-roundtrip-") as raw:
    root = Path(raw)
    repo = root / "repo"
    project = repo / PROJECT_RELATIVE
    project.parent.mkdir(parents=True)
    copy_project(project)

    run("git", "init", "-q", cwd=repo)
    run("git", "config", "user.name", "PokePod Test", cwd=repo)
    run("git", "config", "user.email", "pokepod@example.invalid", cwd=repo)
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-qm", "fixture", cwd=repo)

    script = project / "tools/package-source-audit.py"
    first = root / "first.zip"
    second = root / "second.zip"
    outputs = []
    for output in (first, second):
        result = run(
            sys.executable,
            str(script),
            "--repo",
            str(repo),
            "--output",
            str(output),
            cwd=repo,
        )
        outputs.append(json.loads(result.stdout))
    assert first.read_bytes() == second.read_bytes(), "audit ZIP is not deterministic"
    assert outputs[0]["sha256"] == sha256(first)
    assert outputs[1]["sha256"] == sha256(second)

    extract_root = root / "extract"
    with zipfile.ZipFile(first) as archive:
        names = archive.namelist()
        assert len(names) == len(set(names)), "audit ZIP contains duplicate entries"
        archive.extractall(extract_root)
    extracted_project = verify_extracted_package(extract_root)
    source_gate = run(
        "sh",
        str(extracted_project / "firmware/run-source-only-tests.sh"),
        cwd=extracted_project,
        timeout=600,
        env=os.environ.copy(),
    )
    assert "PASS source_only_gate" in source_gate.stdout

    write(project / "untracked.txt", "dirty\n")
    dirty = run(
        sys.executable,
        str(script),
        "--repo",
        str(repo),
        "--output",
        str(root / "dirty.zip"),
        cwd=repo,
        check=False,
    )
    assert dirty.returncode == 2
    assert "requires a clean" in dirty.stderr
    (project / "untracked.txt").unlink()

    write(project / ".env", "TOKEN=secret\n")
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-qm", "sensitive path fixture", cwd=repo)
    sensitive = run(
        sys.executable,
        str(script),
        "--repo",
        str(repo),
        "--output",
        str(root / "sensitive.zip"),
        cwd=repo,
        check=False,
    )
    assert sensitive.returncode == 6
    assert "sensitive files refuse" in sensitive.stderr

    run("git", "rm", "-q", str(PROJECT_RELATIVE / ".env"), cwd=repo)
    write(project / "private.txt", "-----BEGIN " + "PRIVATE KEY-----\nsecret\n")
    run("git", "add", ".", cwd=repo)
    run("git", "commit", "-qm", "sensitive content fixture", cwd=repo)
    private_content = run(
        sys.executable,
        str(script),
        "--repo",
        str(repo),
        "--output",
        str(root / "private-content.zip"),
        cwd=repo,
        check=False,
    )
    assert private_content.returncode == 6
    assert "sensitive content refuses" in private_content.stderr

print("PASS test-source-audit-package (fresh ZIP, manifest, SHA, source gate, sensitive scan)")
