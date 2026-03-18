package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"testing"
	"time"

	"github.com/hashicorp/mdns"
)

func TestDiscoverOutputsJSONForMatchingBoardDevices(t *testing.T) {
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		if params.Service != discoverServiceName {
			t.Fatalf("service = %q, want %q", params.Service, discoverServiceName)
		}
		if params.Timeout != discoverDefaultTimeout {
			t.Fatalf("timeout = %s, want %s", params.Timeout, discoverDefaultTimeout)
		}

		params.Entries <- &mdns.ServiceEntry{
			Host:       "relay-a.local.",
			AddrV4:     net.ParseIP("192.168.1.50"),
			Port:       80,
			InfoFields: []string{"fw_version=0.3.1", "board=esp32-evb"},
		}
		params.Entries <- &mdns.ServiceEntry{
			Host:       "printer.local.",
			AddrV4:     net.ParseIP("192.168.1.90"),
			Port:       631,
			InfoFields: []string{"model=office-printer"},
		}
		return nil
	})
	defer restore()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--format", "json", "discover"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload struct {
		Devices []struct {
			Hostname string   `json:"hostname"`
			IP       string   `json:"ip"`
			Port     int      `json:"port"`
			TXT      []string `json:"txt"`
		} `json:"devices"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if len(payload.Devices) != 1 {
		t.Fatalf("device count = %d, want 1", len(payload.Devices))
	}
	if got := payload.Devices[0].Hostname; got != "relay-a.local" {
		t.Fatalf("hostname = %q, want %q", got, "relay-a.local")
	}
	if got := payload.Devices[0].IP; got != "192.168.1.50" {
		t.Fatalf("ip = %q, want %q", got, "192.168.1.50")
	}
	if got := payload.Devices[0].Port; got != 80 {
		t.Fatalf("port = %d, want 80", got)
	}
}

func TestDiscoverSupportsRobotJSONEnvelope(t *testing.T) {
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		params.Entries <- &mdns.ServiceEntry{
			Host:       "relay-b.local.",
			AddrV4:     net.ParseIP("192.168.1.51"),
			Port:       8080,
			InfoFields: []string{"fw_version=0.4.0", "board=esp32-evb"},
		}
		return nil
	})
	defer restore()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--robot", "--format", "json", "discover"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["command"]; got != "discover" {
		t.Fatalf("command = %#v, want %q", got, "discover")
	}
	if got := payload["exit_code"]; got != float64(0) {
		t.Fatalf("exit_code = %#v, want 0", got)
	}

	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("data = %#v; want object", payload["data"])
	}
	devices, ok := data["devices"].([]any)
	if !ok || len(devices) != 1 {
		t.Fatalf("devices = %#v; want one device", data["devices"])
	}
}

func stubMDNSQuery(t *testing.T, fn func(context.Context, *mdns.QueryParam) error) func() {
	t.Helper()

	previous := queryMDNS
	queryMDNS = fn
	return func() {
		queryMDNS = previous
	}
}

func TestDiscoverAllowsCustomTimeoutOverride(t *testing.T) {
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		if params.Timeout != 2*time.Second {
			t.Fatalf("timeout = %s, want %s", params.Timeout, 2*time.Second)
		}
		return nil
	})
	defer restore()

	command := newRootCommand()
	command.SetOut(&bytes.Buffer{})
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--timeout", "2s", "--format", "json", "discover"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
}
