package cmd

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
)

func TestConfigShowOutputsJSON(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodGet)
		}
		if r.URL.Path != "/api/v1/config" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/config")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer cfg-token" {
			t.Fatalf("Authorization header = %q, want %q", got, "Bearer cfg-token")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"config":{"poll_interval_ms":250,"hostname":"lab-relay","modio_boot_policy":"all_off","api_token_set":true,"wifi":{"ssid_set":true,"passphrase_set":true,"network_policy":"prefer_ethernet"}}}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "cfg-token",
		"--format", "json",
		"config", "show",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload configShowResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if payload.Config.PollIntervalMS != 250 {
		t.Fatalf("poll_interval_ms = %d, want 250", payload.Config.PollIntervalMS)
	}
	if payload.Config.Hostname != "lab-relay" {
		t.Fatalf("hostname = %q, want %q", payload.Config.Hostname, "lab-relay")
	}
	if payload.Config.ModIOBootPolicy != "all_off" {
		t.Fatalf("modio_boot_policy = %q, want %q", payload.Config.ModIOBootPolicy, "all_off")
	}
	if !payload.Config.APITokenSet {
		t.Fatal("api_token_set = false, want true")
	}
	if !payload.Config.WiFi.SSIDSet {
		t.Fatal("wifi.ssid_set = false, want true")
	}
	if !payload.Config.WiFi.PassphraseSet {
		t.Fatal("wifi.passphrase_set = false, want true")
	}
	if payload.Config.WiFi.NetworkPolicy != "prefer_ethernet" {
		t.Fatalf("wifi.network_policy = %q, want %q", payload.Config.WiFi.NetworkPolicy, "prefer_ethernet")
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want empty", stderr.String())
	}
}

func TestConfigSetParsesAssignmentsAndWarnsOnRestart(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPut {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodPut)
		}
		if r.URL.Path != "/api/v1/config" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/config")
		}

		var requestBody map[string]any
		if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil {
			t.Fatalf("json.Decode() error = %v", err)
		}

		if got := requestBody["poll_interval_ms"]; got != float64(250) {
			t.Fatalf("poll_interval_ms = %#v, want 250", got)
		}
		if got := requestBody["hostname"]; got != "lab-relay" {
			t.Fatalf("hostname = %#v, want %q", got, "lab-relay")
		}
		if got := requestBody["modio_boot_policy"]; got != "all_off" {
			t.Fatalf("modio_boot_policy = %#v, want %q", got, "all_off")
		}
		if got := requestBody["api_token"]; got != "new-secret" {
			t.Fatalf("api_token = %#v, want %q", got, "new-secret")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"changes":[{"key":"poll_interval_ms","old":100,"new":250,"live":true},{"key":"hostname","old":"esp32-evb-relay","new":"lab-relay","live":false},{"key":"modio_boot_policy","old":"leave_unchanged","new":"all_off","live":false}],"restart_required":true}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "cfg-token",
		"--format", "json",
		"config", "set",
		"poll_interval_ms=250",
		"hostname=lab-relay",
		"modio_boot_policy=all_off",
		"api_token=new-secret",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload configSetResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if len(payload.Changes) != 3 {
		t.Fatalf("changes length = %d, want 3", len(payload.Changes))
	}
	if !payload.RestartRequired {
		t.Fatal("restart_required = false, want true")
	}
	if got := stderr.String(); !strings.Contains(got, "Restart the device for all changes to take full effect.") {
		t.Fatalf("stderr = %q, want restart warning", got)
	}
}

func TestConfigSetSupportsRobotJSONEnvelope(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-FW-Version", "0.4.0")
		w.Header().Set("X-ModIO-Present", "false")
		_, _ = io.WriteString(w, `{"changes":[{"key":"hostname","old":"esp32-evb-relay","new":"lab-relay","live":false}],"restart_required":true}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "cfg-token",
		"--robot",
		"--format", "json",
		"config", "set", "hostname=lab-relay",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["command"]; got != configSetCommandName {
		t.Fatalf("command = %#v, want %q", got, configSetCommandName)
	}
	if got := payload["exit_code"]; got != float64(0) {
		t.Fatalf("exit_code = %#v, want 0", got)
	}

	parsedURL, err := url.Parse(server.URL)
	if err != nil {
		t.Fatalf("url.Parse() error = %v", err)
	}
	if got := payload["host"]; got != parsedURL.Host {
		t.Fatalf("host = %#v, want %q", got, parsedURL.Host)
	}

	deviceContext, ok := payload["device_context"].(map[string]any)
	if !ok {
		t.Fatalf("device_context = %#v; want object", payload["device_context"])
	}
	if got := deviceContext["firmware_version"]; got != "0.4.0" {
		t.Fatalf("firmware_version = %#v, want %q", got, "0.4.0")
	}
	if got := deviceContext["modio_present"]; got != false {
		t.Fatalf("modio_present = %#v, want false", got)
	}

	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("data = %#v; want object", payload["data"])
	}
	if got := data["restart_required"]; got != true {
		t.Fatalf("restart_required = %#v, want true", got)
	}

	warnings, ok := payload["warnings"].([]any)
	if !ok || len(warnings) != 1 {
		t.Fatalf("warnings = %#v, want one warning", payload["warnings"])
	}
	if got := warnings[0]; got != "Restart the device for all changes to take full effect." {
		t.Fatalf("warning = %#v, want restart warning", got)
	}

	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want empty", stderr.String())
	}
}

func TestConfigWiFiParsesAssignmentsAndWarnsOnRestart(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPut {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodPut)
		}
		if r.URL.Path != "/api/v1/config/wifi" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/config/wifi")
		}

		var requestBody map[string]any
		if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil {
			t.Fatalf("json.Decode() error = %v", err)
		}

		if got := requestBody["ssid"]; got != "lab-net" {
			t.Fatalf("ssid = %#v, want %q", got, "lab-net")
		}
		if got := requestBody["passphrase"]; got != "new-secret" {
			t.Fatalf("passphrase = %#v, want %q", got, "new-secret")
		}
		if got := requestBody["network_policy"]; got != "wifi_only" {
			t.Fatalf("network_policy = %#v, want %q", got, "wifi_only")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"changes":[{"key":"wifi_ssid_set","old":false,"new":true,"live":false},{"key":"wifi_passphrase_set","old":false,"new":true,"live":false},{"key":"network_policy","old":"ethernet_only","new":"wifi_only","live":false}],"restart_required":true}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "cfg-token",
		"--format", "json",
		"config", "wifi",
		"ssid=lab-net",
		"passphrase=new-secret",
		"network_policy=wifi_only",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload configSetResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if len(payload.Changes) != 3 {
		t.Fatalf("changes length = %d, want 3", len(payload.Changes))
	}
	if !payload.RestartRequired {
		t.Fatal("restart_required = false, want true")
	}
	if got := stderr.String(); !strings.Contains(got, "Restart the device for all changes to take full effect.") {
		t.Fatalf("stderr = %q, want restart warning", got)
	}
}

func TestConfigWiFiClearBuildsNullSSID(t *testing.T) {
	t.Parallel()

	requestBody, err := parseWiFiConfigAssignments([]string{"clear=true", "network_policy=ethernet_only"})
	if err != nil {
		t.Fatalf("parseWiFiConfigAssignments() error = %v", err)
	}

	if _, ok := requestBody["ssid"]; !ok {
		t.Fatalf("ssid missing from request body: %#v", requestBody)
	}
	if got := requestBody["ssid"]; got != nil {
		t.Fatalf("ssid = %#v, want nil", got)
	}
	if got := requestBody["network_policy"]; got != "ethernet_only" {
		t.Fatalf("network_policy = %#v, want %q", got, "ethernet_only")
	}
}

func TestConfigSetWrapsParseErrorsInRobotMode(t *testing.T) {
	t.Parallel()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", "relay-box",
		"--robot",
		"--format", "json",
		"config", "set", "bogus=value",
	})

	err := command.Execute()
	if got := exitcodes.FromError(err); got != exitcodes.BadArgument {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.BadArgument)
	}

	var payload map[string]any
	if unmarshalErr := json.Unmarshal(stdout.Bytes(), &payload); unmarshalErr != nil {
		t.Fatalf("json.Unmarshal() error = %v", unmarshalErr)
	}

	if got := payload["command"]; got != configSetCommandName {
		t.Fatalf("command = %#v, want %q", got, configSetCommandName)
	}
	if _, exists := payload["data"]; exists {
		t.Fatalf("data unexpectedly present in error payload: %#v", payload["data"])
	}

	errorDetails, ok := payload["error"].(map[string]any)
	if !ok {
		t.Fatalf("error = %#v; want object", payload["error"])
	}
	if got := errorDetails["code"]; got != "BAD_ARGUMENT" {
		t.Fatalf("error.code = %#v, want %q", got, "BAD_ARGUMENT")
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want empty", stderr.String())
	}
}
