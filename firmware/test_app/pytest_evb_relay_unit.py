from types import SimpleNamespace

from pytest_evb_relay import (
    CRASH_RECOVERY_TIMEOUT,
    _case_complete,
    _collect_case_result_after_prompt,
    _install_fail_fast_case_analyzer,
    _recover_case_input_prompt,
    _run_all_cases_via_serial,
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
        self.dtr_values: list[bool] = []
        self.rts_values: list[bool] = []
        self.reset_input_buffer_calls = 0

    def read_all(self) -> bytes:
        if self._chunks:
            return self._chunks.pop(0)
        return b""

    def write(self, data: bytes) -> None:
        self.writes.append(data)

    def setDTR(self, value: bool) -> None:
        self.dtr_values.append(value)

    def setRTS(self, value: bool) -> None:
        self.rts_values.append(value)

    def reset_input_buffer(self) -> None:
        self.reset_input_buffer_calls += 1

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        return None


class _FakeSerialManager:
    def __init__(self, proc: object) -> None:
        self.proc = proc
        self.port = "/dev/fake-esp32"
        self.baud = 115200
        self.close_calls = 0

    class _DisableRedirectThread:
        def __enter__(self):
            return True

        def __exit__(self, exc_type, exc, tb) -> None:
            return None

    def disable_redirect_thread(self):
        return self._DisableRedirectThread()

    def close(self) -> None:
        self.close_calls += 1


class _FakeRunnerDut:
    def __init__(self) -> None:
        self.serial = _FakeSerialManager(object())
        self.test_menu = [SimpleNamespace(index=1, name="foo")]
        self.recorded_cases: list[dict[str, object]] = []
        self.app = SimpleNamespace(app_path="/tmp/fake-app")

    def _add_test_case_to_suite(self, attrs: dict[str, object]) -> None:
        self.recorded_cases.append(attrs)


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


def test_collect_case_result_after_prompt_reads_trailing_unity_result() -> None:
    serial = _FakeSerial(
        [
            b"./main/test_rest_api_device.c:1497:"
            b"rest_api device fails closed when no status provider is configured:PASS\r\n"
        ]
    )

    buffer = _collect_case_result_after_prompt(
        serial,
        case_name="rest_api device fails closed when no status provider is configured",
        initial_buffer=(
            b"Running rest_api device fails closed when no status provider is configured...\r\n"
            b"Enter next test, or 'enter' to see menu\r\n"
        ),
        timeout=0.01,
    )

    assert b":PASS" in buffer


def test_collect_case_result_after_prompt_keeps_initial_buffer_without_more_output() -> None:
    serial = _FakeSerial()
    initial = (
        b"Running rest_api device fails closed when no status provider is configured...\r\n"
        b"Enter next test, or 'enter' to see menu\r\n"
    )

    buffer = _collect_case_result_after_prompt(
        serial,
        case_name="rest_api device fails closed when no status provider is configured",
        initial_buffer=initial,
        timeout=0.01,
    )

    assert buffer == initial


def test_run_all_cases_via_serial_uses_owned_serial_port() -> None:
    dut = _FakeRunnerDut()
    owned_serial = _FakeSerial(
        [
            b"Press ENTER to see the list of tests\r\n",
            b"Here's the test menu, pick your combo:\r\n(1)\t\"foo\"\r\nEnter test for running.\r\n",
            b"Running foo...\r\n./main/test_rest_api_device.c:1:foo:PASS\r\n",
            b"Enter next test, or 'enter' to see menu\r\n",
        ]
    )

    _run_all_cases_via_serial(dut, timeout=0.01, open_serial_port=lambda _dut: owned_serial)

    assert len(dut.recorded_cases) == 1
    assert dut.recorded_cases[0]["name"] == "foo"
    assert dut.recorded_cases[0]["result"] == "PASS"
    assert owned_serial.writes == [b"\n", b"1\n"]
    assert dut.serial.close_calls == 1


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
