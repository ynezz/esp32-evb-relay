package client

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func TestDeviceContextJSONUsesFirmwareVersionField(t *testing.T) {
	t.Parallel()

	payload, err := json.Marshal(DeviceContext{FirmwareVersion: "0.3.1"})
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}

	got := string(payload)
	if !strings.Contains(got, `"firmware_version":"0.3.1"`) {
		t.Fatalf("marshaled device context = %s; want firmware_version field", got)
	}
	if strings.Contains(got, `"fw_version"`) {
		t.Fatalf("marshaled device context = %s; unexpected legacy fw_version field", got)
	}
}

func TestDeviceContextFromHeadersReturnsNilWhenHeadersAreAbsent(t *testing.T) {
	t.Parallel()

	if got := deviceContextFromHeaders(http.Header{}); got != nil {
		t.Fatalf("deviceContextFromHeaders(empty) = %#v; want nil", got)
	}
}

func TestDeviceContextFromHeadersParsesExpectedFields(t *testing.T) {
	t.Parallel()

	headers := http.Header{}
	headers.Set("X-FW-Version", "0.3.1")
	headers.Set("X-ModIO-Present", "true")
	headers.Set("X-ModIO-Sync", "synchronized")

	got := deviceContextFromHeaders(headers)
	if got == nil {
		t.Fatal("deviceContextFromHeaders(headers) = nil; want device context")
	}
	if got.FirmwareVersion != "0.3.1" {
		t.Fatalf("FirmwareVersion = %q; want %q", got.FirmwareVersion, "0.3.1")
	}
	if got.ModIOPresent == nil || !*got.ModIOPresent {
		t.Fatalf("ModIOPresent = %v; want true", got.ModIOPresent)
	}
	if got.ModIOSync != "synchronized" {
		t.Fatalf("ModIOSync = %q; want %q", got.ModIOSync, "synchronized")
	}
}

func TestNormalizeBaseURLRejectsPortOnlyHost(t *testing.T) {
	t.Parallel()

	_, err := normalizeBaseURL(":8080")
	if err == nil {
		t.Fatal("normalizeBaseURL(:8080) succeeded; want error")
	}
	if !strings.Contains(err.Error(), "hostname or IP address") {
		t.Fatalf("normalizeBaseURL(:8080) error = %v", err)
	}
}

func TestNormalizeBaseURLAcceptsHostWithExplicitPort(t *testing.T) {
	t.Parallel()

	got, err := normalizeBaseURL("relay-box:8080")
	if err != nil {
		t.Fatalf("normalizeBaseURL(relay-box:8080) error = %v", err)
	}

	want := &url.URL{
		Scheme: "http",
		Host:   "relay-box:8080",
		Path:   apiBasePath,
	}
	if got.String() != want.String() {
		t.Fatalf("normalizeBaseURL(relay-box:8080) = %q; want %q", got.String(), want.String())
	}
}

func TestNewRejectsZeroTimeout(t *testing.T) {
	t.Parallel()

	_, err := New(Config{
		Host:    "relay-box",
		Timeout: 0,
	})
	if err == nil {
		t.Fatal("New() succeeded; want timeout validation error")
	}
	if !strings.Contains(err.Error(), "timeout must be greater than zero") {
		t.Fatalf("New() error = %v", err)
	}
}

func TestNewPreservesConfiguredTimeout(t *testing.T) {
	t.Parallel()

	client, err := New(Config{
		Host:    "relay-box",
		Timeout: 3 * time.Second,
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}

	if got := client.httpClient.Timeout; got != 3*time.Second {
		t.Fatalf("http timeout = %v; want %v", got, 3*time.Second)
	}
}

func TestUploadBinarySendsOctetStreamRequest(t *testing.T) {
	t.Parallel()

	firmware := []byte("firmware-binary")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			t.Errorf("request method = %q, want %q", r.Method, http.MethodPost)
		}
		if r.URL.Path != "/api/v1/ota" {
			t.Errorf("request path = %q, want %q", r.URL.Path, "/api/v1/ota")
		}
		if got := r.Header.Get("Accept"); got != "application/json" {
			t.Errorf("Accept header = %q, want %q", got, "application/json")
		}
		if got := r.Header.Get("Content-Type"); got != "application/octet-stream" {
			t.Errorf("Content-Type header = %q, want %q", got, "application/octet-stream")
		}
		if got := r.ContentLength; got != int64(len(firmware)) {
			t.Errorf("Content-Length = %d, want %d", got, len(firmware))
		}
		if got := r.Header.Get("Authorization"); got != "Bearer upload-token" {
			t.Errorf("Authorization header = %q, want %q", got, "Bearer upload-token")
		}

		payload, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("io.ReadAll() error = %v", err)
		}
		if !bytes.Equal(payload, firmware) {
			t.Fatalf("uploaded payload = %q, want %q", payload, firmware)
		}

		w.Header().Set("X-FW-Version", "0.4.0")
		w.WriteHeader(http.StatusAccepted)
	}))
	defer server.Close()

	uploadClient, err := New(Config{
		Host:     server.URL,
		APIToken: "upload-token",
		Timeout:  time.Second,
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}

	result, err := uploadClient.UploadBinary(
		context.Background(),
		"/ota",
		bytes.NewReader(firmware),
		int64(len(firmware)),
		nil,
	)
	if err != nil {
		t.Fatalf("UploadBinary() error = %v", err)
	}

	if result.StatusCode != http.StatusAccepted {
		t.Fatalf("status = %d, want %d", result.StatusCode, http.StatusAccepted)
	}
	if result.DeviceContext == nil || result.DeviceContext.FirmwareVersion != "0.4.0" {
		t.Fatalf("device context = %#v; want firmware version", result.DeviceContext)
	}
}

func TestUploadBinaryRejectsNonPositiveSize(t *testing.T) {
	t.Parallel()

	uploadClient, err := New(Config{
		Host:    "relay-box",
		Timeout: time.Second,
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}

	_, err = uploadClient.UploadBinary(context.Background(), "/ota", bytes.NewReader(nil), 0, nil)
	if err == nil {
		t.Fatal("UploadBinary() succeeded; want validation error")
	}
	if !strings.Contains(err.Error(), "upload size must be greater than zero") {
		t.Fatalf("UploadBinary() error = %v", err)
	}
}

func TestWatchEventsOnceParsesDeviceContextAndEvents(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/events" {
			t.Errorf("request path = %q, want %q", r.URL.Path, "/api/v1/events")
			http.Error(w, "unexpected path", http.StatusNotFound)
			return
		}
		if got := r.Header.Get("Accept"); got != "text/event-stream" {
			t.Errorf("Accept header = %q, want %q", got, "text/event-stream")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer stream-token" {
			t.Errorf("Authorization header = %q, want %q", got, "Bearer stream-token")
		}

		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("X-FW-Version", "0.3.1")
		w.Header().Set("X-ModIO-Present", "true")
		w.Header().Set("X-ModIO-Sync", "synchronized")

		_, _ = io.WriteString(w, "event: digital_input\n")
		_, _ = io.WriteString(w, "data: {\"id\":2,\"state\":true,\"ts_ms\":12345}\n\n")
		_, _ = io.WriteString(w, "event: heartbeat\n")
		_, _ = io.WriteString(w, "data: {\"ts_ms\":42345}\n\n")
	}))
	defer server.Close()

	streamClient, err := New(Config{
		Host:     server.URL,
		APIToken: "stream-token",
		Timeout:  time.Second,
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}

	var started Result
	var events []StreamEvent
	err = streamClient.WatchEventsOnce(
		context.Background(),
		func(result Result) error {
			started = result
			return nil
		},
		func(event StreamEvent) error {
			events = append(events, event)
			return nil
		},
	)
	if !errors.Is(err, io.EOF) {
		t.Fatalf("WatchEventsOnce() error = %v, want io.EOF", err)
	}

	if started.StatusCode != http.StatusOK {
		t.Fatalf("started status = %d, want %d", started.StatusCode, http.StatusOK)
	}
	if started.DeviceContext == nil {
		t.Fatal("started device context = nil; want parsed headers")
	}
	if started.DeviceContext.FirmwareVersion != "0.3.1" {
		t.Fatalf("firmware version = %q, want %q", started.DeviceContext.FirmwareVersion, "0.3.1")
	}
	if started.DeviceContext.ModIOPresent == nil || !*started.DeviceContext.ModIOPresent {
		t.Fatalf("modio present = %v, want true", started.DeviceContext.ModIOPresent)
	}
	if started.DeviceContext.ModIOSync != "synchronized" {
		t.Fatalf("modio sync = %q, want %q", started.DeviceContext.ModIOSync, "synchronized")
	}

	if len(events) != 2 {
		t.Fatalf("events length = %d, want 2", len(events))
	}
	if events[0].Event != "digital_input" {
		t.Fatalf("events[0].Event = %q, want %q", events[0].Event, "digital_input")
	}
	if events[1].Event != "heartbeat" {
		t.Fatalf("events[1].Event = %q, want %q", events[1].Event, "heartbeat")
	}

	firstPayload, ok := events[0].Data.(json.RawMessage)
	if !ok {
		t.Fatalf("events[0].Data = %#v; want json.RawMessage", events[0].Data)
	}
	var firstEvent map[string]any
	if err := json.Unmarshal(firstPayload, &firstEvent); err != nil {
		t.Fatalf("json.Unmarshal(firstPayload) error = %v", err)
	}
	if got := firstEvent["id"]; got != float64(2) {
		t.Fatalf("first payload id = %#v, want 2", got)
	}
	if got := firstEvent["state"]; got != true {
		t.Fatalf("first payload state = %#v, want true", got)
	}
}
