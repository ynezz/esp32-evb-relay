from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _load_integration_conftest():
    conftest_path = _repo_root() / "firmware/test_integration/conftest.py"
    spec = importlib.util.spec_from_file_location("integration_conftest", conftest_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"failed to load integration conftest from {conftest_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_firmware_build_disables_download_stub() -> None:
    flasher_args_path = _repo_root() / "firmware/build/flasher_args.json"
    if not flasher_args_path.exists():
        pytest.skip("firmware build artifacts are required for this regression check")

    flasher_args = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    assert flasher_args["extra_esptool_args"]["stub"] is False


def test_integration_parttool_uses_rom_bootloader_only() -> None:
    module = _load_integration_conftest()
    command = module._parttool_command(
        Path("/tmp/parttool.py"),
        Path("/tmp/partitions.csv"),
        "/dev/ttyS4",
        "115200",
        "read_partition",
        "--partition-name",
        "nvs",
    )

    assert command[:10] == [
        sys.executable,
        "/tmp/parttool.py",
        "-f",
        "/tmp/partitions.csv",
        "--esptool-args",
        "no-stub",
        "-p",
        "/dev/ttyS4",
        "-b",
        "115200",
    ]


def test_provision_script_disables_stub_for_parttool() -> None:
    script_text = (_repo_root() / "scripts/provision.sh").read_text(encoding="utf-8")
    assert "parttool_esptool_args=(--esptool-args no-stub)" in script_text
