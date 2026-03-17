package client

import (
	"encoding/json"
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
