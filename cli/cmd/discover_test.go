package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/hashicorp/mdns"

	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
)

// defaultTestInterface is the single fake up/multicast-capable interface
// used by tests that don't care about multi-interface behavior.
var defaultTestInterface = net.Interface{Name: "eth-test", Flags: net.FlagUp | net.FlagMulticast}

func stubMDNSQuery(t *testing.T, fn func(context.Context, *mdns.QueryParam) error) func() {
	t.Helper()

	previous := queryMDNS
	queryMDNS = fn
	return func() {
		queryMDNS = previous
	}
}

func stubInterfaces(t *testing.T, interfaces []net.Interface) func() {
	t.Helper()

	previous := listInterfaces
	listInterfaces = func() ([]net.Interface, error) {
		return interfaces, nil
	}
	return func() {
		listInterfaces = previous
	}
}

func TestDiscoverOutputsJSONForMatchingBoardDevices(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{defaultTestInterface})()
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
	defer stubInterfaces(t, []net.Interface{defaultTestInterface})()
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

func TestDiscoverAllowsCustomTimeoutOverride(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{defaultTestInterface})()
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

func TestDiscoverWarnsWhenNoDevicesAreFound(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{defaultTestInterface})()
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		return nil
	})
	defer restore()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{"--format", "json", "discover"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload struct {
		Devices []any `json:"devices"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if len(payload.Devices) != 0 {
		t.Fatalf("device count = %d, want 0", len(payload.Devices))
	}

	warning := "No EVB relay devices were discovered via mDNS on this network segment."
	if got := stderr.String(); !bytes.Contains([]byte(got), []byte(warning)) {
		t.Fatalf("stderr = %q, want warning %q", got, warning)
	}
	if got := stderr.String(); !bytes.Contains([]byte(got), []byte(discoverNoResultsNext[0])) {
		t.Fatalf("stderr = %q, want fallback hint %q", got, discoverNoResultsNext[0])
	}
}

func TestDiscoverRobotIncludesWarningWhenNoDevicesAreFound(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{defaultTestInterface})()
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
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

	warnings, ok := payload["warnings"].([]any)
	if !ok || len(warnings) != 1 {
		t.Fatalf("warnings = %#v, want one warning", payload["warnings"])
	}
	if got := warnings[0]; got != "No EVB relay devices were discovered via mDNS on this network segment." {
		t.Fatalf("warning = %#v, want no-devices warning", got)
	}

	next, ok := payload["next"].([]any)
	if !ok || len(next) != len(discoverNoResultsNext) {
		t.Fatalf("next = %#v, want %#v", payload["next"], discoverNoResultsNext)
	}
	for index, want := range discoverNoResultsNext {
		if got := next[index]; got != want {
			t.Fatalf("next[%d] = %#v, want %q", index, got, want)
		}
	}
}

func TestDiscoverInterfacesFiltersDownLoopbackAndNonMulticastInterfaces(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "lo", Flags: net.FlagUp | net.FlagLoopback | net.FlagMulticast},
		{Name: "wg0", Flags: net.FlagUp},
		{Name: "down0", Flags: net.FlagMulticast},
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "docker0", Flags: net.FlagUp | net.FlagMulticast | net.FlagBroadcast},
	})()

	got, err := discoverInterfaces("")
	if err != nil {
		t.Fatalf("discoverInterfaces() error = %v", err)
	}

	var names []string
	for _, iface := range got {
		names = append(names, iface.Name)
	}

	want := []string{"eth0", "docker0"}
	if len(names) != len(want) {
		t.Fatalf("interfaces = %v, want %v", names, want)
	}
	for index, name := range want {
		if names[index] != name {
			t.Fatalf("interfaces = %v, want %v", names, want)
		}
	}
}

func TestDiscoverInterfaceOverrideQueriesOnlyNamedInterface(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "wlan0", Flags: net.FlagUp | net.FlagMulticast},
	})()

	var calls []string
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		calls = append(calls, params.Interface.Name)
		return nil
	})
	defer restore()

	command := newRootCommand()
	command.SetOut(&bytes.Buffer{})
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--format", "json", "discover", "--interface", "wlan0"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	if len(calls) != 1 || calls[0] != "wlan0" {
		t.Fatalf("mdns queried interfaces = %v, want [wlan0]", calls)
	}
}

func TestDiscoverInterfaceOverrideErrorsWhenInterfaceMissing(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
	})()
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		t.Fatal("mdns query should not run when the requested interface is missing")
		return nil
	})
	defer restore()

	command := newRootCommand()
	command.SetOut(&bytes.Buffer{})
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--format", "json", "discover", "--interface", "does-not-exist"})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() error = nil, want an error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.BadArgument {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.BadArgument)
	}
}

func TestDiscoverErrorsWhenNoUsableInterfacesExist(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "lo", Flags: net.FlagUp | net.FlagLoopback | net.FlagMulticast},
	})()
	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		t.Fatal("mdns query should not run when no interfaces are usable")
		return nil
	})
	defer restore()

	command := newRootCommand()
	command.SetOut(&bytes.Buffer{})
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--format", "json", "discover"})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() error = nil, want an error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.NetworkError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.NetworkError)
	}
}

func TestDiscoverMergesAndDedupesResultsAcrossInterfaces(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "eth1", Flags: net.FlagUp | net.FlagMulticast},
	})()

	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		switch params.Interface.Name {
		case "eth0":
			params.Entries <- &mdns.ServiceEntry{
				Host:       "relay-a.local.",
				AddrV4:     net.ParseIP("192.168.1.50"),
				Port:       80,
				InfoFields: []string{"board=esp32-evb"},
			}
		case "eth1":
			// Same device, reachable on a second interface: must dedupe.
			params.Entries <- &mdns.ServiceEntry{
				Host:       "relay-a.local.",
				AddrV4:     net.ParseIP("192.168.1.50"),
				Port:       80,
				InfoFields: []string{"board=esp32-evb"},
			}
			params.Entries <- &mdns.ServiceEntry{
				Host:       "relay-b.local.",
				AddrV4:     net.ParseIP("192.168.1.51"),
				Port:       80,
				InfoFields: []string{"board=esp32-evb"},
			}
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
			Hostname string `json:"hostname"`
		} `json:"devices"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if len(payload.Devices) != 2 {
		t.Fatalf("device count = %d, want 2 (deduped): %#v", len(payload.Devices), payload.Devices)
	}
}

func TestDiscoverSurfacesPerInterfaceErrorsWithoutFailingTheRun(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "eth1", Flags: net.FlagUp | net.FlagMulticast},
	})()

	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		if params.Interface.Name == "eth1" {
			return errors.New("bind: permission denied")
		}
		params.Entries <- &mdns.ServiceEntry{
			Host:       "relay-a.local.",
			AddrV4:     net.ParseIP("192.168.1.50"),
			Port:       80,
			InfoFields: []string{"board=esp32-evb"},
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
		t.Fatalf("Execute() error = %v, want nil (one good interface should keep the run successful)", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
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

	interfaceErrors, ok := data["interface_errors"].([]any)
	if !ok || len(interfaceErrors) != 1 {
		t.Fatalf("interface_errors = %#v; want one entry", data["interface_errors"])
	}
	entry, ok := interfaceErrors[0].(map[string]any)
	if !ok || entry["interface"] != "eth1" {
		t.Fatalf("interface_errors[0] = %#v; want interface eth1", interfaceErrors[0])
	}

	warnings, ok := payload["warnings"].([]any)
	if !ok || len(warnings) == 0 {
		t.Fatalf("warnings = %#v, want a per-interface warning", payload["warnings"])
	}
}

func TestDiscoverHumanOutputSuppressesInterfaceErrorsWhenDeviceFound(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "virbr0", Flags: net.FlagUp | net.FlagMulticast},
	})()

	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		if params.Interface.Name == "virbr0" {
			return errors.New("sendto: network is unreachable")
		}
		params.Entries <- &mdns.ServiceEntry{
			Host:       "relay-a.local.",
			AddrV4:     net.ParseIP("192.168.1.50"),
			Port:       80,
			InfoFields: []string{"board=esp32-evb"},
		}
		return nil
	})
	defer restore()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{"--format", "json", "discover"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	if got := stderr.String(); strings.Contains(got, "mDNS query failed on interface") {
		t.Fatalf("stderr = %q, want no per-interface failure noise once a device was found", got)
	}
}

func TestDiscoverHumanOutputShowsInterfaceErrorsWhenNoDeviceFound(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "virbr0", Flags: net.FlagUp | net.FlagMulticast},
	})()

	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		if params.Interface.Name == "virbr0" {
			return errors.New("sendto: network is unreachable")
		}
		return nil
	})
	defer restore()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(stderr)
	command.SetArgs([]string{"--format", "json", "discover"})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	want := "mDNS query failed on interface virbr0: sendto: network is unreachable"
	if got := stderr.String(); !strings.Contains(got, want) {
		t.Fatalf("stderr = %q, want it to contain %q when no device was found", got, want)
	}
}

func TestDiscoverRobotOutputKeepsInterfaceErrorsWhenDeviceFound(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "virbr0", Flags: net.FlagUp | net.FlagMulticast},
	})()

	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		if params.Interface.Name == "virbr0" {
			return errors.New("sendto: network is unreachable")
		}
		params.Entries <- &mdns.ServiceEntry{
			Host:       "relay-a.local.",
			AddrV4:     net.ParseIP("192.168.1.50"),
			Port:       80,
			InfoFields: []string{"board=esp32-evb"},
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

	warnings, ok := payload["warnings"].([]any)
	if !ok || len(warnings) != 1 {
		t.Fatalf("warnings = %#v, want the per-interface warning to survive in robot mode", payload["warnings"])
	}

	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("data = %#v; want object", payload["data"])
	}
	interfaceErrors, ok := data["interface_errors"].([]any)
	if !ok || len(interfaceErrors) != 1 {
		t.Fatalf("interface_errors = %#v; want one entry", data["interface_errors"])
	}
}

func TestDiscoverFailsWhenAllInterfacesError(t *testing.T) {
	defer stubInterfaces(t, []net.Interface{
		{Name: "eth0", Flags: net.FlagUp | net.FlagMulticast},
		{Name: "eth1", Flags: net.FlagUp | net.FlagMulticast},
	})()

	restore := stubMDNSQuery(t, func(_ context.Context, params *mdns.QueryParam) error {
		return errors.New("bind: permission denied")
	})
	defer restore()

	command := newRootCommand()
	command.SetOut(&bytes.Buffer{})
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{"--format", "json", "discover"})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() error = nil, want an error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.NetworkError {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.NetworkError)
	}
}
