#!/usr/bin/env python3
"""Exercise the shared protocol and repository-integration contracts."""

from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / "projects" / "pokepod-amoled"
PROTOCOL = ROOT / "projects" / "pokecapsule-protocol"
VALIDATOR = PROTOCOL / "validate-fixtures.py"
WORKFLOW = ROOT / ".github" / "workflows" / "repository-integration.yml"
GATES = PROJECT / "tools" / "test-gates.json"


def run_validator(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), "--root", str(root)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_protocol_positive_and_negative_cases() -> None:
    passed = run_validator(PROTOCOL)
    assert passed.returncode == 0, passed.stdout + passed.stderr
    assert "PASS protocol_fixtures" in passed.stdout

    with tempfile.TemporaryDirectory(prefix="pokepod-protocol-contract-") as raw:
        copied = Path(raw) / "protocol"
        shutil.copytree(PROTOCOL, copied)

        command_path = copied / "example" / "move-command.json"
        command = json.loads(command_path.read_text(encoding="utf-8"))
        del command["expectedRevisions"]
        command_path.write_text(
            json.dumps(command, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        missing_field = run_validator(copied)
        assert missing_field.returncode != 0
        assert "expectedRevisions" in missing_field.stderr

        command_path.write_text(
            json.dumps(
                json.loads((PROTOCOL / "example" / "move-command.json").read_text()),
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        schema_path = copied / "processing.schema.json"
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        schema["$ref"] = "missing.schema.json"
        # The real processing schema has no root ref. Add a local ref under a
        # harmless optional branch so the graph walk must still reject it.
        schema["properties"] = {
            **schema.get("properties", {}),
            "testOnly": {"$ref": "missing.schema.json"},
        }
        schema_path.write_text(json.dumps(schema, indent=2) + "\n", encoding="utf-8")
        missing_ref = run_validator(copied)
        assert missing_ref.returncode != 0
        assert "$ref target missing" in missing_ref.stderr


def test_workflow_contract() -> None:
    source = WORKFLOW.read_text(encoding="utf-8")
    for required in (
        "name: Repository integration",
        "pull_request:",
        "workflow_dispatch:",
        "merge-result-firmware:",
        "MERGE_RESULT_SHA: ${{ github.event.pull_request.merge_commit_sha || github.sha }}",
        "ref: ${{ env.MERGE_RESULT_SHA }}",
        "mac:",
        "swift test --parallel",
        "swift build -c release",
        "android:",
        "actions/setup-java@cf277c60eb25467037889841efdb72551f06f6c3",
        "gradle/actions/setup-gradle@ed408507eac070d1f99cc633dbcf757c94c7933a",
        'gradle-version: "8.14.5"',
        "./build.sh",
        "testPoke3LegacyDebugUnitTest",
        "testPhoneModernDebugUnitTest",
        "protocol:",
        "python3 projects/pokecapsule-protocol/validate-fixtures.py",
        "sys.version_info[:2] == (3, 12)",
        "permissions:\n  contents: read",
    ):
        assert required in source, f"repository workflow contract missing: {required}"
    for forbidden in (
        "./flash.sh",
        "esptool",
        "serial",
        "build.sh --release",
        "if: always()",
        "permissions:\n  contents: write",
    ):
        assert forbidden not in source, f"forbidden workflow operation: {forbidden}"

    uses = re.findall(r"^\s*uses:\s*([^#\s]+)(?:\s*#\s*(\S+))?\s*$", source, re.MULTILINE)
    assert uses, "workflow has no action pin declarations"
    for action, version in uses:
        assert re.fullmatch(r"[^@]+@[0-9a-f]{40}", action), action
        assert version, f"action pin has no version annotation: {action}"


def test_governance_and_test_registry() -> None:
    codeowners = (ROOT / ".github" / "CODEOWNERS").read_text(encoding="utf-8")
    assert "@zzliu-coder" in codeowners
    assert "/.github/" in codeowners
    assert "/projects/pokecapsule-protocol/" in codeowners

    security = (ROOT / "SECURITY.md").read_text(encoding="utf-8")
    for required in (
        "Supported versions",
        "Reporting a vulnerability",
        "GitHub Security Advisories",
        "不通过串口或刷写设备收集安全报告",
        "Secure Boot",
    ):
        assert required in security, required

    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for required in (
        "repository-integration.yml",
        "merge-result",
        "Mac",
        "Android",
        "protocol",
        "branch protection",
        "unverified",
    ):
        assert required in readme, required

    gates = json.loads(GATES.read_text(encoding="utf-8"))
    assert "test-whole-repo-audit-integration.py" in gates["repository"]
    assert "test-fixture-boot-ack-loss.py" in gates["source"]
    assert "test-fixture-build-identity.py" in gates["source"]
    assert "test-fixture-link-behavior.py" in gates["source"]
    assert "test-rescue-flash-policy.py" in gates["source"]
    assert "test-rescue-partition-layout.py" in gates["source"]
    pending = gates.get("pending", {})
    assert pending["android"][0]["path"] == "projects/pokecapsule-android/test-android-variants.py"
    assert pending["android"][0]["owner"] == "L5"


if __name__ == "__main__":
    test_protocol_positive_and_negative_cases()
    test_workflow_contract()
    test_governance_and_test_registry()
    print("PASS whole_repo_audit_integration_contract")
