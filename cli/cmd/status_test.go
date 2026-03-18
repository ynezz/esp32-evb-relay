package cmd

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
)

func TestStatusOutputsJSON(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodGet)
		}
		if r.URL.Path != "/api/v1/status" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/status")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer status-token" {
			t.Fatalf("Authorization header = %q, want %q", got, "Bearer status-token")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(
			w,
			`{"uptime_seconds":42,"firmware_version":"0.4.0","free_heap_bytes":123456,"network":{"hostname":"lab-relay","connected":true,"ip":"192.168.1.60","netmask":"255.255.255.0","gateway":"192.168.1.1"},"modio":{"present":true,"sync":"synchronized"}}`,
		)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "status-token",
		"--format", "json",
		"status",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload statusResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload.Status.UptimeSeconds; got != 42 {
		t.Fatalf("uptime_seconds = %v, want 42", got)
	}
	if got := payload.Status.FirmwareVersion; got != "0.4.0" {
		t.Fatalf("firmware_version = %q, want %q", got, "0.4.0")
	}
	if got := payload.Status.FreeHeapBytes; got != 123456 {
		t.Fatalf("free_heap_bytes = %v, want 123456", got)
	}
	if got := payload.Status.Network.Hostname; got != "lab-relay" {
		t.Fatalf("network.hostname = %q, want %q", got, "lab-relay")
	}
	if got := payload.Status.Network.IP; got != "192.168.1.60" {
		t.Fatalf("network.ip = %q, want %q", got, "192.168.1.60")
	}
	if got := payload.Status.ModIO.Present; got != true {
		t.Fatalf("modio.present = %t, want true", got)
	}
	if got := payload.Status.ModIO.Sync; got != "synchronized" {
		t.Fatalf("modio.sync = %q, want %q", got, "synchronized")
	}
}

func TestStatusSupportsRobotJSONEnvelope(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-FW-Version", "0.4.0")
		w.Header().Set("X-ModIO-Present", "true")
		w.Header().Set("X-ModIO-Sync", "synchronized")
		_, _ = io.WriteString(
			w,
			`{"uptime_seconds":42,"firmware_version":"0.4.0","free_heap_bytes":123456,"network":{"hostname":"lab-relay","connected":true,"ip":"192.168.1.60","netmask":"255.255.255.0","gateway":"192.168.1.1"},"modio":{"present":true,"sync":"synchronized"}}`,
		)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "status-token",
		"--robot",
		"--format", "json",
		"status",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["command"]; got != statusCommandName {
		t.Fatalf("command = %#v, want %q", got, statusCommandName)
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
	if got := deviceContext["modio_present"]; got != true {
		t.Fatalf("modio_present = %#v, want true", got)
	}
	if got := deviceContext["modio_sync"]; got != "synchronized" {
		t.Fatalf("modio_sync = %#v, want %q", got, "synchronized")
	}

	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("data = %#v; want object", payload["data"])
	}
	status, ok := data["status"].(map[string]any)
	if !ok {
		t.Fatalf("data.status = %#v; want object", data["status"])
	}
	if got := status["firmware_version"]; got != "0.4.0" {
		t.Fatalf("status.firmware_version = %#v, want %q", got, "0.4.0")
	}
}

func TestStatusResultTableOutput(t *testing.T) {
	t.Parallel()

	result := statusResult{
		Status: deviceStatus{
			UptimeSeconds:   42,
			FirmwareVersion: "0.4.0",
			FreeHeapBytes:   123456,
			Network: deviceNetworkStatus{
				Hostname:  "lab-relay",
				Connected: true,
				IP:        "192.168.1.60",
				Netmask:   "255.255.255.0",
				Gateway:   "192.168.1.1",
			},
			ModIO: deviceModIOStatus{
				Present: true,
				Sync:    "synchronized",
			},
		},
	}

	table, err := result.TableOutput()
	if err != nil {
		t.Fatalf("TableOutput() error = %v", err)
	}
	if len(table.Headers) != 10 {
		t.Fatalf("header count = %d, want 10", len(table.Headers))
	}
	if len(table.Rows) != 1 {
		t.Fatalf("row count = %d, want 1", len(table.Rows))
	}
	if got := table.Rows[0][0]; got != "42" {
		t.Fatalf("uptime cell = %q, want %q", got, "42")
	}
	if got := table.Rows[0][8]; got != "true" {
		t.Fatalf("modio present cell = %q, want %q", got, "true")
	}
}

func TestStatusResultPlainOutput(t *testing.T) {
	t.Parallel()

	result := statusResult{
		Status: deviceStatus{
			UptimeSeconds:   42,
			FirmwareVersion: "0.4.0",
			FreeHeapBytes:   123456,
			Network: deviceNetworkStatus{
				Hostname:  "lab-relay",
				Connected: true,
				IP:        "192.168.1.60",
				Netmask:   "255.255.255.0",
				Gateway:   "192.168.1.1",
			},
			ModIO: deviceModIOStatus{
				Present: true,
				Sync:    "synchronized",
			},
		},
	}

	lines, err := result.PlainOutput()
	if err != nil {
		t.Fatalf("PlainOutput() error = %v", err)
	}
	if len(lines) != 10 {
		t.Fatalf("line count = %d, want 10", len(lines))
	}
	if got := lines[0]; got != "uptime_seconds=42" {
		t.Fatalf("line 0 = %q, want %q", got, "uptime_seconds=42")
	}
	if got := lines[8]; got != "modio.present=true" {
		t.Fatalf("line 8 = %q, want %q", got, "modio.present=true")
	}
}
