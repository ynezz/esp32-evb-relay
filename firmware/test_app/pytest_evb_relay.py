import logging
import os
import re
import time
import types

import pytest
import serial as pyserial
from pexpect.exceptions import TIMEOUT
from pytest_embedded import Dut
from pytest_embedded.unity import UNITY_SUMMARY_LINE_REGEX
from pytest_embedded.utils import remove_asci_color_code
from pytest_embedded_idf.unity_tester import (
    READY_PATTERN_LIST,
    UNITY_BASIC_REGEX,
    UNITY_FIXTURE_REGEX,
    _parse_unity_test_output,
)

os.environ.setdefault("ESPBAUD", "115200")

MENU_PARSE_PATTERN = r"Here's the test menu, pick your combo:(.+)Enter test for running."
MENU_PARSE_RETRIES = 5
READY_PATTERN_BYTES = [pattern.encode("utf-8") for pattern in READY_PATTERN_LIST]
MENU_END = b"Enter test for running."
CRASH_MARKER_PATTERNS = [
    re.compile(rb"\*\*\*ERROR\*\*\*"),
    re.compile(rb"Guru Meditation Error"),
    re.compile(rb"abort\(\) was called"),
    re.compile(rb"Backtrace:"),
    re.compile(rb"Rebooting\.\.\."),
]
CRASH_RECOVERY_TIMEOUT = 15
CASE_RESULT_GRACE_TIMEOUT = 0.5


def _load_unity_menu_with_retries(dut: Dut, retries: int = MENU_PARSE_RETRIES) -> None:
    last_error: Exception | None = None

    for attempt in range(1, retries + 1):
        try:
            if attempt == 1:
                menu = dut._parse_test_menu()
            else:
                response = dut.confirm_write("\n", expect_pattern=MENU_PARSE_PATTERN)
                menu = dut._parse_unity_menu_from_str(response.group(1).decode("utf-8"))

            expected_indices = list(range(1, len(menu) + 1))
            actual_indices = [case.index for case in menu]
            if actual_indices != expected_indices:
                raise ValueError(
                    f"Unity menu indices are not contiguous: expected {expected_indices}, got {actual_indices}"
                )

            dut._test_menu = menu
            dut._hard_reset()
            return
        except (NotImplementedError, TIMEOUT, ValueError) as exc:
            last_error = exc
            logging.warning("Unity menu parse attempt %d/%d failed: %s", attempt, retries, exc)

    assert last_error is not None
    raise last_error


def _install_fail_fast_case_analyzer(dut: Dut) -> None:
    def _analyze_test_case_result_fail_fast(
        self: Dut,
        case,
        pre_run_failure: Exception | None,
        *,
        start_time: float = 0,
        timeout: float = 30,
    ) -> None:
        if pre_run_failure is not None:
            self._add_test_case_to_suite(
                {
                    "name": case.name,
                    "result": "IGNORE",
                    "message": (
                        "Skipped due to a failure before test execution. "
                        f"The write command probably failed: {pre_run_failure}"
                    ),
                    "time": 0,
                    "app_path": self.app.app_path,
                }
            )
            return

        match = None
        log = ""
        remaining_timeout = timeout - (time.perf_counter() - start_time)
        if remaining_timeout < 0:
            remaining_timeout = 0

        try:
            match = self.expect(
                [UNITY_SUMMARY_LINE_REGEX, *CRASH_MARKER_PATTERNS, *READY_PATTERN_LIST],
                timeout=remaining_timeout,
            )
        except Exception:
            pass
        else:
            matched_output = match.group()
            matched_text = matched_output.decode("utf-8", errors="ignore")
            log = remove_asci_color_code(self.pexpect_proc.before + matched_output)

            if not UNITY_SUMMARY_LINE_REGEX.match(matched_output):
                consumed_ready_prompt = matched_text in READY_PATTERN_LIST
                message = (
                    f'Unexpected return to Unity prompt while "{case.name}" was active'
                    if consumed_ready_prompt
                    else f'Crash marker while "{case.name}" was active: {matched_text.strip()}'
                )

                if not consumed_ready_prompt:
                    try:
                        ready_match = self.expect(READY_PATTERN_LIST, timeout=CRASH_RECOVERY_TIMEOUT)
                    except Exception:
                        pass
                    else:
                        log += remove_asci_color_code(self.pexpect_proc.before + ready_match.group())
                        consumed_ready_prompt = True

                if consumed_ready_prompt:
                    self._ignore_first_ready_pattern = True

                attrs = _parse_unity_test_output(log, case.name, self.pexpect_proc.buffer_debug_str)
                attrs["message"] = message
                attrs.update(
                    {
                        "app_path": self.app.app_path,
                        "time": round(time.perf_counter() - start_time, 3),
                    }
                )
                self._add_test_case_to_suite(attrs)
                return

            log = remove_asci_color_code(self.pexpect_proc.before)

        attrs = _parse_unity_test_output(log, case.name, self.pexpect_proc.buffer_debug_str)
        attrs.update(
            {
                "app_path": self.app.app_path,
                "time": round(time.perf_counter() - start_time, 3),
            }
        )
        self._add_test_case_to_suite(attrs)

    dut._analyze_test_case_result = types.MethodType(_analyze_test_case_result_fail_fast, dut)


class _SerialCaseTimeout(Exception):
    def __init__(self, buffer: bytes) -> None:
        super().__init__(remove_asci_color_code(buffer))
        self.buffer = buffer


def _serial_hard_reset(ser) -> None:
    ser.setDTR(False)
    ser.setRTS(False)
    ser.reset_input_buffer()
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)


def _serial_read_until(ser, *, timeout: float, predicate, initial_buffer: bytes = b"") -> bytes:
    deadline = time.monotonic() + timeout
    buf = bytearray(initial_buffer)

    if predicate(bytes(buf)):
        return bytes(buf)

    while time.monotonic() < deadline:
        chunk = ser.read_all()
        if chunk:
            buf.extend(chunk)
            if predicate(bytes(buf)):
                return bytes(buf)
        else:
            time.sleep(0.01)

    raise _SerialCaseTimeout(bytes(buf))


def _extract_case_attrs(log: str, case_name: str) -> dict | None:
    for match in UNITY_FIXTURE_REGEX.finditer(log):
        attrs = {k: v for k, v in match.groupdict().items() if v is not None}
        if attrs.get("name") == case_name:
            return attrs

    for match in UNITY_BASIC_REGEX.finditer(log):
        attrs = {k: v for k, v in match.groupdict().items() if v is not None}
        if attrs.get("name") == case_name:
            return attrs

    return None


def _complete_lines(log: str) -> str:
    # Unity result lines arrive over UART in chunks. Only trust lines that
    # are newline-terminated, otherwise a partially received
    # "file:line:name:FAIL: message" line matches and truncates the message.
    return log[: log.rfind("\n") + 1]


def _case_result_received(buffer: bytes, case_name: str) -> bool:
    return _extract_case_attrs(_complete_lines(remove_asci_color_code(buffer)), case_name) is not None


def _prompt_seen(buffer: bytes) -> bool:
    return any(prompt in buffer for prompt in READY_PATTERN_BYTES)


def _case_input_ready(buffer: bytes) -> bool:
    return MENU_END in buffer or READY_PATTERN_BYTES[1] in buffer


def _recover_case_input_prompt(ser, *, timeout: float, initial_buffer: bytes = b"") -> bytes:
    if _case_input_ready(initial_buffer):
        return initial_buffer

    buffer = _serial_read_until(ser, timeout=timeout, predicate=_prompt_seen, initial_buffer=initial_buffer)
    if READY_PATTERN_BYTES[0] in buffer and not _case_input_ready(buffer):
        ser.write(b"\n")
        buffer += _serial_read_until(ser, timeout=timeout, predicate=_case_input_ready)

    return buffer


def _case_complete(buffer: bytes, case_name: str) -> bool:
    if any(pattern.search(buffer) for pattern in CRASH_MARKER_PATTERNS):
        return True

    if _case_result_received(buffer, case_name):
        return True

    return _prompt_seen(buffer)


def _collect_case_result_after_prompt(
    ser,
    *,
    case_name: str,
    initial_buffer: bytes,
    timeout: float = CASE_RESULT_GRACE_TIMEOUT,
) -> bytes:
    if _case_result_received(initial_buffer, case_name) or not _prompt_seen(initial_buffer):
        return initial_buffer

    try:
        return _serial_read_until(
            ser,
            timeout=timeout,
            predicate=lambda buffer, name=case_name: _case_result_received(buffer, name),
            initial_buffer=initial_buffer,
        )
    except _SerialCaseTimeout as exc:
        return exc.buffer


def _format_case_failure_report(case_index: int, case_name: str, attrs: dict, log: str) -> str:
    location = ""
    if attrs.get("file") and attrs.get("line"):
        location = f"{attrs['file']}:{attrs['line']}"

    lines = [
        f"FAILURE {case_index}: {case_name}",
        f"  result:   {attrs.get('result', 'UNKNOWN')}",
        f"  location: {location or 'unknown'}",
        f"  message:  {(attrs.get('message') or 'none').strip()}",
        f"----- device log for case {case_index} -----",
        log.rstrip("\r\n"),
        f"----- end device log for case {case_index} -----",
    ]
    return "\n".join(lines)


def _open_case_runner_serial(dut: Dut):
    return pyserial.serial_for_url(
        dut.serial.port,
        baudrate=dut.serial.baud,
        bytesize=pyserial.EIGHTBITS,
        parity=pyserial.PARITY_NONE,
        stopbits=pyserial.STOPBITS_ONE,
        timeout=0.05,
        xonxoff=False,
        rtscts=False,
    )


def _run_all_cases_via_serial(dut: Dut, *, timeout: float, open_serial_port=_open_case_runner_serial) -> None:
    # Fully detach pytest-embedded from the UART before the owned runner
    # takes over so late-suite reads cannot race against the shared serial.
    dut.serial.close()

    with open_serial_port(dut) as ser:

        _serial_hard_reset(ser)
        _serial_read_until(ser, timeout=20, predicate=lambda buffer: READY_PATTERN_BYTES[0] in buffer)
        ser.write(b"\n")
        _serial_read_until(ser, timeout=10, predicate=lambda buffer: MENU_END in buffer)

        for case in dut.test_menu:
            start_time = time.perf_counter()
            print(f"START {case.index}: {case.name}", flush=True)
            ser.reset_input_buffer()
            ser.write(f"{case.index}\n".encode("utf-8"))
            start_marker = f"Running {case.name}...".encode("utf-8")

            try:
                started = _serial_read_until(
                    ser,
                    timeout=10.0,
                    predicate=lambda buffer, marker=start_marker: marker in buffer,
                )
                started = started[started.find(start_marker):]
                raw = _serial_read_until(
                    ser,
                    timeout=timeout + 10.0,
                    predicate=lambda buffer, case_name=case.name: _case_complete(buffer, case_name),
                    initial_buffer=started,
                )
            except _SerialCaseTimeout as exc:
                raw = exc.buffer

            raw = _collect_case_result_after_prompt(ser, case_name=case.name, initial_buffer=raw)
            log = remove_asci_color_code(raw)
            attrs = _extract_case_attrs(log, case.name)
            if attrs is None:
                attrs = _parse_unity_test_output(log, case.name, log[-2000:])
            attrs.update(
                {
                    "app_path": dut.app.app_path,
                    "time": round(time.perf_counter() - start_time, 3),
                }
            )
            dut._add_test_case_to_suite(attrs)
            if attrs.get("result") != "PASS":
                # The JUnit report alone is not surfaced by `just test-device`;
                # print the Unity assertion and the case's full UART log so a
                # device failure can be diagnosed from the captured output.
                print(_format_case_failure_report(case.index, case.name, attrs, log), flush=True)
            try:
                _recover_case_input_prompt(ser, timeout=20.0, initial_buffer=raw)
            except _SerialCaseTimeout:
                pass
            print(f"END {case.index}: {case.name} -> {attrs.get('result', 'UNKNOWN')}", flush=True)


@pytest.mark.esp32
@pytest.mark.generic
def test_all_cases(dut: Dut) -> None:
    _load_unity_menu_with_retries(dut)
    _run_all_cases_via_serial(dut, timeout=120)
