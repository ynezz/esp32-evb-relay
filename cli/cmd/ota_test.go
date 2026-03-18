package cmd

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

func TestOTAFlashUploadsBinaryAndOutputsJSON(t *testing.T) {
	t.Parallel()

	firmwarePath, firmwareBody := writeTempFirmware(t, []byte{0xde, 0xad, 0xbe, 0xef})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			t.Fatalf("request method = %q, want %q", r.Method, http.MethodPost)
		}
		if r.URL.Path != "/api/v1/ota" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/ota")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer ota-token" {
			t.Fatalf("Authorization header = %q, want %q", got, "Bearer ota-token")
		}
		if got := r.Header.Get("Accept"); got != "application/json" {
			t.Fatalf("Accept header = %q, want %q", got, "application/json")
		}
		if got := r.Header.Get("Content-Type"); got != "application/octet-stream" {
			t.Fatalf("Content-Type header = %q, want %q", got, "application/octet-stream")
		}
		if got := r.ContentLength; got != int64(len(firmwareBody)) {
			t.Fatalf("ContentLength = %d, want %d", got, len(firmwareBody))
		}

		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("io.ReadAll() error = %v", err)
		}
		if !bytes.Equal(body, firmwareBody) {
			t.Fatalf("request body = %v, want %v", body, firmwareBody)
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"reboot_in_seconds":5}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "ota-token",
		"--format", "json",
		"ota", "flash", firmwarePath,
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload otaFlashResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if payload.UploadedBytes != int64(len(firmwareBody)) {
		t.Fatalf("uploaded_bytes = %d, want %d", payload.UploadedBytes, len(firmwareBody))
	}
	if payload.FirmwareFile != firmwarePath {
		t.Fatalf("firmware_file = %q, want %q", payload.FirmwareFile, firmwarePath)
	}
	if payload.RebootInSeconds != 5 {
		t.Fatalf("reboot_in_seconds = %d, want 5", payload.RebootInSeconds)
	}

	if got := stderr.String(); !strings.Contains(got, "Uploading firmware:") {
		t.Fatalf("stderr = %q, want upload progress", got)
	}
	if got := stderr.String(); !strings.Contains(got, otaRebootWarning(5)) {
		t.Fatalf("stderr = %q, want reboot warning", got)
	}
}

func TestOTAFlashSupportsRobotJSONEnvelope(t *testing.T) {
	t.Parallel()

	firmwarePath, firmwareBody := writeTempFirmware(t, []byte{0xca, 0xfe, 0xba, 0xbe})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("io.ReadAll() error = %v", err)
		}
		if !bytes.Equal(body, firmwareBody) {
			t.Fatalf("request body = %v, want %v", body, firmwareBody)
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-FW-Version", "0.3.1")
		w.Header().Set("X-ModIO-Present", "true")
		w.Header().Set("X-ModIO-Sync", "synchronized")
		_, _ = io.WriteString(w, `{"reboot_in_seconds":2}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "ota-token",
		"--robot",
		"--format", "json",
		"ota", "flash", firmwarePath,
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["command"]; got != otaCommandName {
		t.Fatalf("command = %#v, want %q", got, otaCommandName)
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
	if got := deviceContext["firmware_version"]; got != "0.3.1" {
		t.Fatalf("firmware_version = %#v, want %q", got, "0.3.1")
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
	if got := data["uploaded_bytes"]; got != float64(len(firmwareBody)) {
		t.Fatalf("uploaded_bytes = %#v, want %d", got, len(firmwareBody))
	}
	if got := data["firmware_file"]; got != firmwarePath {
		t.Fatalf("firmware_file = %#v, want %q", got, firmwarePath)
	}
	if got := data["reboot_in_seconds"]; got != float64(2) {
		t.Fatalf("reboot_in_seconds = %#v, want 2", got)
	}

	warnings, ok := payload["warnings"].([]any)
	if !ok || len(warnings) != 1 {
		t.Fatalf("warnings = %#v, want one warning", payload["warnings"])
	}
	if got := warnings[0]; got != otaRebootWarning(2) {
		t.Fatalf("warning = %#v, want %q", got, otaRebootWarning(2))
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want empty", stderr.String())
	}
}

func TestOTAFlashRobotModeWrapsMissingFirmwareFileErrors(t *testing.T) {
	t.Parallel()

	missingPath := filepath.Join(t.TempDir(), "missing.bin")
	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{
		"--robot",
		"--format", "json",
		"ota", "flash", missingPath,
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.BadArgument {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.BadArgument)
	}

	var payload map[string]any
	if unmarshalErr := json.Unmarshal(stdout.Bytes(), &payload); unmarshalErr != nil {
		t.Fatalf("json.Unmarshal() error = %v", unmarshalErr)
	}

	if got := payload["command"]; got != otaCommandName {
		t.Fatalf("command = %#v, want %q", got, otaCommandName)
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
	if got := stderr.String(); got != "" {
		t.Fatalf("stderr = %q, want empty", got)
	}
}

func TestOTATimeoutUsesLongerDefault(t *testing.T) {
	t.Parallel()

	if got := otaTimeout(appconfig.DefaultTimeout); got != otaDefaultTimeout {
		t.Fatalf("otaTimeout(default) = %s, want %s", got, otaDefaultTimeout)
	}
}

func TestOTATimeoutPreservesExplicitOverride(t *testing.T) {
	t.Parallel()

	if got := otaTimeout(45 * time.Second); got != 45*time.Second {
		t.Fatalf("otaTimeout(45s) = %s, want %s", got, 45*time.Second)
	}
}

func TestCloseFirmwareUploadWrapsCloseErrors(t *testing.T) {
	t.Parallel()

	path, _ := writeTempFirmware(t, []byte{0xde, 0xad, 0xbe, 0xef})
	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("os.Open() error = %v", err)
	}
	if err := file.Close(); err != nil {
		t.Fatalf("file.Close() error = %v", err)
	}

	err = closeFirmwareUpload(&firmwareUpload{
		Path: path,
		File: file,
	})
	if err == nil {
		t.Fatal("closeFirmwareUpload() succeeded; want error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.GeneralError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.GeneralError)
	}
	if !strings.Contains(err.Error(), "close firmware file") {
		t.Fatalf("closeFirmwareUpload() error = %v; want wrapped close context", err)
	}
}

func writeTempFirmware(t *testing.T, payload []byte) (string, []byte) {
	t.Helper()

	path := filepath.Join(t.TempDir(), "firmware.bin")
	if err := os.WriteFile(path, payload, 0o600); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}

	return path, payload
}
