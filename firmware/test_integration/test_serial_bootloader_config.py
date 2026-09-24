from __future__ import annotations

import importlib.util
import json
import os
import re
import signal
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest
import requests

FLASH_SIZE_BYTES = 0x400000


class DummyConfig:
    def __init__(self, **options: object) -> None:
        self._options = options

    def getoption(self, name: str) -> object | None:
        return self._options.get(name)


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


def _watchdog_script_path() -> Path:
    return _repo_root() / "scripts/run_with_watchdog.py"


def _pre_commit_hook_path() -> Path:
    return _repo_root() / "tools/pre-commit-hook.sh"


def _load_watchdog_script():
    watchdog_path = _watchdog_script_path()
    spec = importlib.util.spec_from_file_location("run_with_watchdog", watchdog_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"failed to load watchdog script from {watchdog_path}")

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


def _init_pre_commit_test_repo(tmp_path: Path) -> Path:
    repo = tmp_path / "pre-commit-repo"
    cli_dir = repo / "cli"

    cli_dir.mkdir(parents=True)
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)

    (cli_dir / "go.mod").write_text(
        "module example.com/precommit\n\ngo 1.25.0\n",
        encoding="utf-8",
    )
    (cli_dir / "main.go").write_text(
        _gofmt_source(
            """\
            package main

            import "fmt"

            func main() {
                fmt.Printf("%s", "ok")
            }
            """
        ),
        encoding="utf-8",
    )

    subprocess.run(["git", "add", "cli/go.mod", "cli/main.go"], cwd=repo, check=True)
    return repo


def _gofmt_source(source: str) -> str:
    return textwrap.dedent(source)


def _install_fake_go_tools(tmp_path: Path) -> dict[str, str]:
    bin_dir = tmp_path / "fake-go-bin"
    bin_dir.mkdir()

    (bin_dir / "go").write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env python3
            from pathlib import Path
            import sys

            if len(sys.argv) < 2 or sys.argv[1] != "vet":
                raise SystemExit(2)

            for path in Path(".").rglob("*.go"):
                source = path.read_text(encoding="utf-8")
                if 'fmt.Printf("%d", "broken")' in source:
                    raise SystemExit(1)
                if 'fmt.Printf("%d", "worktree-only-breakage")' in source:
                    raise SystemExit(1)

            raise SystemExit(0)
            """
        ),
        encoding="utf-8",
    )
    (bin_dir / "gofmt").write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env python3
            import sys

            if len(sys.argv) > 1 and sys.argv[1] in {"-l", "-d"}:
                raise SystemExit(0)

            sys.stdout.write(sys.stdin.read())
            """
        ),
        encoding="utf-8",
    )

    for tool in (bin_dir / "go", bin_dir / "gofmt"):
        tool.chmod(0o755)

    env = os.environ.copy()
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    return env


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


def test_read_device_config_values_rejects_unexpected_dump_lines(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    module = _load_integration_conftest()
    outputs = iter(["", "device_cfg:api_token = token-123\nbad-line-without-separators\n"])

    monkeypatch.setenv("IDF_PATH", str(tmp_path / "idf"))
    monkeypatch.setattr(module, "_run_command", lambda _command: next(outputs))

    with pytest.raises(RuntimeError, match=r"unexpected nvs_tool output: 'bad-line-without-separators'"):
        module._read_device_config_values(_repo_root(), "/dev/esp32-evb", "115200")


def test_provision_script_disables_stub_for_parttool() -> None:
    script_text = (_repo_root() / "scripts/provision.sh").read_text(encoding="utf-8")
    assert "parttool_esptool_args=(--esptool-args no-stub)" in script_text
    assert 'port="${EVB_FLASH_PORT:-${EVB_SERIAL_PORT:-/dev/esp32-evb}}"' in script_text
    assert '"${download_mode_check}" --port "${port}" --baud "${baud}"' in script_text


def test_provision_script_preserves_wifi_and_network_policy_keys() -> None:
    script_text = (_repo_root() / "scripts/provision.sh").read_text(encoding="utf-8")
    preserve_match = re.search(r"for key in \((.*?)\):", script_text, re.DOTALL)

    assert preserve_match is not None
    preserved_keys = set(re.findall(r'"([^"]+)"', preserve_match.group(1)))
    assert {"poll_ms", "hostname", "modio_policy"} <= preserved_keys
    assert {"wifi_ssid", "wifi_pass", "net_policy"} <= preserved_keys


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


def test_test_device_recipe_wraps_pytest_in_whole_run_watchdog() -> None:
    justfile_text = (_repo_root() / "Justfile").read_text(encoding="utf-8")

    assert 'test_device_watchdog_seconds := env("EVB_TEST_DEVICE_WATCHDOG_SECONDS", "480")' in justfile_text
    assert "../../scripts/run_with_watchdog.py \\" in justfile_text
    assert "--timeout-seconds {{test_device_watchdog_seconds}} \\" in justfile_text


def test_pre_commit_hook_exports_only_cli_snapshot_for_go_vet() -> None:
    hook_text = _pre_commit_hook_path().read_text(encoding="utf-8")

    assert 'git ls-files -z cli | git checkout-index --stdin -z --prefix="$INDEX_SNAPSHOT_DIR/"' in hook_text
    assert "git checkout-index --all" not in hook_text


def test_watchdog_wrapper_preserves_child_output_and_exit_code() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_watchdog_script_path()),
            "--timeout-seconds",
            "5",
            "--",
            sys.executable,
            "-c",
            "import sys; print('watchdog-ok'); sys.exit(3)",
        ],
        check=False,
        text=True,
        capture_output=True,
    )

    assert result.returncode == 3
    assert result.stdout.strip() == "watchdog-ok"
    assert result.stderr == ""


def test_watchdog_wrapper_times_out_and_reports_the_command() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_watchdog_script_path()),
            "--timeout-seconds",
            "0.2",
            "--",
            sys.executable,
            "-c",
            "import time; print('watchdog-start', flush=True); time.sleep(10)",
        ],
        check=False,
        text=True,
        capture_output=True,
    )

    assert result.returncode == 124
    assert result.stdout.strip() == "watchdog-start"
    assert "command timed out after" in result.stderr
    assert "limit 0.2s" in result.stderr
    assert "time.sleep(10)" in result.stderr


def test_pre_commit_hook_rejects_bad_staged_go_even_when_worktree_is_fixed(
    tmp_path: Path,
) -> None:
    repo = _init_pre_commit_test_repo(tmp_path)
    env = _install_fake_go_tools(tmp_path)
    main_go = repo / "cli/main.go"

    main_go.write_text(
        _gofmt_source(
            """\
            package main

            import "fmt"

            func main() {
                fmt.Printf("%d", "broken")
            }
            """
        ),
        encoding="utf-8",
    )
    subprocess.run(["git", "add", "cli/main.go"], cwd=repo, check=True)

    main_go.write_text(
        _gofmt_source(
            """\
            package main

            import "fmt"

            func main() {
                fmt.Printf("%s", "fixed")
            }
            """
        ),
        encoding="utf-8",
    )

    worktree_vet = subprocess.run(
        ["go", "vet", "./..."],
        cwd=repo / "cli",
        check=False,
        text=True,
        capture_output=True,
        env=env,
    )
    assert worktree_vet.returncode == 0

    hook_result = subprocess.run(
        ["bash", str(_pre_commit_hook_path())],
        cwd=repo,
        check=False,
        text=True,
        capture_output=True,
        env=env,
    )

    assert hook_result.returncode != 0


def test_pre_commit_hook_ignores_unstaged_worktree_breakage_when_index_is_clean(
    tmp_path: Path,
) -> None:
    repo = _init_pre_commit_test_repo(tmp_path)
    env = _install_fake_go_tools(tmp_path)
    main_go = repo / "cli/main.go"

    main_go.write_text(
        _gofmt_source(
            """\
            package main

            import "fmt"

            func main() {
                fmt.Printf("%s", "staged")
            }
            """
        ),
        encoding="utf-8",
    )
    subprocess.run(["git", "add", "cli/main.go"], cwd=repo, check=True)

    main_go.write_text(
        _gofmt_source(
            """\
            package main

            import "fmt"

            func main() {
                fmt.Printf("%d", "worktree-only-breakage")
            }
            """
        ),
        encoding="utf-8",
    )

    worktree_vet = subprocess.run(
        ["go", "vet", "./..."],
        cwd=repo / "cli",
        check=False,
        text=True,
        capture_output=True,
        env=env,
    )
    assert worktree_vet.returncode != 0

    hook_result = subprocess.run(
        ["bash", str(_pre_commit_hook_path())],
        cwd=repo,
        check=False,
        text=True,
        capture_output=True,
        env=env,
    )

    assert hook_result.returncode == 0


def test_watchdog_signals_process_group_even_if_parent_already_exited(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_watchdog_script()
    killpg_calls: list[tuple[int, signal.Signals]] = []

    class FakeProcess:
        pid = 4242

        @staticmethod
        def poll() -> int:
            return 0

    monkeypatch.setattr(module.os, "name", "posix", raising=False)
    monkeypatch.setattr(module.os, "killpg", lambda pid, sig: killpg_calls.append((pid, sig)))

    module._signal_process_tree(FakeProcess(), signal.SIGTERM)

    assert killpg_calls == [(4242, signal.SIGTERM)]


def test_rest_api_reserves_uri_slots_for_all_registered_routes() -> None:
    source = (_repo_root() / "firmware/components/rest_api/rest_api.c").read_text(encoding="utf-8")
    config_source = (
        _repo_root() / "firmware/components/rest_api/rest_api_server_config.c"
    ).read_text(encoding="utf-8")
    header_source = (
        _repo_root() / "firmware/components/rest_api/rest_api_server_config.h"
    ).read_text(encoding="utf-8")
    match = re.search(r"#define REST_API_URI_HANDLER_COUNT\s+(\d+)U", source)

    assert match is not None
    assert "rest_api_make_httpd_config(config->port, REST_API_URI_HANDLER_COUNT);" in source
    assert "server_config.max_uri_handlers = max_uri_handlers;" in config_source
    assert "#define REST_API_HTTPD_STACK_SIZE 8192U" in header_source

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


def test_monitor_port_defaults_prefer_runtime_console(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_integration_conftest()

    monkeypatch.delenv("EVB_SERIAL_PORT", raising=False)
    assert module._default_monitor_port("/dev/esp32-evb-flash") == "/dev/esp32-evb-flash"

    monkeypatch.setenv("EVB_SERIAL_PORT", "/dev/esp32-evb-console")
    assert module._default_monitor_port("/dev/esp32-evb-flash") == "/dev/esp32-evb-console"


def test_monitor_baud_defaults_do_not_reuse_flash_baud(monkeypatch: pytest.MonkeyPatch) -> None:
    module = _load_integration_conftest()

    monkeypatch.delenv("EVB_FLASH_BAUD", raising=False)
    monkeypatch.delenv("EVB_SERIAL_BAUD", raising=False)
    assert module._default_monitor_baud() == "115200"

    monkeypatch.setenv("EVB_FLASH_BAUD", "921600")
    assert module._default_monitor_baud() == "115200"

    monkeypatch.setenv("EVB_SERIAL_BAUD", "74880")
    assert module._default_monitor_baud() == "74880"


def test_monitor_transport_from_config_prefers_explicit_overrides(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_integration_conftest()
    config = DummyConfig(
        port="/dev/cli-flash",
        baud="460800",
        monitor_port="/dev/cli-console",
        monitor_baud="57600",
    )

    monkeypatch.setenv("EVB_FLASH_PORT", "/dev/env-flash")
    monkeypatch.setenv("EVB_FLASH_BAUD", "921600")
    monkeypatch.setenv("EVB_SERIAL_PORT", "/dev/env-console")
    monkeypatch.setenv("EVB_SERIAL_BAUD", "115200")

    assert module._flash_port_from_config(config) == "/dev/cli-flash"
    assert module._flash_baud_from_config(config) == "460800"
    assert module._monitor_port_from_config(config, "/dev/cli-flash") == "/dev/cli-console"
    assert module._monitor_baud_from_config(config) == "57600"


def test_qemu_udev_example_defines_stable_serial_aliases() -> None:
    rule_text = (_repo_root() / "tools/udev/99-esp32-evb-qemu-serial.rules.example").read_text(
        encoding="utf-8"
    )

    assert 'ATTRS{vendor}=="0x1b36"' in rule_text
    assert 'ATTRS{device}=="0x0002"' in rule_text
    assert 'ATTRS{subsystem_vendor}=="0x1af4"' in rule_text
    assert 'ATTRS{subsystem_device}=="0x1100"' in rule_text
    assert 'SUBSYSTEMS=="pci"' in rule_text
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


def test_dut_endpoint_resolution_falls_back_to_monitor_transport(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_integration_conftest()
    serial_fallback_calls: list[tuple[str, str, float]] = []

    def _raise_runtime_error(host: str, timeout_seconds: float) -> str:
        raise RuntimeError("failed to resolve 'esp32-evb-relay.local': [Errno -3] Temporary failure in name resolution")

    monkeypatch.setattr(module, "_wait_for_host", _raise_runtime_error)
    monkeypatch.setattr(
        module,
        "_wait_for_ip_on_serial",
        lambda port, baud, timeout_seconds: serial_fallback_calls.append((port, baud, timeout_seconds)) or "192.0.2.55",
    )

    endpoint = module._resolve_dut_endpoint(
        "esp32-evb-relay.local",
        80,
        15.0,
        "/dev/esp32-evb-console",
        "74880",
    )

    assert endpoint.host == "esp32-evb-relay.local"
    assert endpoint.ip == "192.0.2.55"
    assert endpoint.port == 80
    assert endpoint.base_url == "http://192.0.2.55:80"
    assert serial_fallback_calls == [("/dev/esp32-evb-console", "74880", 30.0)]


def test_justfile_supports_split_flash_and_monitor_ports() -> None:
    justfile_text = (_repo_root() / "Justfile").read_text(encoding="utf-8")

    assert 'serial_port := env("EVB_SERIAL_PORT", "/dev/esp32-evb")' in justfile_text
    assert 'serial_baud := env("EVB_SERIAL_BAUD", "115200")' in justfile_text
    assert 'flash_port := env("EVB_FLASH_PORT", serial_port)' in justfile_text
    assert "./scripts/check-download-mode.sh --port {{flash_port}} --baud 115200" in justfile_text
    assert "--flash-port {{flash_port}}" in justfile_text
    assert "--port {{flash_port}}" in justfile_text
    assert "--monitor-port {{serial_port}}" in justfile_text
    assert "--monitor-baud {{serial_baud}}" in justfile_text


def test_cleanup_ignores_modio_not_present_service_unavailable() -> None:
    module = _load_integration_conftest()

    assert 503 in module.IGNORED_CLEANUP_STATUS_CODES


def test_restore_safe_relays_succeeds_while_modio_sync_is_unknown() -> None:
    module = _load_integration_conftest()

    class FakeResponse:
        def __init__(self, status_code: int) -> None:
            self.status_code = status_code

        def raise_for_status(self) -> None:
            if self.status_code >= 400:
                raise requests.HTTPError(f"{self.status_code} for PUT")

    class FreshBootDevice:
        """Mirrors firmware semantics right after boot: MOD-IO sync unknown."""

        def __init__(self) -> None:
            self.modio_sync_known = False
            self.requests: list[tuple[str, str, object]] = []

        def request(self, method: str, path: str, **kwargs: object) -> FakeResponse:
            body = kwargs.get("json")
            self.requests.append((method, path, body))
            if path == "/api/v1/relays/modio":
                self.modio_sync_known = True
                return FakeResponse(200)
            if path.startswith("/api/v1/relays/modio/") and not self.modio_sync_known:
                return FakeResponse(409)
            return FakeResponse(200)

    device = FreshBootDevice()

    module._restore_safe_relays(device)

    assert ("PUT", "/api/v1/relays/modio", {"states": [False, False, False, False]}) in device.requests
    assert not any(path.startswith("/api/v1/relays/modio/") for _, path, _ in device.requests)
    assert ("PUT", "/api/v1/relays/onboard/1", {"state": False}) in device.requests
    assert ("PUT", "/api/v1/relays/onboard/2", {"state": False}) in device.requests


def test_wait_for_http_ready_retries_until_status_succeeds(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_integration_conftest()
    current_time = {"value": 0.0}
    attempts: list[float] = []

    class FakeResponse:
        def __init__(self, status_code: int) -> None:
            self.status_code = status_code
            self.closed = False

        def close(self) -> None:
            self.closed = True

    success_response = FakeResponse(200)
    outcomes: list[object] = [
        requests.ConnectionError("not ready"),
        success_response,
    ]

    def _request(method: str, path: str, **kwargs: object) -> FakeResponse:
        attempts.append(float(kwargs["timeout"]))
        outcome = outcomes.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    client = module.IntegrationHttpClient(
        base_url="http://192.0.2.10:80",
        session=object(),
        timeout=5.0,
    )
    monkeypatch.setattr(client, "request", _request)
    monkeypatch.setattr(module.time, "monotonic", lambda: current_time["value"])
    monkeypatch.setattr(
        module.time,
        "sleep",
        lambda seconds: current_time.__setitem__("value", current_time["value"] + seconds),
    )

    module._wait_for_http_ready(client, 5.0)

    assert attempts == [3.0, 3.0]
    assert success_response.closed is True


def test_wait_for_http_ready_reports_last_status_on_timeout(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_integration_conftest()
    current_time = {"value": 0.0}
    closed_responses: list[int] = []

    class FakeResponse:
        def __init__(self, status_code: int) -> None:
            self.status_code = status_code

        def close(self) -> None:
            closed_responses.append(self.status_code)

    client = module.IntegrationHttpClient(
        base_url="http://192.0.2.10:80",
        session=object(),
        timeout=2.5,
    )
    monkeypatch.setattr(
        client,
        "request",
        lambda method, path, **kwargs: FakeResponse(503),
    )
    monkeypatch.setattr(module.time, "monotonic", lambda: current_time["value"])
    monkeypatch.setattr(
        module.time,
        "sleep",
        lambda seconds: current_time.__setitem__("value", current_time["value"] + seconds),
    )

    with pytest.raises(RuntimeError, match=r"last status 503"):
        module._wait_for_http_ready(client, 2.0)

    assert closed_responses == [503, 503]


def test_http_client_fixture_waits_for_http_ready(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_integration_conftest()
    wait_calls: list[tuple[str, float, str]] = []

    def _fake_wait(client, timeout_seconds: float) -> None:
        wait_calls.append(
            (
                client.base_url,
                timeout_seconds,
                client.session.headers["Authorization"],
            )
        )

    monkeypatch.setenv("EVB_HTTP_READY_TIMEOUT_SECONDS", "12.5")
    monkeypatch.setattr(module, "_wait_for_http_ready", _fake_wait)

    endpoint = module.DutEndpoint(
        host="esp32-evb-relay.local",
        ip="192.0.2.10",
        port=80,
        base_url="http://192.0.2.10:80",
    )
    generator = module.http_client.__wrapped__("token-123", endpoint)
    client = next(generator)

    try:
        assert client.base_url == endpoint.base_url
        assert wait_calls == [("http://192.0.2.10:80", 12.5, "Bearer token-123")]
    finally:
        generator.close()


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
