import logging
import os
import re
import time
import types

import pytest
from pexpect.exceptions import TIMEOUT
from pytest_embedded import Dut
from pytest_embedded.unity import UNITY_SUMMARY_LINE_REGEX
from pytest_embedded.utils import remove_asci_color_code
from pytest_embedded_idf.unity_tester import READY_PATTERN_LIST, _parse_unity_test_output

os.environ.setdefault("ESPBAUD", "115200")

MENU_PARSE_PATTERN = r"Here's the test menu, pick your combo:(.+)Enter test for running."
MENU_PARSE_RETRIES = 5
CRASH_MARKER_PATTERNS = [
    re.compile(rb"\*\*\*ERROR\*\*\*"),
    re.compile(rb"Guru Meditation Error"),
    re.compile(rb"abort\(\) was called"),
    re.compile(rb"Backtrace:"),
    re.compile(rb"Rebooting\.\.\."),
]
CRASH_RECOVERY_TIMEOUT = 15


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


@pytest.mark.esp32
@pytest.mark.generic
def test_all_cases(dut: Dut) -> None:
    _load_unity_menu_with_retries(dut)
    _install_fail_fast_case_analyzer(dut)
    dut.run_all_single_board_cases(timeout=120)
