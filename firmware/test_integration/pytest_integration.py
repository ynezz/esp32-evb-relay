import pytest


@pytest.mark.esp32
@pytest.mark.integration
def test_integration_fixture_scaffold(auth_token: str, http_client, relay_cleanup) -> None:
    assert len(auth_token) == 32
    assert http_client.base_url.startswith("http://")
    assert http_client.session.headers["Authorization"] == f"Bearer {auth_token}"
    assert http_client.timeout > 0
