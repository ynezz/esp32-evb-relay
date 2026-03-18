package robot

import (
	"bytes"
	"encoding/json"
	"errors"
	"net/url"
	"strings"
	"testing"
	"time"

	"example.com/esp32-evb-relay/cli/client"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
)

func TestWrapJSONSuccessEnvelope(t *testing.T) {
	t.Parallel()

	modioPresent := true
	deviceContext := &DeviceContext{
		ModIOPresent:    &modioPresent,
		ModIOSync:       "synchronized",
		FirmwareVersion: "0.3.1",
	}

	timestamp := time.Date(2026, time.March, 16, 14, 22, 3, 412000000, time.UTC)
	data := map[string]any{
		"relays": []map[string]any{
			{
				"group": "modio",
				"id":    3,
				"state": true,
				"sync":  "synchronized",
			},
		},
	}

	var buffer bytes.Buffer
	err := Wrap(nil, &buffer, WrapOpts{
		Command:       "relay list",
		Data:          data,
		DeviceContext: deviceContext,
		Format:        outputformat.JSON,
		Host:          "192.168.1.50",
		Timestamp:     timestamp,
		Elapsed:       42 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("Wrap() error = %v", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(buffer.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["v"]; got != float64(1) {
		t.Fatalf("v = %#v, want 1", got)
	}
	if got := payload["command"]; got != "relay list" {
		t.Fatalf("command = %#v, want %q", got, "relay list")
	}
	if got := payload["timestamp"]; got != "2026-03-16T14:22:03.412Z" {
		t.Fatalf("timestamp = %#v, want %q", got, "2026-03-16T14:22:03.412Z")
	}
	if got := payload["elapsed_ms"]; got != float64(42) {
		t.Fatalf("elapsed_ms = %#v, want 42", got)
	}
	if got := payload["exit_code"]; got != float64(0) {
		t.Fatalf("exit_code = %#v, want 0", got)
	}
	if got := payload["host"]; got != "192.168.1.50" {
		t.Fatalf("host = %#v, want %q", got, "192.168.1.50")
	}

	deviceContextJSON, ok := payload["device_context"].(map[string]any)
	if !ok {
		t.Fatalf("device_context = %#v; want object", payload["device_context"])
	}
	if got := deviceContextJSON["modio_present"]; got != true {
		t.Fatalf("modio_present = %#v, want true", got)
	}
	if got := deviceContextJSON["modio_sync"]; got != "synchronized" {
		t.Fatalf("modio_sync = %#v, want %q", got, "synchronized")
	}
	if got := deviceContextJSON["firmware_version"]; got != "0.3.1" {
		t.Fatalf("firmware_version = %#v, want %q", got, "0.3.1")
	}

	if _, exists := payload["error"]; exists {
		t.Fatalf("unexpected error envelope in success payload: %#v", payload["error"])
	}
}

func TestWrapTOONErrorEnvelopeReturnsSilentExitError(t *testing.T) {
	t.Parallel()

	modioPresent := false
	deviceContext := &DeviceContext{
		ModIOPresent:    &modioPresent,
		ModIOSync:       "absent",
		FirmwareVersion: "0.3.1",
	}

	var buffer bytes.Buffer
	err := Wrap(nil, &buffer, WrapOpts{
		Command:       "relay on modio:3",
		DeviceContext: deviceContext,
		Err: &client.APIError{
			Code:    "MODIO_NOT_PRESENT",
			Message: "MOD-IO board is not present.",
			Status:  503,
		},
		Format:    outputformat.TOON,
		Host:      "192.168.1.50",
		Timestamp: time.Date(2026, time.March, 16, 14, 22, 3, 464000000, time.UTC),
		Elapsed:   52 * time.Millisecond,
	})
	if err == nil {
		t.Fatal("Wrap() succeeded; want error")
	}
	if err.Error() != "" {
		t.Fatalf("Wrap() error string = %q, want empty", err.Error())
	}
	if got := exitcodes.FromError(err); got != exitcodes.HardwareUnavailable {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.HardwareUnavailable)
	}

	expected := strings.Join([]string{
		"v: 1",
		"command: \"relay on modio:3\"",
		"timestamp: \"2026-03-16T14:22:03.464Z\"",
		"elapsed_ms: 52",
		"exit_code: 7",
		"host: 192.168.1.50",
		"device_context:",
		"  modio_present: false",
		"  modio_sync: absent",
		"  firmware_version: 0.3.1",
		"error:",
		"  code: MODIO_NOT_PRESENT",
		"  message: MOD-IO board is not present.",
		"  http_status: 503",
		"  retryable: false",
		"  remediation: null",
	}, "\n")

	if got := buffer.String(); got != expected {
		t.Fatalf("TOON envelope = %q, want %q", got, expected)
	}
}

func TestBuildErrorDetailsMapsAuthAndNetworkRemediation(t *testing.T) {
	t.Parallel()

	authDetails, authCode, authNext := buildErrorDetails(&client.APIError{
		Code:    "AUTH_INVALID",
		Message: "provided token is invalid",
		Status:  403,
	})
	if authCode != exitcodes.AuthError {
		t.Fatalf("auth exit code = %d, want %d", authCode, exitcodes.AuthError)
	}
	if authDetails.Code != "AUTH_INVALID" {
		t.Fatalf("auth code = %q, want %q", authDetails.Code, "AUTH_INVALID")
	}
	if authDetails.Remediation == nil || *authDetails.Remediation != authRemediation {
		t.Fatalf("auth remediation = %#v, want %q", authDetails.Remediation, authRemediation)
	}
	if len(authNext) != 0 {
		t.Fatalf("auth next = %#v, want none", authNext)
	}

	networkDetails, networkCode, networkNextSteps := buildErrorDetails(&url.Error{
		Op:  "Get",
		URL: "http://192.168.1.50/api/v1/status",
		Err: errors.New("dial tcp: i/o timeout"),
	})
	if networkCode != exitcodes.NetworkError {
		t.Fatalf("network exit code = %d, want %d", networkCode, exitcodes.NetworkError)
	}
	if networkDetails.Code != "NETWORK_ERROR" {
		t.Fatalf("network code = %q, want %q", networkDetails.Code, "NETWORK_ERROR")
	}
	if !networkDetails.Retryable {
		t.Fatal("network retryable = false, want true")
	}
	if networkDetails.Remediation == nil || *networkDetails.Remediation != networkRemediation {
		t.Fatalf("network remediation = %#v, want %q", networkDetails.Remediation, networkRemediation)
	}
	if len(networkNextSteps) != 1 || networkNextSteps[0] != "evb-relay discover" {
		t.Fatalf("network next = %#v, want %q", networkNextSteps, "evb-relay discover")
	}
}
