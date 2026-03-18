package client

import (
	"encoding/json"
	"net/url"
	"strings"
	"testing"
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
