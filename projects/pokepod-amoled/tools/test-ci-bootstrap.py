#!/usr/bin/env python3
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile


TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / "bootstrap-ci.sh"


with tempfile.TemporaryDirectory(prefix="pokepod-ci-bootstrap-") as raw:
    root = Path(raw)
    bin_dir = root / "bin"
    bin_dir.mkdir()

    for command in (
        "python3", "clang++", "rg", "git", "sha256sum", "curl", "file",
        "tar", "sed", "uname",
    ):
        target = bin_dir / command
        target.write_text(
            "#!/bin/sh\n"
            "case \"$0\" in\n"
            "  *python3) echo 'Python 3.14.0' ;;\n"
            "  *clang++) echo 'clang version 20.0.0' ;;\n"
            "  *rg) echo 'ripgrep 14.1.1' ;;\n"
            "  *git) echo 'git version 2.50.0' ;;\n"
            "  *sha256sum) echo 'sha256sum 9.0' ;;\n"
            "  *curl) echo 'curl 8.0.0' ;;\n"
            "  *file) echo 'file-5.45' ;;\n"
            "  *tar) echo 'tar 1.35' ;;\n"
            "  *uname) echo 'Linux' ;;\n"
            "  *sed) /usr/bin/sed \"$@\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        target.chmod(0o755)

    environment = {"PATH": str(bin_dir)}
    passed = subprocess.run(
        ["/bin/sh", str(SCRIPT), "--verify"],
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    assert passed.returncode == 0, passed.stdout + passed.stderr
    assert "PASS pokepod_ci_bootstrap" in passed.stdout

    (bin_dir / "rg").unlink()
    missing = subprocess.run(
        ["/bin/sh", str(SCRIPT), "--verify"],
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    assert missing.returncode == 69, missing.stdout + missing.stderr
    assert "missing required tool: rg" in missing.stderr
    assert "--install" in missing.stderr

source = SCRIPT.read_text(encoding="utf-8")
assert "apt-get install -y --no-install-recommends" in source
assert "ripgrep" in source
assert "packages=\"${packages}${packages:+ }curl\"" in source
assert "packages=\"${packages}${packages:+ }file\"" in source
assert "packages=\"${packages}${packages:+ }tar\"" in source
assert "[ \"$install\" -eq 1 ]" in source
print("PASS test-ci-bootstrap")
