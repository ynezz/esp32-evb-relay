from __future__ import annotations

from typing import Any

import pytest


pytestmark = [
    pytest.mark.esp32,
    pytest.mark.integration,
    pytest.mark.cli_e2e,
]


def _assert_success(result: Any, command: str) -> dict[str, Any]:
    assert result.stderr == ""
    assert result.exit_code == 0
    assert result.payload["command"] == command
    assert result.payload["exit_code"] == 0
    assert result.payload.get("error") is None
    data = result.payload["data"]
    assert isinstance(data, dict)
    device_context = result.payload.get("device_context")
    assert isinstance(device_context, dict)
    assert isinstance(device_context["firmware_version"], str)
    assert device_context["firmware_version"]
    assert device_context["modio_sync"] in {"absent", "synchronized"}
    assert device_context["modio_present"] in {True, False}
    return data


def _relay_state_by_target(relays: list[dict[str, Any]]) -> dict[tuple[str, int], bool]:
    return {(str(relay["group"]), int(relay["id"])): bool(relay["state"]) for relay in relays}


def _status_payload(data: dict[str, Any]) -> dict[str, Any]:
    status = data["status"]
    assert isinstance(status, dict)
    return status


def _restore_onboard_relays(http_client) -> None:
    for path in ("/api/v1/relays/onboard/1", "/api/v1/relays/onboard/2"):
        response = http_client.request("PUT", path, json={"state": False})
        response.raise_for_status()


@pytest.fixture(scope="session")
def require_modio(cli_robot_run) -> dict[str, Any]:
    data = _assert_success(cli_robot_run("status"), "status")
    status = _status_payload(data)
    if not status["modio"]["present"]:
        pytest.skip("MOD-IO is not present on this device")
    return status


@pytest.fixture
def onboard_relay_cleanup(cleanup_actions, http_client) -> None:
    cleanup_actions.append(lambda: _restore_onboard_relays(http_client))


def test_cli_status_round_trip(cli_robot_run, dut_endpoint) -> None:
    data = _assert_success(cli_robot_run("status"), "status")
    status = _status_payload(data)

    assert float(status["uptime_seconds"]) >= 0
    assert isinstance(status["firmware_version"], str)
    assert status["firmware_version"]
    assert float(status["free_heap_bytes"]) > 0
    assert status["network"]["connected"] is True
    assert status["network"]["ip"] == dut_endpoint.ip
    assert status["network"]["hostname"]
    assert status["modio"]["sync"] in {"absent", "synchronized"}


def test_cli_relay_list_round_trip(cli_robot_run) -> None:
    data = _assert_success(cli_robot_run("relay", "list"), "relay list")
    relays = data["relays"]
    assert isinstance(relays, list)

    states = _relay_state_by_target(relays)
    assert ("onboard", 1) in states
    assert ("onboard", 2) in states

    modio_present = bool(data["modio_present"])
    if modio_present:
        for relay_id in range(1, 5):
            assert ("modio", relay_id) in states
    else:
        assert all(group != "modio" for group, _ in states)


def test_cli_onboard_relay_on_off_round_trip(cli_robot_run, onboard_relay_cleanup) -> None:
    on_data = _assert_success(cli_robot_run("relay", "on", "onboard:1"), "relay on")
    assert on_data["relay"] == {"group": "onboard", "id": 1, "state": True, "sync": None}

    list_data = _assert_success(cli_robot_run("relay", "list"), "relay list")
    assert _relay_state_by_target(list_data["relays"])[("onboard", 1)] is True

    off_data = _assert_success(cli_robot_run("relay", "off", "onboard:1"), "relay off")
    assert off_data["relay"] == {"group": "onboard", "id": 1, "state": False, "sync": None}

    list_data = _assert_success(cli_robot_run("relay", "list"), "relay list")
    assert _relay_state_by_target(list_data["relays"])[("onboard", 1)] is False


def test_cli_relay_toggle_round_trip(cli_robot_run, onboard_relay_cleanup) -> None:
    initial_data = _assert_success(cli_robot_run("relay", "list"), "relay list")
    initial_states = _relay_state_by_target(initial_data["relays"])
    initial_state = initial_states[("onboard", 2)]

    toggle_data = _assert_success(cli_robot_run("relay", "toggle", "onboard:2"), "relay toggle")
    assert toggle_data["relay"]["group"] == "onboard"
    assert toggle_data["relay"]["id"] == 2
    assert toggle_data["relay"]["state"] is (not initial_state)

    restore_data = _assert_success(cli_robot_run("relay", "toggle", "onboard:2"), "relay toggle")
    assert restore_data["relay"]["state"] is initial_state


def test_cli_relay_set_batch_round_trip(cli_robot_run, onboard_relay_cleanup) -> None:
    data = _assert_success(
        cli_robot_run("relay", "set", "onboard:1=on", "onboard:2=off"),
        "relay set",
    )
    assert data["all_ok"] is True
    results = data["results"]
    assert isinstance(results, list)
    assert len(results) == 2
    expected = {
        "onboard:1": True,
        "onboard:2": False,
    }
    for item in results:
        assert item["target"] in expected
        assert item["group"] == "onboard"
        assert item["requested"] is expected[item["target"]]
        assert item["state"] is expected[item["target"]]
        assert item["ok"] is True
        assert item.get("error") is None


@pytest.mark.modio
def test_cli_input_digital_round_trip(cli_robot_run, require_modio) -> None:
    data = _assert_success(cli_robot_run("input", "digital"), "input digital")
    assert int(data["sample_ts_ms"]) >= 0
    assert int(data["sample_age_ms"]) >= 0
    assert int(data["poll_interval_ms"]) > 0
    inputs = data["inputs"]
    assert isinstance(inputs, list)
    assert len(inputs) == 4
    assert [int(item["id"]) for item in inputs] == [1, 2, 3, 4]

    single = _assert_success(cli_robot_run("input", "digital", "1"), "input digital")
    assert single["input"]["id"] == 1
    assert isinstance(single["input"]["state"], bool)


@pytest.mark.modio
def test_cli_input_analog_round_trip(cli_robot_run, require_modio) -> None:
    data = _assert_success(cli_robot_run("input", "analog"), "input analog")
    assert int(data["sample_ts_ms"]) >= 0
    assert int(data["sample_age_ms"]) >= 0
    assert int(data["poll_interval_ms"]) > 0
    inputs = data["inputs"]
    assert isinstance(inputs, list)
    assert len(inputs) == 4
    assert [int(item["id"]) for item in inputs] == [1, 2, 3, 4]
    assert all(int(item["value"]) >= 0 for item in inputs)

    single = _assert_success(cli_robot_run("input", "analog", "3"), "input analog")
    assert single["input"]["id"] == 3
    assert int(single["input"]["value"]) >= 0


def test_cli_auth_enforcement(cli_robot_run) -> None:
    missing = cli_robot_run("status", api_token=None)
    assert missing.stderr == ""
    assert missing.exit_code == 3
    assert missing.payload["exit_code"] == 3
    assert missing.payload["error"]["code"] == "AUTH_REQUIRED"
    assert missing.payload.get("device_context") is None

    wrong = cli_robot_run("status", api_token="wrong-secret")
    assert wrong.stderr == ""
    assert wrong.exit_code == 3
    assert wrong.payload["exit_code"] == 3
    assert wrong.payload["error"]["code"] in {"AUTH_FORBIDDEN", "AUTH_REQUIRED"}
    assert wrong.payload.get("device_context") is None
