#!/usr/bin/env python3
"""Validate a manifest-backed PokePod artifact before any device access."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct


APP_SLOT_BYTES = 0x300000
APP_FLASH_OFFSET = "0x10000"
PRODUCTION_CORE_VERSION = "3.3.8"
IDENTITY_MAGIC = b"PKPDIMG2"
IDENTITY_SCHEMA = 2
IDENTITY_FORMAT = "<8sHH24s16s41sB3s41s65s"
IDENTITY_BYTES = struct.calcsize(IDENTITY_FORMAT)
POLICY_SOURCE = Path(__file__).with_name("flash-size-policy.py")
SPEC = importlib.util.spec_from_file_location("pokepod_flash_size_policy", POLICY_SOURCE)
assert SPEC and SPEC.loader
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)


def fail(message: str) -> None:
    raise SystemExit(f"FAIL {message}")


def text_field(value: bytes, field: str) -> str | None:
    if b"\0" not in value:
        return None
    try:
        decoded = value.split(b"\0", 1)[0].decode("ascii", errors="strict")
    except UnicodeDecodeError:
        return None
    if not decoded:
        return None
    return decoded


def require_sha(value: str, field: str, lengths: tuple[int, ...] = (40, 64)) -> str:
    if len(value) not in lengths or re.fullmatch(r"[0-9a-fA-F]+", value) is None:
        fail(f"image_identity_{field}_invalid")
    return value.lower()


def extract_image_identity(payload: bytes) -> dict[str, object]:
    identities: list[dict[str, object]] = []
    cursor = 0
    while True:
        offset = payload.find(IDENTITY_MAGIC, cursor)
        if offset < 0:
            break
        cursor = offset + 1
        if offset + IDENTITY_BYTES > len(payload):
            continue
        fields = struct.unpack_from(IDENTITY_FORMAT, payload, offset)
        magic, schema, struct_bytes, product, version, revision, dirty, _, tree, elf = fields
        if magic != IDENTITY_MAGIC or schema != IDENTITY_SCHEMA or struct_bytes != IDENTITY_BYTES:
            continue
        product_text = text_field(product, "product")
        version_text = text_field(version, "firmware_version")
        revision_text = text_field(revision, "source_revision")
        tree_text = text_field(tree, "source_tree")
        elf_text = text_field(elf, "app_elf_sha256")
        if None in (product_text, version_text, revision_text, tree_text, elf_text):
            continue
        if product_text != "PokePodAmoled" or dirty not in (0, 1):
            continue
        if re.fullmatch(r"[0-9a-fA-F]{40}", revision_text) is None:
            continue
        if re.fullmatch(r"[0-9a-fA-F]{40}", tree_text) is None:
            continue
        if elf_text != "unknown" and re.fullmatch(r"[0-9a-fA-F]{64}", elf_text) is None:
            continue
        identities.append(
            {
                "offset": offset,
                "product": product_text,
                "firmwareVersion": version_text,
                "sourceRevision": revision_text.lower(),
                "sourceTree": tree_text.lower(),
                "sourceDirty": bool(dirty),
                "appElfSha256": elf_text.lower(),
            }
        )
    if len(identities) != 1:
        fail(f"image_identity_count actual={len(identities)}")
    return identities[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--source-tree")
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
    if lane == "release" and re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", source_revision) is None:
        fail("release_artifact_source_revision")

    toolchain = manifest.get("toolchain")
    if not isinstance(toolchain, dict):
        fail("artifact_toolchain")
    core_profile = toolchain.get("coreProfile")
    core_version = toolchain.get("esp32ArduinoCore")
    if core_profile not in {"production", "matrix"} or not isinstance(core_version, str):
        fail("artifact_toolchain_profile")
    if lane == "release" and (
        core_profile != "production" or core_version != PRODUCTION_CORE_VERSION
    ):
        fail("release_artifact_toolchain")

    binary = manifest.get("binary")
    if not isinstance(binary, dict) or binary.get("file") != args.binary.name:
        fail("artifact_manifest_binary_name")
    if binary.get("flashOffset") != APP_FLASH_OFFSET:
        fail("artifact_flash_offset")
    payload = args.binary.read_bytes()
    if binary.get("sizeBytes") != len(payload):
        fail("artifact_manifest_binary_size")
    actual_sha = hashlib.sha256(payload).hexdigest()
    if binary.get("sha256") != actual_sha:
        fail("artifact_manifest_binary_sha256")

    identity = extract_image_identity(payload)
    if manifest.get("firmwareVersion") != identity["firmwareVersion"]:
        fail("artifact_identity_firmware_version")
    source_revision = str(source_revision).lower()
    if source_revision != identity["sourceRevision"]:
        fail("artifact_identity_source_revision")
    manifest_tree = manifest.get("sourceTree")
    if not isinstance(manifest_tree, str):
        fail("artifact_source_tree")
    manifest_tree = require_sha(manifest_tree, "source_tree", (40, 64))
    if args.source_tree and manifest_tree != require_sha(args.source_tree, "expected_source_tree", (40, 64)):
        fail("artifact_source_tree_expected")
    if manifest_tree != identity["sourceTree"]:
        fail("artifact_identity_source_tree")
    if bool(manifest.get("sourceDirty")) != identity["sourceDirty"]:
        fail("artifact_identity_source_dirty")
    image_identity = manifest.get("imageIdentity")
    if not isinstance(image_identity, dict):
        fail("artifact_image_identity")
    for key in ("magic", "schema", "product", "firmwareVersion", "sourceRevision", "sourceTree", "sourceDirty", "appElfSha256"):
        if key not in image_identity:
            fail(f"artifact_image_identity_{key}")
    if image_identity.get("magic") != IDENTITY_MAGIC.decode("ascii") or image_identity.get("schema") != IDENTITY_SCHEMA:
        fail("artifact_image_identity_schema")
    if image_identity.get("product") != identity["product"] or image_identity.get("firmwareVersion", identity["firmwareVersion"]) != identity["firmwareVersion"]:
        fail("artifact_image_identity_product")
    if image_identity.get("sourceRevision") != identity["sourceRevision"] or image_identity.get("sourceTree") != identity["sourceTree"] or bool(image_identity.get("sourceDirty")) != identity["sourceDirty"]:
        fail("artifact_image_identity_source")
    declared_elf = image_identity.get("appElfSha256")
    if not isinstance(declared_elf, str) or (declared_elf != "unknown" and re.fullmatch(r"[0-9a-fA-F]{64}", declared_elf) is None):
        fail("artifact_image_identity_elf")
    if args.elf:
        if not args.elf.is_file():
            fail("artifact_elf_missing")
        actual_elf = hashlib.sha256(args.elf.read_bytes()).hexdigest()
        if declared_elf != actual_elf:
            fail("artifact_image_identity_elf_sha256")
        if image_identity.get("appElfSha256") != actual_elf:
            fail("artifact_image_identity_elf_manifest")

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
        if lane == "release" and resource_review.get("approved") is not True:
            fail("release_resource_review_not_approved")
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
