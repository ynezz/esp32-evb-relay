from __future__ import annotations

import requests
import pytest


def _assert_device_context_headers(response: requests.Response) -> None:
    assert response.headers["X-FW-Version"]
    assert response.headers["X-ModIO-Present"] in {"true", "false"}
    assert response.headers["X-ModIO-Sync"] in {"absent", "synchronized"}


def _assert_no_device_context_headers(response: requests.Response) -> None:
    assert "X-FW-Version" not in response.headers
    assert "X-ModIO-Present" not in response.headers
    assert "X-ModIO-Sync" not in response.headers


@pytest.mark.esp32
@pytest.mark.integration
def test_status_endpoint_returns_expected_schema(http_client) -> None:
    response = http_client.request("GET", "/api/v1/status")

    assert response.status_code == 200
    _assert_device_context_headers(response)

    payload = response.json()
    assert isinstance(payload["uptime_seconds"], (int, float))
    assert payload["uptime_seconds"] >= 0
    assert isinstance(payload["firmware_version"], str)
    assert payload["firmware_version"] == response.headers["X-FW-Version"]
    assert isinstance(payload["free_heap_bytes"], (int, float))
    assert payload["free_heap_bytes"] > 0

    network = payload["network"]
    assert isinstance(network["hostname"], str)
    assert isinstance(network["connected"], bool)
    assert isinstance(network["ip"], str)
    assert isinstance(network["netmask"], str)
    assert isinstance(network["gateway"], str)

    modio = payload["modio"]
    assert isinstance(modio["present"], bool)
    assert modio["present"] == (response.headers["X-ModIO-Present"] == "true")
    assert modio["sync"] == response.headers["X-ModIO-Sync"]


@pytest.mark.esp32
@pytest.mark.integration
def test_onboard_relay_control_round_trips_through_http(http_client, relay_cleanup) -> None:
    list_response = http_client.request("GET", "/api/v1/relays/onboard")

    assert list_response.status_code == 200
    _assert_device_context_headers(list_response)
    initial_relays = list_response.json()["relays"]
    assert [relay["id"] for relay in initial_relays] == [1, 2]
    assert {relay["group"] for relay in initial_relays} == {"onboard"}

    set_response = http_client.request("PUT", "/api/v1/relays/onboard/1", json={"state": True})
    assert set_response.status_code == 200
    _assert_device_context_headers(set_response)
    assert set_response.json()["relay"] == {"group": "onboard", "id": 1, "state": True}

    after_set_response = http_client.request("GET", "/api/v1/relays/onboard")
    assert after_set_response.status_code == 200
    states_after_set = {relay["id"]: relay["state"] for relay in after_set_response.json()["relays"]}
    assert states_after_set[1] is True
    assert states_after_set[2] is False

    toggle_response = http_client.request("POST", "/api/v1/relays/onboard/1/toggle")
    assert toggle_response.status_code == 200
    _assert_device_context_headers(toggle_response)
    assert toggle_response.json()["relay"] == {"group": "onboard", "id": 1, "state": False}

    after_toggle_response = http_client.request("GET", "/api/v1/relays/onboard")
    assert after_toggle_response.status_code == 200
    states_after_toggle = {relay["id"]: relay["state"] for relay in after_toggle_response.json()["relays"]}
    assert states_after_toggle[1] is False


@pytest.mark.esp32
@pytest.mark.integration
def test_auth_enforcement_hides_device_context_until_authenticated(dut_endpoint, http_client) -> None:
    base_url = dut_endpoint.base_url
    unauthenticated_session = requests.Session()
    unauthenticated_session.headers["Accept"] = "application/json"

    try:
        missing_token = unauthenticated_session.get(
            f"{base_url}/api/v1/status",
            timeout=http_client.timeout,
        )
        assert missing_token.status_code == 401
        _assert_no_device_context_headers(missing_token)
        assert missing_token.json()["error"]["code"] == "AUTH_REQUIRED"

        wrong_token = unauthenticated_session.get(
            f"{base_url}/api/v1/status",
            timeout=http_client.timeout,
            headers={"Authorization": "Bearer wrong-secret"},
        )
        assert wrong_token.status_code in {401, 403}
        _assert_no_device_context_headers(wrong_token)
        assert wrong_token.json()["error"]["code"] in {"AUTH_REQUIRED", "AUTH_FORBIDDEN"}
    finally:
        unauthenticated_session.close()

    authenticated = http_client.request("GET", "/api/v1/status")
    assert authenticated.status_code == 200
    _assert_device_context_headers(authenticated)


@pytest.mark.esp32
@pytest.mark.integration
def test_authenticated_application_errors_keep_device_context_headers(http_client, relay_cleanup) -> None:
    response = http_client.request("PUT", "/api/v1/relays/onboard/9", json={"state": True})

    assert response.status_code == 404
    _assert_device_context_headers(response)
    error = response.json()["error"]
    assert error["code"] == "RELAY_NOT_FOUND"
    assert error["status"] == 404
