package cmd

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
)

func TestRelayListOutputsJSON(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodGet)
		}
		if r.URL.Path != "/api/v1/relays" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/relays")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer relay-token" {
			t.Fatalf("Authorization header = %q, want %q", got, "Bearer relay-token")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(
			w,
			`{"modio_present":true,"modio_sync":"synchronized","relays":[{"group":"onboard","id":1,"state":false,"sync":null},{"group":"modio","id":1,"state":true,"sync":"synchronized"}]}`,
		)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "list",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload relayListResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if !payload.ModIOPresent {
		t.Fatal("modio_present = false, want true")
	}
	if payload.ModIOSync != "synchronized" {
		t.Fatalf("modio_sync = %q, want %q", payload.ModIOSync, "synchronized")
	}
	if len(payload.Relays) != 2 {
		t.Fatalf("relay count = %d, want 2", len(payload.Relays))
	}
	if payload.Relays[0].Group != "onboard" || payload.Relays[0].ID != 1 {
		t.Fatalf("first relay = %#v, want onboard:1", payload.Relays[0])
	}
	if payload.Relays[1].Group != "modio" || payload.Relays[1].ID != 1 || !payload.Relays[1].State {
		t.Fatalf("second relay = %#v, want modio:1 on", payload.Relays[1])
	}
}

func TestRelayOnUsesDirectRelayEndpoint(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPut {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodPut)
		}
		if r.URL.Path != "/api/v1/relays/onboard/1" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/relays/onboard/1")
		}

		var requestBody map[string]bool
		if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil {
			t.Fatalf("Decode() error = %v", err)
		}
		if got := requestBody["state"]; got != true {
			t.Fatalf("state = %t, want true", got)
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":1,"state":true}}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "on", "onboard:1",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload relaySingleResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if payload.Relay.Group != "onboard" || payload.Relay.ID != 1 || !payload.Relay.State {
		t.Fatalf("relay payload = %#v, want onboard:1 on", payload.Relay)
	}
}

func TestRelayToggleModIOUsesNativeToggleEndpoint(t *testing.T) {
	t.Parallel()

	var modioPostCount int

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPost && r.URL.Path == "/api/v1/relays/modio/2/toggle":
			modioPostCount++
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"relay":{"group":"modio","id":2,"state":false,"sync":"synchronized"}}`)
		default:
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "toggle", "modio:2",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	if modioPostCount != 1 {
		t.Fatalf("modio POST count = %d, want 1", modioPostCount)
	}

	var payload relaySingleResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if payload.Relay.Group != "modio" || payload.Relay.ID != 2 || payload.Relay.State {
		t.Fatalf("relay payload = %#v, want modio:2 off", payload.Relay)
	}
}

func TestRelaySetCoalescesModIOTargetsIntoSingleBulkRequest(t *testing.T) {
	t.Parallel()

	var onboardPutCount int
	var modioGetCount int
	var modioPutCount int
	var modioStates []bool

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/onboard/1":
			onboardPutCount++
			var requestBody map[string]bool
			if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil {
				t.Fatalf("Decode() error = %v", err)
			}
			if got := requestBody["state"]; got != true {
				t.Fatalf("onboard state = %t, want true", got)
			}

			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":1,"state":true}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays/modio":
			modioGetCount++
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(
				w,
				`{"relays":[{"group":"modio","id":1,"state":false,"sync":"synchronized"},{"group":"modio","id":2,"state":true,"sync":"synchronized"},{"group":"modio","id":3,"state":false,"sync":"synchronized"},{"group":"modio","id":4,"state":true,"sync":"synchronized"}]}`,
			)
		case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/modio":
			modioPutCount++
			var requestBody struct {
				States []bool `json:"states"`
			}
			if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil {
				t.Fatalf("Decode() error = %v", err)
			}
			modioStates = append([]bool(nil), requestBody.States...)

			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(
				w,
				`{"relays":[{"group":"modio","id":1,"state":true,"sync":"synchronized"},{"group":"modio","id":2,"state":true,"sync":"synchronized"},{"group":"modio","id":3,"state":false,"sync":"synchronized"},{"group":"modio","id":4,"state":false,"sync":"synchronized"}]}`,
			)
		default:
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "set", "onboard:1=on", "modio:1=on", "modio:4=off",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	if onboardPutCount != 1 {
		t.Fatalf("onboard PUT count = %d, want 1", onboardPutCount)
	}
	if modioGetCount != 1 {
		t.Fatalf("modio GET count = %d, want 1", modioGetCount)
	}
	if modioPutCount != 1 {
		t.Fatalf("modio PUT count = %d, want 1", modioPutCount)
	}
	wantStates := []bool{true, true, false, false}
	if len(modioStates) != len(wantStates) {
		t.Fatalf("modio states length = %d, want %d", len(modioStates), len(wantStates))
	}
	for index, want := range wantStates {
		if modioStates[index] != want {
			t.Fatalf("modio states[%d] = %t, want %t", index, modioStates[index], want)
		}
	}

	var payload relaySetResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if !payload.AllOK {
		t.Fatalf("all_ok = false, want true; payload=%#v", payload)
	}
	if len(payload.Results) != 3 {
		t.Fatalf("result count = %d, want 3", len(payload.Results))
	}
	for _, result := range payload.Results {
		if !result.OK {
			t.Fatalf("result = %#v, want ok", result)
		}
	}
}

func TestRelaySetRobotEnvelopeReportsPartialFailure(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/onboard/1":
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("X-FW-Version", "0.4.0")
			w.Header().Set("X-ModIO-Present", "false")
			w.Header().Set("X-ModIO-Sync", "absent")
			_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":1,"state":true}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays/modio":
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusServiceUnavailable)
			_, _ = io.WriteString(w, `{"error":{"code":"MODIO_NOT_PRESENT","message":"MOD-IO is not present","status":503}}`)
		default:
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--robot",
		"--format", "json",
		"relay", "set", "onboard:1=on", "modio:2=off",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want partial failure")
	}
	if got := exitcodes.FromError(err); got != exitcodes.GeneralError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.GeneralError)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["command"]; got != relaySetCommandName {
		t.Fatalf("command = %#v, want %q", got, relaySetCommandName)
	}
	if got := payload["exit_code"]; got != float64(1) {
		t.Fatalf("exit_code = %#v, want 1", got)
	}

	errorPayload, ok := payload["error"].(map[string]any)
	if !ok {
		t.Fatalf("error = %#v; want object", payload["error"])
	}
	if got := errorPayload["code"]; got != "PARTIAL_FAILURE" {
		t.Fatalf("error.code = %#v, want %q", got, "PARTIAL_FAILURE")
	}

	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("data = %#v; want object", payload["data"])
	}
	if got := data["all_ok"]; got != false {
		t.Fatalf("all_ok = %#v, want false", got)
	}

	results, ok := data["results"].([]any)
	if !ok || len(results) != 2 {
		t.Fatalf("results = %#v, want 2 items", data["results"])
	}
	first, ok := results[0].(map[string]any)
	if !ok {
		t.Fatalf("first result = %#v; want object", results[0])
	}
	if got := first["ok"]; got != true {
		t.Fatalf("first ok = %#v, want true", got)
	}
	second, ok := results[1].(map[string]any)
	if !ok {
		t.Fatalf("second result = %#v; want object", results[1])
	}
	if got := second["ok"]; got != false {
		t.Fatalf("second ok = %#v, want false", got)
	}
	secondError, ok := second["error"].(map[string]any)
	if !ok {
		t.Fatalf("second error = %#v; want object", second["error"])
	}
	if got := secondError["code"]; got != "MODIO_NOT_PRESENT" {
		t.Fatalf("second error.code = %#v, want %q", got, "MODIO_NOT_PRESENT")
	}
}

func TestRelaySetJSONPartialFailureMapsRawForbiddenToAuthForbidden(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/onboard/1":
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":1,"state":true}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays/modio":
			w.Header().Set("Content-Type", "text/plain")
			w.WriteHeader(http.StatusForbidden)
			_, _ = io.WriteString(w, "access denied")
		default:
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "set", "onboard:1=on", "modio:2=off",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want partial failure")
	}
	if got := exitcodes.FromError(err); got != exitcodes.GeneralError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.GeneralError)
	}

	var payload relaySetResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if payload.AllOK {
		t.Fatalf("all_ok = true, want false; payload=%#v", payload)
	}
	if len(payload.Results) != 2 {
		t.Fatalf("result count = %d, want 2", len(payload.Results))
	}
	if !payload.Results[0].OK {
		t.Fatalf("first result = %#v, want ok", payload.Results[0])
	}
	if payload.Results[1].OK {
		t.Fatalf("second result = %#v, want failure", payload.Results[1])
	}
	if payload.Results[1].Error == nil {
		t.Fatalf("second error = nil, want auth forbidden details; payload=%#v", payload)
	}
	if got := payload.Results[1].Error.Code; got != "AUTH_FORBIDDEN" {
		t.Fatalf("second error code = %q, want %q", got, "AUTH_FORBIDDEN")
	}
	if got := payload.Results[1].Error.Message; got != "access denied" {
		t.Fatalf("second error message = %q, want %q", got, "access denied")
	}
	if got := payload.Results[1].Error.HTTPStatus; got != http.StatusForbidden {
		t.Fatalf("second error status = %d, want %d", got, http.StatusForbidden)
	}
}

func TestRelaySetJSONPartialFailureCanonicalizesAuthInvalidToAuthForbidden(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/onboard/1":
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":1,"state":true}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays/modio":
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusForbidden)
			_, _ = io.WriteString(w, `{"error":{"code":"AUTH_INVALID","message":"provided token is invalid","status":403}}`)
		default:
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "set", "onboard:1=on", "modio:2=off",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want partial failure")
	}
	if got := exitcodes.FromError(err); got != exitcodes.GeneralError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.GeneralError)
	}

	var payload relaySetResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if len(payload.Results) != 2 || payload.Results[1].Error == nil {
		t.Fatalf("results = %#v, want auth failure in second result", payload.Results)
	}
	if got := payload.Results[1].Error.Code; got != "AUTH_FORBIDDEN" {
		t.Fatalf("second error code = %q, want %q", got, "AUTH_FORBIDDEN")
	}
}

func TestRelaySetSurfacesModIONotPresentForModioOnlyBatch(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/api/v1/relays/modio" {
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = io.WriteString(w, `{"error":{"code":"MODIO_NOT_PRESENT","message":"MOD-IO is not present","status":503}}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--robot",
		"--format", "json",
		"relay", "set", "modio:2=off",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want hardware-unavailable error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.HardwareUnavailable {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.HardwareUnavailable)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["exit_code"]; got != float64(7) {
		t.Fatalf("exit_code = %#v, want 7", got)
	}
	errorPayload, ok := payload["error"].(map[string]any)
	if !ok {
		t.Fatalf("error = %#v; want object", payload["error"])
	}
	if got := errorPayload["code"]; got != "MODIO_NOT_PRESENT" {
		t.Fatalf("error.code = %#v, want %q", got, "MODIO_NOT_PRESENT")
	}
}

func TestRelaySetRejectsPartialModIOBatchWhenSyncIsUnknown(t *testing.T) {
	t.Parallel()

	var modioGetCount int

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/api/v1/relays/modio" {
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-FW-Version", "0.4.0")
		w.Header().Set("X-ModIO-Present", "true")
		w.Header().Set("X-ModIO-Sync", "unknown")
		_, _ = io.WriteString(
			w,
			`{"relays":[{"group":"modio","id":1,"state":false,"sync":"unknown"},{"group":"modio","id":2,"state":false,"sync":"unknown"},{"group":"modio","id":3,"state":false,"sync":"unknown"},{"group":"modio","id":4,"state":false,"sync":"unknown"}]}`,
		)
		modioGetCount++
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--robot",
		"--format", "json",
		"relay", "set", "modio:2=off",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want state error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.StateError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.StateError)
	}
	if modioGetCount != 1 {
		t.Fatalf("modio request count = %d, want 1 GET only", modioGetCount)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["exit_code"]; got != float64(6) {
		t.Fatalf("exit_code = %#v, want 6", got)
	}
	errorPayload, ok := payload["error"].(map[string]any)
	if !ok {
		t.Fatalf("error = %#v; want object", payload["error"])
	}
	if got := errorPayload["code"]; got != "MODIO_STATE_UNKNOWN" {
		t.Fatalf("error.code = %#v, want %q", got, "MODIO_STATE_UNKNOWN")
	}
}

func TestRelaySetExpandsAllShorthandAcrossBothRelayGroups(t *testing.T) {
	t.Parallel()

	var onboardPutPaths []string
	var modioPutCount int
	var modioStates []bool

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPut && (r.URL.Path == "/api/v1/relays/onboard/1" || r.URL.Path == "/api/v1/relays/onboard/2"):
			onboardPutPaths = append(onboardPutPaths, r.URL.Path)
			w.Header().Set("Content-Type", "application/json")
			if r.URL.Path == "/api/v1/relays/onboard/1" {
				_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":1,"state":true}}`)
				return
			}
			_, _ = io.WriteString(w, `{"relay":{"group":"onboard","id":2,"state":true}}`)
		case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/modio":
			modioPutCount++
			var requestBody struct {
				States []bool `json:"states"`
			}
			if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil {
				t.Fatalf("Decode() error = %v", err)
			}
			modioStates = append([]bool(nil), requestBody.States...)

			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(
				w,
				`{"relays":[{"group":"modio","id":1,"state":false,"sync":"synchronized"},{"group":"modio","id":2,"state":false,"sync":"synchronized"},{"group":"modio","id":3,"state":false,"sync":"synchronized"},{"group":"modio","id":4,"state":false,"sync":"synchronized"}]}`,
			)
		default:
			t.Fatalf("unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "relay-token",
		"--format", "json",
		"relay", "set", "onboard:all=on", "modio:all=off",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	if len(onboardPutPaths) != 2 {
		t.Fatalf("onboard PUT paths = %#v, want 2 requests", onboardPutPaths)
	}
	if modioPutCount != 1 {
		t.Fatalf("modio PUT count = %d, want 1", modioPutCount)
	}
	wantStates := []bool{false, false, false, false}
	if len(modioStates) != len(wantStates) {
		t.Fatalf("modio states length = %d, want %d", len(modioStates), len(wantStates))
	}
	for index, want := range wantStates {
		if modioStates[index] != want {
			t.Fatalf("modio states[%d] = %t, want %t", index, modioStates[index], want)
		}
	}

	var payload relaySetResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if !payload.AllOK {
		t.Fatalf("all_ok = false, want true; payload=%#v", payload)
	}
	if len(payload.Results) != 6 {
		t.Fatalf("result count = %d, want 6", len(payload.Results))
	}
	for _, result := range payload.Results {
		if !result.OK {
			t.Fatalf("result = %#v, want ok", result)
		}
	}
}
