import logging
import os

import pytest
from pexpect.exceptions import TIMEOUT
from pytest_embedded import Dut

os.environ.setdefault("ESPBAUD", "115200")

MENU_PARSE_PATTERN = r"Here's the test menu, pick your combo:(.+)Enter test for running."
MENU_PARSE_RETRIES = 5


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


@pytest.mark.esp32
@pytest.mark.generic
def test_all_cases(dut: Dut) -> None:
    _load_unity_menu_with_retries(dut)
    dut.run_all_single_board_cases(timeout=120)
