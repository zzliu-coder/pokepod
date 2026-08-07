#!/usr/bin/env python3
"""Move Tencent credentials from Android PokeCapsule into PokePod safely."""

from __future__ import annotations

import argparse
import getpass
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid


ROOT = Path(__file__).resolve().parent
REMOTE_ROOT = "/sdcard/PokeCapsule"
PACKAGE = "com.zheliu.pokecapsule"
SECRET_ID = re.compile(
    r"(?im)^\s*SecretId\b[ \t]*(?:[:=：][ \t]*)?[\"'`]?([A-Za-z0-9]+)"
)
SECRET_KEY = re.compile(
    r"(?im)^\s*SecretKey\b[ \t]*(?:[:=：][ \t]*)?[\"'`]?([A-Za-z0-9]+)"
)


def run_adb(adb: str, serial: str, *arguments: str,
            timeout: float = 15.0) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [adb, "-s", serial, *arguments], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=timeout, check=False,
    )


def connected_android(adb: str, requested: str | None) -> str:
    result = subprocess.run(
        [adb, "devices"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=10, check=True, text=True,
    )
    devices = [line.split("\t", 1)[0] for line in result.stdout.splitlines()
               if "\tdevice" in line]
    if requested:
        if requested not in devices:
            raise RuntimeError("指定的 Android 设备没有处于 ADB device 状态")
        return requested
    if len(devices) != 1:
        raise RuntimeError(f"需要恰好一台 ADB 设备，当前检测到 {len(devices)} 台")
    return devices[0]


def export_android_credentials(adb: str, serial: str) -> tuple[str, str]:
    transaction = str(uuid.uuid4())
    relative_stage = f".staging/{transaction}/tencent.txt"
    remote_stage_dir = f"{REMOTE_ROOT}/.staging/{transaction}"
    remote_stage = f"{REMOTE_ROOT}/{relative_stage}"
    remote_command = f"{REMOTE_ROOT}/.commands/{transaction}.json"
    remote_result = f"{REMOTE_ROOT}/.commands/results/{transaction}.json"
    command = {
        "schemaVersion": 2,
        "transactionId": transaction,
        "operation": "exportTencentCredentials",
        "stagedPath": relative_stage,
    }
    try:
        mkdir = run_adb(adb, serial, "shell", "mkdir", "-p", remote_stage_dir)
        if mkdir.returncode != 0:
            raise RuntimeError("无法创建 Android 临时密钥目录")
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", prefix="pokepod-command-",
                suffix=".json") as command_file:
            json.dump(command, command_file, separators=(",", ":"))
            command_file.flush()
            pushed = run_adb(adb, serial, "push", command_file.name,
                             remote_command)
        if pushed.returncode != 0:
            raise RuntimeError("无法提交 Android 密钥导出命令")
        broadcast = run_adb(
            adb, serial, "shell", "am", "broadcast",
            "-a", f"{PACKAGE}.PROCESS_COMMAND",
            "-n", f"{PACKAGE}/.command.CommandReceiver",
            "--es", "commandFile", f"{transaction}.json",
        )
        if broadcast.returncode != 0:
            raise RuntimeError("Android 拒绝执行密钥导出命令")

        deadline = time.monotonic() + 15
        result_payload: dict[str, object] | None = None
        while time.monotonic() < deadline:
            result = run_adb(adb, serial, "exec-out", "cat", remote_result,
                             timeout=3)
            if result.returncode == 0 and result.stdout.strip():
                result_payload = json.loads(result.stdout.decode("utf-8"))
                break
            time.sleep(0.25)
        if not result_payload or not result_payload.get("ok"):
            raise RuntimeError("Android 没有生成有效的密钥导出结果")
        exported = run_adb(adb, serial, "exec-out", "cat", remote_stage)
        if exported.returncode != 0:
            raise RuntimeError("无法读取 Android 临时密钥")
        text = exported.stdout.decode("utf-8")
        identifier = SECRET_ID.search(text)
        secret = SECRET_KEY.search(text)
        if not identifier or not secret:
            raise RuntimeError("Android 导出的腾讯配置格式无效")
        return identifier.group(1), secret.group(1)
    finally:
        # All targets are scoped by the fresh UUID.  Cleanup is mandatory so
        # plaintext never remains on Android shared storage.
        run_adb(adb, serial, "shell", "rm", "-f", remote_stage,
                remote_command, remote_result, timeout=5)
        run_adb(adb, serial, "shell", "rmdir", remote_stage_dir, timeout=5)


def link_module():
    source = ROOT / "cdc-status.py"
    spec = importlib.util.spec_from_file_location("pokepod_cdc", source)
    if spec is None or spec.loader is None:
        raise RuntimeError("无法加载 PokePod Link 客户端")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-serial")
    parser.add_argument("--adb", default=os.environ.get("ADB", "adb"))
    parser.add_argument("--port", default="")
    parser.add_argument("--wifi-ssid", default="")
    parser.add_argument(
        "--wifi-password-env", default="POKEPOD_WIFI_PASSWORD",
        help="读取 Wi-Fi 密码的环境变量名；不把密码放进命令行",
    )
    arguments = parser.parse_args()

    serial = connected_android(arguments.adb, arguments.android_serial)
    secret_id, secret_key = export_android_credentials(arguments.adb, serial)
    values: dict[str, object] = {
        "secretId": secret_id,
        "secretKey": secret_key,
    }
    if arguments.wifi_ssid:
        password = os.environ.get(arguments.wifi_password_env)
        if password is None and sys.stdin.isatty():
            password = getpass.getpass("Wi-Fi 密码（不会显示）: ")
        if password is None:
            raise RuntimeError(
                f"请设置环境变量 {arguments.wifi_password_env} 或在终端交互输入密码"
            )
        values.update({
            "wifiSsid": arguments.wifi_ssid,
            "wifiPassword": password,
            "wifiEnabled": True,
        })

    client = link_module()
    ports = [arguments.port] if arguments.port else sorted(
        str(path) for path in Path("/dev").glob("cu.usbmodem*")
    )
    last_error: Exception | None = None
    for port in ports:
        try:
            response = client.query(
                port, "configure", 8, fields={"values": values})
            if response.get("status") != "ok" or not response.get(
                    "tencentConfigured", False):
                raise RuntimeError("PokePod 没有确认腾讯配置")
            print(json.dumps({
                "status": "ok",
                "androidSerial": serial,
                "host_port": port,
                "tencentConfigured": True,
                "wifiConfigured": bool(response.get("wifiConfigured", False)),
            }, ensure_ascii=False, separators=(",", ":")))
            return 0
        except Exception as error:  # report only a redacted error
            last_error = error
    raise RuntimeError(f"PokePod 配置失败：{last_error or '没有 CDC 端口'}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(2)
