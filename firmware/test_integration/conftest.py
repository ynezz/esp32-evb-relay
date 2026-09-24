from __future__ import annotations

import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterator, Mapping

import pytest
import requests
import serial

os.environ.setdefault("ESPBAUD", "115200")

DEFAULT_DUT_HOST = "esp32-evb-relay.local"
DEFAULT_HTTP_PORT = 80
DEFAULT_FLASH_PORT = "/dev/esp32-evb"
DEFAULT_FLASH_BAUD = "115200"
DEFAULT_MONITOR_BAUD = "115200"
DEFAULT_REQUEST_TIMEOUT = 5.0
DEFAULT_DISCOVERY_TIMEOUT = 15.0
DEFAULT_SERIAL_ENDPOINT_TIMEOUT = 30.0
DEFAULT_HTTP_READY_TIMEOUT = 30.0
DEFAULT_HTTP_READY_POLL_SECONDS = 1.0
DEFAULT_HTTP_READY_REQUEST_TIMEOUT = 3.0
DEFAULT_BOOT_SETTLE_SECONDS = 3.0
DEFAULT_CLI_TIMEOUT_SECONDS = 30.0
DEFAULT_CLI_REQUEST_TIMEOUT = "5s"
PARTTOOL_ESPTOOL_ARGS = ("--esptool-args", "no-stub")
ETHERNET_IP_LOG_PATTERN = re.compile(r"Ethernet got IP: ip=(\d+\.\d+\.\d+\.\d+)")
# MOD-IO relay state cannot be read back from the board, so the firmware
# rejects per-relay writes with 409 MODIO_STATE_UNKNOWN until a full-mask
# write has established a known state (e.g. right after boot). Restore the
# MOD-IO relays with one full-mask write, which is valid in every sync state.
SAFE_OFF_REQUESTS = (
    ("/api/v1/relays/onboard/1", {"state": False}),
    ("/api/v1/relays/onboard/2", {"state": False}),
    ("/api/v1/relays/modio", {"states": [False, False, False, False]}),
)
IGNORED_CLEANUP_STATUS_CODES = {404, 405, 501, 503}
_USE_DEFAULT_TOKEN = object()


@dataclass(frozen=True)
class DutEndpoint:
    host: str
    ip: str
    port: int
    base_url: str


@dataclass
class IntegrationHttpClient:
    base_url: str
    session: requests.Session
    timeout: float

    def request(self, method: str, path: str, **kwargs: object) -> requests.Response:
        timeout = float(kwargs.pop("timeout", self.timeout))
        if not path.startswith("/"):
            raise ValueError(f"path must start with '/': {path!r}")
        return self.session.request(method, f"{self.base_url}{path}", timeout=timeout, **kwargs)


@dataclass(frozen=True)
class CLIRunResult:
    stdout: str
    stderr: str
    exit_code: int


@dataclass(frozen=True)
class CLIRobotRunResult:
    stdout: str
    stderr: str
    exit_code: int
    payload: dict[str, Any]


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("evb-relay")
    group.addoption("--monitor-port",
                    action="store",
                    dest="monitor_port",
                    default=None,
                    help="Runtime console port for serial endpoint fallback")
    group.addoption("--monitor-baud",
                    action="store",
                    dest="monitor_baud",
                    default=None,
                    help="Runtime console baud for serial endpoint fallback")


def _default_flash_port() -> str:
    return os.environ.get("EVB_FLASH_PORT") or os.environ.get("EVB_SERIAL_PORT") or DEFAULT_FLASH_PORT


def _default_flash_baud() -> str:
    return os.environ.get("EVB_FLASH_BAUD", DEFAULT_FLASH_BAUD)


def _default_monitor_port(flash_port: str) -> str:
    return os.environ.get("EVB_SERIAL_PORT") or flash_port


def _default_monitor_baud() -> str:
    return os.environ.get("EVB_SERIAL_BAUD", DEFAULT_MONITOR_BAUD)


def _flash_port_from_config(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("port") or _default_flash_port())


def _flash_baud_from_config(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("baud") or _default_flash_baud())


def _monitor_port_from_config(pytestconfig: pytest.Config, flash_port: str) -> str:
    return str(pytestconfig.getoption("monitor_port") or _default_monitor_port(flash_port))


def _monitor_baud_from_config(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("monitor_baud") or _default_monitor_baud())


def _run_command(args: list[str], cwd: Path | None = None) -> str:
    env = os.environ.copy()
    result = subprocess.run(args,
                            cwd=str(cwd) if cwd is not None else None,
                            env=env,
                            check=True,
                            text=True,
                            capture_output=True)
    return result.stdout


def _cli_env(config_home: Path, extra_env: Mapping[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env["XDG_CONFIG_HOME"] = str(config_home)
    env.pop("EVB_RELAY_HOST", None)
    env.pop("EVB_RELAY_API_TOKEN", None)
    env.pop("EVB_RELAY_ROBOT", None)
    env.pop("EVB_RELAY_TIMEOUT", None)
    if extra_env:
        env.update(extra_env)
    return env


def _parttool_command(
    parttool_py: Path,
    partition_table: Path,
    flash_port: str,
    flash_baud: str,
    *args: str,
) -> list[str]:
    return [
        sys.executable,
        str(parttool_py),
        "-f",
        str(partition_table),
        *PARTTOOL_ESPTOOL_ARGS,
        "-p",
        flash_port,
        "-b",
        flash_baud,
        *args,
    ]


def _read_device_config_values(repo_root: Path, flash_port: str, flash_baud: str) -> dict[str, str]:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError("IDF_PATH must be set for integration tests")

    parttool_py = Path(idf_path) / "components/partition_table/parttool.py"
    nvs_tool_py = Path(idf_path) / "components/nvs_flash/nvs_partition_tool/nvs_tool.py"
    partition_table = repo_root / "firmware/partitions.csv"

    with tempfile.TemporaryDirectory() as tempdir:
        nvs_bin = Path(tempdir) / "nvs.bin"
        _run_command(_parttool_command(
            parttool_py,
            partition_table,
            flash_port,
            flash_baud,
            "read_partition",
            "--partition-name",
            "nvs",
            "--output",
            str(nvs_bin),
        ))
        minimal_dump = _run_command([sys.executable, str(nvs_tool_py), "-d", "minimal", str(nvs_bin)])

    values: dict[str, str] = {}
    for raw_line in minimal_dump.splitlines():
        line = raw_line.replace("\x00", "").strip()
        if not line or line.startswith("Page "):
            continue

        if ":" not in line or " = " not in line:
            raise RuntimeError(f"unexpected nvs_tool output: {line!r}")

        namespaced_key, value = line.split(" = ", 1)
        namespace, key = namespaced_key.split(":", 1)
        if namespace == "device_cfg":
            values[key] = value

    return values


def _wait_for_host(host: str, timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    last_error: OSError | None = None

    while time.monotonic() < deadline:
        try:
            return socket.gethostbyname(host)
        except OSError as exc:
            last_error = exc
            time.sleep(1.0)

    raise RuntimeError(f"failed to resolve {host!r}: {last_error}")


def _wait_for_ip_on_serial(serial_port: str, serial_baud: str, timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    buffer = ""

    try:
        baudrate = int(serial_baud)
    except ValueError as exc:
        raise RuntimeError(f"invalid serial baud {serial_baud!r}") from exc

    try:
        with serial.Serial(serial_port, baudrate, timeout=0.25, dsrdtr=False, rtscts=False) as console:
            console.setDTR(False)
            console.setRTS(False)
            console.reset_input_buffer()

            # Reset back into the running app so the Ethernet DHCP log appears.
            console.setRTS(True)
            time.sleep(0.1)
            console.setRTS(False)

            while time.monotonic() < deadline:
                chunk = console.read(256)
                if not chunk:
                    continue

                buffer += chunk.decode("utf-8", errors="ignore")
                if len(buffer) > 8192:
                    buffer = buffer[-8192:]

                match = ETHERNET_IP_LOG_PATTERN.search(buffer)
                if match is not None:
                    return match.group(1)
    except serial.SerialException as exc:
        raise RuntimeError(f"failed to read serial boot log from {serial_port!r}: {exc}") from exc

    raise RuntimeError(f"failed to observe Ethernet DHCP lease on {serial_port!r}")


def _resolve_dut_endpoint(
    host: str,
    port: int,
    timeout_seconds: float,
    monitor_port: str | None = None,
    monitor_baud: str = DEFAULT_MONITOR_BAUD,
) -> DutEndpoint:
    try:
        ip = _wait_for_host(host, timeout_seconds)
    except RuntimeError as exc:
        if monitor_port is None:
            pytest.fail(f"could not resolve DUT host {host!r}: {exc}")

        try:
            ip = _wait_for_ip_on_serial(monitor_port,
                                        monitor_baud,
                                        max(timeout_seconds, DEFAULT_SERIAL_ENDPOINT_TIMEOUT))
        except RuntimeError as serial_exc:
            pytest.fail(
                f"could not resolve DUT host {host!r}: {exc}; "
                f"serial fallback on {monitor_port!r} also failed: {serial_exc}"
            )

    return DutEndpoint(host=host, ip=ip, port=port, base_url=f"http://{ip}:{port}")


def _restore_safe_relays(http_client: IntegrationHttpClient) -> None:
    for path, body in SAFE_OFF_REQUESTS:
        try:
            response = http_client.request("PUT", path, json=body)
        except requests.RequestException:
            continue

        if response.status_code in IGNORED_CLEANUP_STATUS_CODES:
            continue

        response.raise_for_status()


def _wait_for_http_ready(http_client: IntegrationHttpClient, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error: Exception | None = None
    last_status: int | None = None
    request_timeout = min(http_client.timeout, DEFAULT_HTTP_READY_REQUEST_TIMEOUT)

    while time.monotonic() < deadline:
        try:
            response = http_client.request("GET", "/api/v1/status", timeout=request_timeout)
        except (requests.ConnectionError, requests.Timeout) as exc:
            last_error = exc
        else:
            try:
                last_status = response.status_code
                if response.status_code == 200:
                    return
            finally:
                response.close()

        time.sleep(DEFAULT_HTTP_READY_POLL_SECONDS)

    if last_status is not None:
        raise RuntimeError(
            f"HTTP server at {http_client.base_url} did not become ready within "
            f"{timeout_seconds:.1f}s (last status {last_status})"
        )

    raise RuntimeError(
        f"HTTP server at {http_client.base_url} did not become ready within "
        f"{timeout_seconds:.1f}s (last error: {last_error})"
    )


@pytest.fixture(scope="session")
def flash_port(pytestconfig: pytest.Config) -> str:
    return _flash_port_from_config(pytestconfig)


@pytest.fixture(scope="session")
def flash_baud(pytestconfig: pytest.Config) -> str:
    return _flash_baud_from_config(pytestconfig)


@pytest.fixture(scope="session")
def monitor_port(pytestconfig: pytest.Config, flash_port: str) -> str:
    return _monitor_port_from_config(pytestconfig, flash_port)


@pytest.fixture(scope="session")
def monitor_baud(pytestconfig: pytest.Config) -> str:
    return _monitor_baud_from_config(pytestconfig)


@pytest.fixture(scope="session")
def firmware_flashed(flash_port: str, flash_baud: str) -> None:
    repo_root = _repo_root()
    _run_command([
        str(repo_root / "scripts/flash.sh"),
        "--port",
        flash_port,
        "--baud",
        flash_baud,
    ], cwd=repo_root)
    time.sleep(float(os.environ.get("EVB_BOOT_SETTLE_SECONDS", DEFAULT_BOOT_SETTLE_SECONDS)))


@pytest.fixture(scope="session")
def auth_token(flash_port: str, flash_baud: str, firmware_flashed: None) -> Iterator[str]:
    repo_root = _repo_root()
    previous_values = _read_device_config_values(repo_root, flash_port, flash_baud)
    previous_token = previous_values.get("api_token")
    token = secrets.token_hex(16)

    _run_command([
        str(repo_root / "scripts/provision.sh"),
        "--port",
        flash_port,
        "--baud",
        flash_baud,
        "--token",
        token,
    ], cwd=repo_root)
    time.sleep(float(os.environ.get("EVB_BOOT_SETTLE_SECONDS", DEFAULT_BOOT_SETTLE_SECONDS)))

    try:
        yield token
    finally:
        if previous_token:
            restore_args = [
                str(repo_root / "scripts/provision.sh"),
                "--port",
                flash_port,
                "--baud",
                flash_baud,
                "--token",
                previous_token,
            ]
        else:
            restore_args = [
                str(repo_root / "scripts/provision.sh"),
                "--port",
                flash_port,
                "--baud",
                flash_baud,
                "--clear",
            ]

        _run_command(restore_args, cwd=repo_root)
        time.sleep(float(os.environ.get("EVB_BOOT_SETTLE_SECONDS", DEFAULT_BOOT_SETTLE_SECONDS)))


@pytest.fixture(scope="session")
def dut_endpoint(auth_token: str, monitor_port: str, monitor_baud: str) -> DutEndpoint:
    host = os.environ.get("EVB_DUT_HOST") or os.environ.get("EVB_DUT_HOSTNAME", DEFAULT_DUT_HOST)
    port = int(os.environ.get("EVB_DUT_HTTP_PORT", DEFAULT_HTTP_PORT))
    timeout = float(os.environ.get("EVB_DISCOVERY_TIMEOUT_SECONDS", DEFAULT_DISCOVERY_TIMEOUT))
    return _resolve_dut_endpoint(host, port, timeout, monitor_port, monitor_baud)


@pytest.fixture(scope="session")
def http_client(auth_token: str, dut_endpoint: DutEndpoint) -> Iterator[IntegrationHttpClient]:
    session = requests.Session()
    session.headers.update({
        "Accept": "application/json",
        "Authorization": f"Bearer {auth_token}",
    })

    client = IntegrationHttpClient(
        base_url=dut_endpoint.base_url,
        session=session,
        timeout=float(os.environ.get("EVB_HTTP_TIMEOUT_SECONDS", DEFAULT_REQUEST_TIMEOUT)),
    )

    try:
        _wait_for_http_ready(
            client,
            float(os.environ.get("EVB_HTTP_READY_TIMEOUT_SECONDS", DEFAULT_HTTP_READY_TIMEOUT)),
        )
        yield client
    finally:
        session.close()


@pytest.fixture(scope="session")
def cli_binary(tmp_path_factory: pytest.TempPathFactory) -> Path:
    if shutil.which("go") is None:
        pytest.skip("Go toolchain is not available")

    repo_root = _repo_root()
    binary_path = tmp_path_factory.mktemp("cli-device") / "evb-relay"
    completed = subprocess.run(
        ["go", "build", "-o", str(binary_path), "."],
        cwd=repo_root / "cli",
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if completed.returncode != 0:
        pytest.fail(f"failed to build CLI binary:\n{completed.stderr}")
    return binary_path


@pytest.fixture(scope="session")
def cli_run(
    cli_binary: Path,
    dut_endpoint: DutEndpoint,
    auth_token: str,
    tmp_path_factory: pytest.TempPathFactory,
) -> Callable[..., CLIRunResult]:
    config_home = tmp_path_factory.mktemp("cli-config")
    default_host = f"{dut_endpoint.ip}:{dut_endpoint.port}"

    def run(
        *args: str,
        api_token: object = _USE_DEFAULT_TOKEN,
        host: str | None = None,
        extra_env: Mapping[str, str] | None = None,
        request_timeout: str = DEFAULT_CLI_REQUEST_TIMEOUT,
        timeout_seconds: float = DEFAULT_CLI_TIMEOUT_SECONDS,
    ) -> CLIRunResult:
        command = [
            str(cli_binary),
            "--host",
            host or default_host,
            "--timeout",
            request_timeout,
        ]
        if api_token is _USE_DEFAULT_TOKEN:
            command.extend(["--api-token", auth_token])
        elif api_token is not None:
            command.extend(["--api-token", str(api_token)])
        command.extend(args)

        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            env=_cli_env(config_home, extra_env),
        )
        return CLIRunResult(
            stdout=completed.stdout,
            stderr=completed.stderr,
            exit_code=completed.returncode,
        )

    return run


@pytest.fixture(scope="session")
def cli_robot_run(cli_run: Callable[..., CLIRunResult]) -> Callable[..., CLIRobotRunResult]:
    def run(
        *args: str,
        api_token: object = _USE_DEFAULT_TOKEN,
        host: str | None = None,
        extra_env: Mapping[str, str] | None = None,
        request_timeout: str = DEFAULT_CLI_REQUEST_TIMEOUT,
        timeout_seconds: float = DEFAULT_CLI_TIMEOUT_SECONDS,
    ) -> CLIRobotRunResult:
        result = cli_run(
            "--robot",
            "--format",
            "json",
            *args,
            api_token=api_token,
            host=host,
            extra_env=extra_env,
            request_timeout=request_timeout,
            timeout_seconds=timeout_seconds,
        )
        return CLIRobotRunResult(
            stdout=result.stdout,
            stderr=result.stderr,
            exit_code=result.exit_code,
            payload=json.loads(result.stdout),
        )

    return run


@pytest.fixture
def cleanup_actions() -> Iterator[list[Callable[[], None]]]:
    actions: list[Callable[[], None]] = []
    yield actions

    failures: list[str] = []
    while actions:
        action = actions.pop()
        try:
            action()
        except Exception as exc:  # pragma: no cover - exercised by future tests
            failures.append(str(exc))

    if failures:
        pytest.fail("cleanup callbacks failed:\n" + "\n".join(failures))


@pytest.fixture
def relay_cleanup(cleanup_actions: list[Callable[[], None]],
                  http_client: IntegrationHttpClient) -> None:
    cleanup_actions.append(lambda: _restore_safe_relays(http_client))
