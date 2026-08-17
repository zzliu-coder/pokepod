#!/usr/bin/env python3
"""Offline contract tests for the Poke3/modern Android product matrix."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent
APP = ROOT / "app"
ANDROID_NS = "http://schemas.android.com/apk/res/android"
NAME = f"{{{ANDROID_NS}}}name"


def fail(message: str) -> None:
    raise AssertionError(message)


def manifest_permissions(path: Path) -> set[str]:
    root = ET.parse(path).getroot()
    return {
        node.attrib.get(NAME, "")
        for node in root.findall("uses-permission")
    }


def main() -> int:
    build = (APP / "build.gradle").read_text(encoding="utf-8")
    if "flavorDimensions 'device'" not in build:
        fail("device flavor dimension is missing")
    if "poke3Legacy" not in build or "targetSdk 28" not in build:
        fail("Poke3 legacy flavor contract is missing")
    if "phoneModern" not in build or "targetSdk 34" not in build:
        fail("modern phone flavor contract is missing")
    if "signingConfig signingConfigs.debug" in build:
        fail("release still references the debug signing key")
    if "requireReleaseSigning" not in build or "releaseStoreFile" not in build:
        fail("external release signing contract is missing")
    for marker in (
        "def requireReleaseSigner",
        "graph.allTasks.any",
        "Release tasks require all four external signer properties",
    ):
        if marker not in build:
            fail(f"release fail-closed marker is missing: {marker}")

    main_permissions = manifest_permissions(APP / "src/main/AndroidManifest.xml")
    legacy_permissions = manifest_permissions(
        APP / "src/poke3Legacy/AndroidManifest.xml")
    modern_permissions = manifest_permissions(
        APP / "src/phoneModern/AndroidManifest.xml")
    if "android.permission.WRITE_EXTERNAL_STORAGE" in main_permissions:
        fail("legacy storage permission leaked into common manifest")
    if "android.permission.WRITE_EXTERNAL_STORAGE" not in legacy_permissions:
        fail("legacy storage permission is missing")
    for permission in (
        "android.permission.MANAGE_EXTERNAL_STORAGE",
        "android.permission.FOREGROUND_SERVICE_MICROPHONE",
        "android.permission.POST_NOTIFICATIONS",
    ):
        if permission not in modern_permissions:
            fail(f"modern permission is missing: {permission}")
    if "android.permission.MANAGE_EXTERNAL_STORAGE" in legacy_permissions:
        fail("modern all-files permission leaked into legacy flavor")

    activity = (APP / "src/main/java/com/zheliu/pokecapsule/ui/MainActivity.java")
    activity_source = activity.read_text(encoding="utf-8")
    for marker in (
        "Environment.isExternalStorageManager()",
        "LibraryStorageAccess.has(this)",
        "请授权 PokeCapsule 文件访问权限",
        "ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION",
    ):
        if marker not in activity_source:
            fail(f"modern access gate marker is missing: {marker}")

    for source in (
        APP / "src/main/java/com/zheliu/pokecapsule/PokeCapsuleApp.java",
        APP / "src/main/java/com/zheliu/pokecapsule/service/RecordingService.java",
        APP / "src/main/java/com/zheliu/pokecapsule/service/TranscriptionJobService.java",
        APP / "src/main/java/com/zheliu/pokecapsule/command/CommandReceiver.java",
    ):
        if "LibraryStorageAccess.has" not in source.read_text(encoding="utf-8"):
            fail(f"storage access gate missing from {source}")

    version_file = ROOT / "gradle-version.txt"
    if version_file.read_text(encoding="utf-8").strip() != "8.14.5":
        fail("Gradle version is not pinned to 8.14.5")
    build_script = ROOT / "build.sh"
    if not os.access(build_script, os.X_OK):
        fail("build.sh is not executable")
    subprocess.run(["zsh", "-n", str(build_script)], check=True)
    build_script_source = build_script.read_text(encoding="utf-8")
    for marker in (
        "POKECAPSULE_RELEASE_STORE_FILE",
        "gradle_signing_properties+=(\"-PreleaseStoreFile=$release_store_file\")",
        "explicit_release_store_file",
        '"${gradle_signing_properties[@]}"',
    ):
        if marker not in build_script_source:
            fail(f"environment signing conversion marker is missing: {marker}")

    installer = ROOT.parent.parent / "artifacts" / "安装-PokeCapsule-1.7.command"
    installer_source = installer.read_text(encoding="utf-8")
    subprocess.run(["zsh", "-n", str(installer)], check=True)
    if "/Users/zheliu/" in installer_source:
        fail("portable installer still contains a user-specific absolute path")
    for marker in ("SCRIPT_DIR=${0:A:h}", "APK_ARG=${1:-}",
                   "多个 APK", "POKECAPSULE_BACKUP_ROOT"):
        if marker not in installer_source:
            fail(f"portable installer marker is missing: {marker}")

    release_test = ROOT / "test-release-signing.sh"
    if not os.access(release_test, os.X_OK):
        fail("release signing focused test is not executable")
    subprocess.run(["zsh", "-n", str(release_test)], check=True)
    release_test_source = release_test.read_text(encoding="utf-8")
    for marker in ("缺少 signer", "完整 signer", "debug", "assemblePoke3LegacyRelease"):
        if marker not in release_test_source:
            fail(f"release signing test marker is missing: {marker}")

    print("PASS android_variant_contracts")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, ET.ParseError) as error:
        print(f"FAIL android_variant_contracts: {error}", file=sys.stderr)
        raise SystemExit(1)
