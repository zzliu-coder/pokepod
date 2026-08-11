#!/usr/bin/env python3
"""Validate a manifest-backed PokePod artifact before any device access."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


APP_SLOT_BYTES = 0x300000
POLICY_SOURCE = Path(__file__).with_name("flash-size-policy.py")
SPEC = importlib.util.spec_from_file_location("pokepod_flash_size_policy", POLICY_SOURCE)
assert SPEC and SPEC.loader
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)


def fail(message: str) -> None:
    raise SystemExit(f"FAIL {message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--expected-lane", choices=("fast", "release"))
    args = parser.parse_args()

    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"artifact_manifest_invalid error={exc}")
    if manifest.get("schemaVersion") != 1 or manifest.get("kind") != "hardmac.artifact":
        fail("artifact_manifest_kind")
    lane = manifest.get("lane")
    if lane not in {"fast", "release"}:
        fail("artifact_manifest_lane")
    if args.expected_lane and lane != args.expected_lane:
        fail(f"artifact_lane_mismatch expected={args.expected_lane} actual={lane}")
    if lane == "release" and manifest.get("sourceDirty") is not False:
        fail("release_artifact_source_dirty")
    source_revision = manifest.get("sourceRevision")
    if not isinstance(source_revision, str) or not source_revision.strip():
        fail("artifact_source_revision")

    binary = manifest.get("binary")
    if not isinstance(binary, dict) or binary.get("file") != args.binary.name:
        fail("artifact_manifest_binary_name")
    payload = args.binary.read_bytes()
    if binary.get("sizeBytes") != len(payload):
        fail("artifact_manifest_binary_size")
    actual_sha = hashlib.sha256(payload).hexdigest()
    if binary.get("sha256") != actual_sha:
        fail("artifact_manifest_binary_sha256")

    expected_policy = POLICY.evaluate(len(payload), APP_SLOT_BYTES)
    resource_policy = manifest.get("resourcePolicy")
    if resource_policy != expected_policy:
        fail("artifact_resource_policy_mismatch")
    if not expected_policy["releaseAllowed"]:
        fail("artifact_resource_red")
    expected_binary_resource = {
        "slotSizeBytes": APP_SLOT_BYTES,
        "remainingBytes": expected_policy["remainingBytes"],
        "usagePercent": expected_policy["percent"],
        "resourceTier": expected_policy["tier"],
    }
    for key, expected in expected_binary_resource.items():
        if binary.get(key) != expected:
            fail(f"artifact_binary_resource_mismatch field={key}")

    resource_review = manifest.get("resourceReview")
    if not isinstance(resource_review, dict):
        fail("artifact_resource_review")
    review_required = expected_policy["tier"] in ("yellow", "orange")
    if resource_review.get("required") is not review_required:
        fail("artifact_resource_review_required")
    if review_required:
        relative = resource_review.get("evidenceFile")
        if relative != "../../resource-review.json":
            fail("resource_review_path")
        review_path = (args.manifest.parent / relative).resolve()
        try:
            review = json.loads(review_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            fail(f"resource_review_missing error={exc}")
        errors = POLICY.validate_release_review(
            expected_policy, review, source_revision, actual_sha
        )
        if errors:
            fail("resource_review_invalid fields=" + ",".join(errors))

    print(f"PASS artifact_manifest lane={lane} sha256={actual_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
