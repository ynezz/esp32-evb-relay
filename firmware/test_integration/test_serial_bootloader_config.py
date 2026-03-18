from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path

import pytest

FLASH_SIZE_BYTES = 0x400000


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


def _load_partition_table():
    partition_rows = []
    partition_path = _repo_root() / "firmware/partitions.csv"

    for raw_line in partition_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if (not line) or line.startswith("#"):
            continue

        fields = [field.strip() for field in raw_line.split(",")]
        partition_rows.append(
            {
                "name": fields[0],
                "type": fields[1],
                "subtype": fields[2],
                "offset": int(fields[3], 0),
                "size": int(fields[4], 0),
            }
        )

    return partition_rows


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
        "/dev/esp32-evb",
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
        "/dev/esp32-evb",
        "-b",
        "115200",
    ]


def test_provision_script_disables_stub_for_parttool() -> None:
    script_text = (_repo_root() / "scripts/provision.sh").read_text(encoding="utf-8")
    assert "parttool_esptool_args=(--esptool-args no-stub)" in script_text
    assert 'port="${EVB_FLASH_PORT:-${EVB_SERIAL_PORT:-/dev/esp32-evb}}"' in script_text
    assert '"${download_mode_check}" --port "${port}" --baud "${baud}"' in script_text


def test_flash_script_runs_download_mode_preflight() -> None:
    script_text = (_repo_root() / "scripts/flash.sh").read_text(encoding="utf-8")
    assert 'download_mode_check="${repo_root}/scripts/check-download-mode.sh"' in script_text
    assert '"${download_mode_check}" --port "${port}" --baud "${baud}"' in script_text


def test_download_mode_helper_documents_runner_failure_signatures() -> None:
    script_text = (_repo_root() / "scripts/check-download-mode.sh").read_text(encoding="utf-8")
    assert "chip_id" in script_text
    assert "--before default_reset" in script_text
    assert "--after hard_reset" in script_text
    assert "--console-port <serial-port>" in script_text
    assert 'console_port="${EVB_SERIAL_PORT:-}"' in script_text
    assert "Detected console activity on" in script_text
    assert "/docs/hardware-reference.md" in script_text
    assert "Failed to enter ESP32 ROM download mode" in script_text


def test_rest_api_reserves_uri_slots_for_all_registered_routes() -> None:
    source = (_repo_root() / "firmware/components/rest_api/rest_api.c").read_text(encoding="utf-8")
    match = re.search(r"#define REST_API_URI_HANDLER_COUNT\s+(\d+)U", source)

    assert match is not None
    assert "server_config.max_uri_handlers = REST_API_URI_HANDLER_COUNT;" in source

    registered_routes = len(re.findall(r"httpd_register_uri_handler\(", source))
    assert int(match.group(1)) == registered_routes


def test_rest_api_closes_unauthorized_connections() -> None:
    source = (_repo_root() / "firmware/components/rest_api/rest_api.c").read_text(encoding="utf-8")

    assert 'httpd_resp_set_hdr(req, "Connection", "close");' in source


def test_rest_api_uses_supported_onboard_toggle_uri_template() -> None:
    source = (_repo_root() / "firmware/components/rest_api/rest_api.c").read_text(encoding="utf-8")

    assert re.search(
        r'httpd_uri_t onboard_relay_toggle_uri = \{\s+'
        r'\.uri = "/api/v1/relays/onboard/\*",\s+'
        r'\.method = HTTP_POST,',
        source,
    )


def test_flash_port_defaults_prefer_explicit_override(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_integration_conftest()

    monkeypatch.delenv("EVB_FLASH_PORT", raising=False)
    monkeypatch.delenv("EVB_SERIAL_PORT", raising=False)
    assert module._default_flash_port() == "/dev/esp32-evb"

    monkeypatch.setenv("EVB_SERIAL_PORT", "/dev/ttyUSB0")
    assert module._default_flash_port() == "/dev/ttyUSB0"

    monkeypatch.setenv("EVB_FLASH_PORT", "/dev/esp32-evb-flash")
    monkeypatch.setenv("EVB_SERIAL_PORT", "/dev/esp32-evb-console")
    assert module._default_flash_port() == "/dev/esp32-evb-flash"


def test_qemu_udev_example_defines_stable_serial_aliases() -> None:
    rule_text = (_repo_root() / "tools/udev/99-esp32-evb-qemu-serial.rules.example").read_text(
        encoding="utf-8"
    )

    assert 'KERNELS=="0000:00:09.0"' in rule_text
    assert 'SYMLINK+="esp32-evb"' in rule_text


def test_dut_endpoint_resolution_uses_resolved_host_ip(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_integration_conftest()
    monkeypatch.setattr(module, "_wait_for_host", lambda host, timeout_seconds: "192.0.2.10")

    endpoint = module._resolve_dut_endpoint("esp32-evb-relay.local", 8080, 15.0)

    assert endpoint.host == "esp32-evb-relay.local"
    assert endpoint.ip == "192.0.2.10"
    assert endpoint.port == 8080
    assert endpoint.base_url == "http://192.0.2.10:8080"


def test_dut_endpoint_resolution_failures_are_fatal(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_integration_conftest()

    def _raise_runtime_error(host: str, timeout_seconds: float) -> str:
        raise RuntimeError("failed to resolve 'esp32-evb-relay.local': [Errno -2] Name or service not known")

    monkeypatch.setattr(module, "_wait_for_host", _raise_runtime_error)

    with pytest.raises(pytest.fail.Exception, match="could not resolve DUT host"):
        module._resolve_dut_endpoint("esp32-evb-relay.local", 80, 15.0)


def test_dut_endpoint_resolution_falls_back_to_serial_ip(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_integration_conftest()

    def _raise_runtime_error(host: str, timeout_seconds: float) -> str:
        raise RuntimeError("failed to resolve 'esp32-evb-relay.local': [Errno -3] Temporary failure in name resolution")

    monkeypatch.setattr(module, "_wait_for_host", _raise_runtime_error)
    monkeypatch.setattr(module, "_wait_for_ip_on_serial", lambda port, baud, timeout_seconds: "192.0.2.55")

    endpoint = module._resolve_dut_endpoint("esp32-evb-relay.local", 80, 15.0, "/dev/esp32-evb", "115200")

    assert endpoint.host == "esp32-evb-relay.local"
    assert endpoint.ip == "192.0.2.55"
    assert endpoint.port == 80
    assert endpoint.base_url == "http://192.0.2.55:80"


def test_justfile_supports_split_flash_and_monitor_ports() -> None:
    justfile_text = (_repo_root() / "Justfile").read_text(encoding="utf-8")

    assert 'flash_port := env("EVB_FLASH_PORT", serial_port)' in justfile_text
    assert "./scripts/check-download-mode.sh --port {{flash_port}} --baud 115200" in justfile_text
    assert "--flash-port {{flash_port}}" in justfile_text
    assert "test_integration \\\n        --port {{flash_port}}" in justfile_text


def test_cleanup_ignores_modio_not_present_service_unavailable() -> None:
    module = _load_integration_conftest()

    assert 503 in module.IGNORED_CLEANUP_STATUS_CODES


def test_partition_table_restores_nvs_size_and_flash_headroom() -> None:
    rows = _load_partition_table()
    partitions_by_name = {row["name"]: row for row in rows}

    assert partitions_by_name["nvs"]["size"] == 0x6000
    assert partitions_by_name["ota_0"]["size"] == 0x1E0000
    assert partitions_by_name["ota_1"]["size"] == 0x1E0000
    assert partitions_by_name["ota_0"]["offset"] % 0x10000 == 0
    assert partitions_by_name["ota_1"]["offset"] % 0x10000 == 0

    total_slack = 0
    previous_end = min(row["offset"] for row in rows)

    for row in sorted(rows, key=lambda item: item["offset"]):
        assert row["offset"] >= previous_end, f"partition overlap at {row['name']}"
        total_slack += row["offset"] - previous_end
        previous_end = row["offset"] + row["size"]

    total_slack += FLASH_SIZE_BYTES - previous_end
    assert total_slack >= 0x1E000
