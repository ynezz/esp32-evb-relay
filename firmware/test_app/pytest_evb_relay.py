import os

import pytest
from pytest_embedded import Dut

os.environ.setdefault("ESPBAUD", "115200")


@pytest.mark.esp32
@pytest.mark.generic
def test_all_cases(dut: Dut) -> None:
    dut.run_all_single_board_cases(timeout=120)
