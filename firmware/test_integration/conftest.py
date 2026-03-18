from __future__ import annotations

import os
import secrets
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator

import pytest
import requests

os.environ.setdefault("ESPBAUD", "115200")

DEFAULT_DUT_HOST = "esp32-evb-relay.local"
DEFAULT_HTTP_PORT = 80
DEFAULT_FLASH_PORT = "/dev/ttyS4"
DEFAULT_SERIAL_BAUD = "115200"
DEFAULT_REQUEST_TIMEOUT = 5.0
DEFAULT_DISCOVERY_TIMEOUT = 15.0
DEFAULT_BOOT_SETTLE_SECONDS = 3.0
PARTTOOL_ESPTOOL_ARGS = ("--esptool-args", "no-stub")
SAFE_OFF_PATHS = (
    "/api/v1/relays/onboard/1",
    "/api/v1/relays/onboard/2",
    "/api/v1/relays/modio/1",
    "/api/v1/relays/modio/2",
    "/api/v1/relays/modio/3",
    "/api/v1/relays/modio/4",
)
IGNORED_CLEANUP_STATUS_CODES = {404, 405, 501}


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


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _default_flash_port() -> str:
    return os.environ.get("EVB_FLASH_PORT") or os.environ.get("EVB_SERIAL_PORT") or DEFAULT_FLASH_PORT


def _run_command(args: list[str], cwd: Path | None = None) -> str:
    env = os.environ.copy()
    result = subprocess.run(args,
                            cwd=str(cwd) if cwd is not None else None,
                            env=env,
                            check=True,
                            text=True,
                            capture_output=True)
    return result.stdout


def _parttool_command(
    parttool_py: Path,
    partition_table: Path,
    serial_port: str,
    serial_baud: str,
    *args: str,
) -> list[str]:
    return [
        sys.executable,
        str(parttool_py),
        "-f",
        str(partition_table),
        *PARTTOOL_ESPTOOL_ARGS,
        "-p",
        serial_port,
        "-b",
        serial_baud,
        *args,
    ]


def _read_device_config_values(repo_root: Path, serial_port: str, serial_baud: str) -> dict[str, str]:
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
            serial_port,
            serial_baud,
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


def _resolve_dut_endpoint(host: str, port: int, timeout_seconds: float) -> DutEndpoint:
    try:
        ip = _wait_for_host(host, timeout_seconds)
    except RuntimeError as exc:
        pytest.fail(f"could not resolve DUT host {host!r}: {exc}")

    return DutEndpoint(host=host, ip=ip, port=port, base_url=f"http://{ip}:{port}")


def _restore_safe_relays(http_client: IntegrationHttpClient) -> None:
    for path in SAFE_OFF_PATHS:
        try:
            response = http_client.request("PUT", path, json={"state": False})
        except requests.RequestException:
            continue

        if response.status_code in IGNORED_CLEANUP_STATUS_CODES:
            continue

        response.raise_for_status()


@pytest.fixture(scope="session")
def serial_port(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("port") or _default_flash_port())


@pytest.fixture(scope="session")
def serial_baud(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("baud") or os.environ.get("EVB_FLASH_BAUD", DEFAULT_SERIAL_BAUD))


@pytest.fixture(scope="session")
def firmware_flashed(serial_port: str, serial_baud: str) -> None:
    repo_root = _repo_root()
    _run_command([
        str(repo_root / "scripts/flash.sh"),
        "--port",
        serial_port,
        "--baud",
        serial_baud,
    ], cwd=repo_root)
    time.sleep(float(os.environ.get("EVB_BOOT_SETTLE_SECONDS", DEFAULT_BOOT_SETTLE_SECONDS)))


@pytest.fixture(scope="session")
def auth_token(serial_port: str, serial_baud: str, firmware_flashed: None) -> Iterator[str]:
    repo_root = _repo_root()
    previous_values = _read_device_config_values(repo_root, serial_port, serial_baud)
    previous_token = previous_values.get("api_token")
    token = secrets.token_hex(16)

    _run_command([
        str(repo_root / "scripts/provision.sh"),
        "--port",
        serial_port,
        "--baud",
        serial_baud,
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
                serial_port,
                "--baud",
                serial_baud,
                "--token",
                previous_token,
            ]
        else:
            restore_args = [
                str(repo_root / "scripts/provision.sh"),
                "--port",
                serial_port,
                "--baud",
                serial_baud,
                "--clear",
            ]

        _run_command(restore_args, cwd=repo_root)
        time.sleep(float(os.environ.get("EVB_BOOT_SETTLE_SECONDS", DEFAULT_BOOT_SETTLE_SECONDS)))


@pytest.fixture(scope="session")
def dut_endpoint(auth_token: str) -> DutEndpoint:
    host = os.environ.get("EVB_DUT_HOST") or os.environ.get("EVB_DUT_HOSTNAME", DEFAULT_DUT_HOST)
    port = int(os.environ.get("EVB_DUT_HTTP_PORT", DEFAULT_HTTP_PORT))
    timeout = float(os.environ.get("EVB_DISCOVERY_TIMEOUT_SECONDS", DEFAULT_DISCOVERY_TIMEOUT))
    return _resolve_dut_endpoint(host, port, timeout)


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
        yield client
    finally:
        session.close()


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
