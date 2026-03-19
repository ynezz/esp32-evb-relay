from types import SimpleNamespace

from pytest_evb_relay import (
    CRASH_RECOVERY_TIMEOUT,
    _case_complete,
    _install_fail_fast_case_analyzer,
    _recover_case_input_prompt,
    _serial_read_until,
)


class _FakeMatch:
    def __init__(self, data: bytes) -> None:
        self._data = data

    def group(self) -> bytes:
        return self._data


class _FakeDut:
    def __init__(self, responses: list[SimpleNamespace]) -> None:
        self._responses = list(responses)
        self.expect_calls: list[tuple[object, float]] = []
        self.recorded_cases: list[dict[str, object]] = []
        self._ignore_first_ready_pattern = False
        self.app = SimpleNamespace(app_path="/tmp/fake-app")
        self.pexpect_proc = SimpleNamespace(before=b"", buffer_debug_str="fake buffer")

    def expect(self, pattern, timeout: float):
        self.expect_calls.append((pattern, timeout))
        response = self._responses.pop(0)
        self.pexpect_proc.before = response.before
        return _FakeMatch(response.match)

    def _add_test_case_to_suite(self, attrs: dict[str, object]) -> None:
        self.recorded_cases.append(attrs)


class _FakeSerial:
    def __init__(self, chunks: list[bytes] | None = None) -> None:
        self._chunks = list(chunks or [])
        self.writes: list[bytes] = []

    def read_all(self) -> bytes:
        if self._chunks:
            return self._chunks.pop(0)
        return b""

    def write(self, data: bytes) -> None:
        self.writes.append(data)


def test_serial_read_until_accepts_complete_initial_buffer() -> None:
    serial = _FakeSerial()

    result = _serial_read_until(
        serial,
        timeout=0.01,
        predicate=lambda buffer: b"ready" in buffer,
        initial_buffer=b"already ready",
    )

    assert result == b"already ready"


def test_case_complete_stops_on_ready_prompt_without_unity_result() -> None:
    assert _case_complete(
        b"Running ota upload case...\r\nEnter next test, or 'enter' to see menu\r\n",
        "rest_api device accepts OTA uploads and switches the boot partition",
    )


def test_recover_case_input_prompt_reopens_menu_after_boot_prompt() -> None:
    serial = _FakeSerial(
        [
            b"Here's the test menu, pick your combo:\r\n(1)\t\"foo\"\r\nEnter test for running.\r\n",
        ]
    )

    buffer = _recover_case_input_prompt(
        serial,
        timeout=0.01,
        initial_buffer=b"Press ENTER to see the list of tests\r\n",
    )

    assert serial.writes == [b"\n"]
    assert b"Enter test for running." in buffer


def test_fail_fast_analyzer_records_crash_and_consumed_menu() -> None:
    dut = _FakeDut(
        [
            SimpleNamespace(
                before=b"Running relay route case...\r\n",
                match=b"***ERROR***",
            ),
            SimpleNamespace(
                before=b"Backtrace: 0x40081b21\r\nRebooting...\r\n",
                match=b"Press ENTER to see the list of tests",
            ),
        ]
    )
    case = SimpleNamespace(name="rest_api device reports absent MOD-IO on relay routes")

    _install_fail_fast_case_analyzer(dut)
    dut._analyze_test_case_result(case, None, start_time=0.0, timeout=120)

    assert len(dut.recorded_cases) == 1
    attrs = dut.recorded_cases[0]
    assert attrs["name"] == case.name
    assert attrs["result"] == "FAIL"
    assert 'Crash marker while "rest_api device reports absent MOD-IO on relay routes" was active' in attrs["message"]
    assert "***ERROR***" in attrs["stdout"]
    assert "Backtrace:" in attrs["stdout"]
    assert "Press ENTER to see the list of tests" in attrs["stdout"]
    assert dut._ignore_first_ready_pattern is True
    assert len(dut.expect_calls) == 2
    assert dut.expect_calls[1][1] == CRASH_RECOVERY_TIMEOUT


def test_fail_fast_analyzer_records_unexpected_menu_return() -> None:
    dut = _FakeDut(
        [
            SimpleNamespace(
                before=b"Running ota upload case...\r\n",
                match=b"Enter next test, or 'enter' to see menu",
            )
        ]
    )
    case = SimpleNamespace(name="rest_api device accepts OTA uploads and switches the boot partition")

    _install_fail_fast_case_analyzer(dut)
    dut._analyze_test_case_result(case, None, start_time=0.0, timeout=120)

    assert len(dut.recorded_cases) == 1
    attrs = dut.recorded_cases[0]
    assert attrs["name"] == case.name
    assert attrs["result"] == "FAIL"
    assert attrs["message"] == 'Unexpected return to Unity prompt while "rest_api device accepts OTA uploads and switches the boot partition" was active'
    assert "Enter next test, or 'enter' to see menu" in attrs["stdout"]
    assert dut._ignore_first_ready_pattern is True
    assert len(dut.expect_calls) == 1
