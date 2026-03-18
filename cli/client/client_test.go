package client

import (
	"encoding/json"
	"net/http"
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
