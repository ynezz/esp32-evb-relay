package cmd

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/hashicorp/mdns"
	"github.com/spf13/cobra"

	appconfig "github.com/ynezz/esp32-evb-relay/cli/internal/config"
	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "github.com/ynezz/esp32-evb-relay/cli/internal/format"
	"github.com/ynezz/esp32-evb-relay/cli/internal/robot"
)

const (
	discoverServiceName    = "_http._tcp"
	discoverBoardType      = "esp32-evb"
	discoverDefaultTimeout = 4 * time.Second
)

var queryMDNS = mdns.QueryContext

// listInterfaces is overridden in tests to avoid depending on the host's
// real network interfaces.
var listInterfaces = net.Interfaces

var discoverNoResultsNext = []string{
	"Check the device serial console or DHCP lease table for the IP address.",
	"Retry discovery from a network segment that forwards mDNS multicast.",
}

type discoverDevice struct {
	Hostname string   `json:"hostname"`
	IP       string   `json:"ip"`
	Port     int      `json:"port"`
	TXT      []string `json:"txt"`
}

type discoverInterfaceError struct {
	Interface string `json:"interface"`
	Error     string `json:"error"`
}

type discoverResult struct {
	Devices         []discoverDevice         `json:"devices"`
	InterfaceErrors []discoverInterfaceError `json:"interface_errors,omitempty"`
}

func newDiscoverCommand() *cobra.Command {
	var interfaceName string

	command := &cobra.Command{
		Use:   "discover",
		Short: "Browse for EVB relay devices via mDNS",
		Args:  cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runDiscover(cmd, args, interfaceName)
		},
	}

	command.Flags().StringVar(&interfaceName, "interface", "", "Query only this network interface instead of every up, multicast-capable interface")

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: []string{"--format", "--timeout", "--robot", "--interface"},
		OutputFields: []string{
			"devices[].hostname",
			"devices[].ip",
			"devices[].port",
			"devices[].txt",
			"interface_errors[].interface",
			"interface_errors[].error",
		},
		Errors:  []string{"NETWORK_ERROR", "BAD_ARGUMENT"},
		Example: "evb-relay discover",
	})

	return command
}

func runDiscover(cmd *cobra.Command, _ []string, interfaceName string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	timeout := runtime.Timeout
	if timeout == appconfig.DefaultTimeout {
		timeout = discoverDefaultTimeout
	}

	startedAt := time.Now()
	result, err := discoverDevices(cmd.Context(), timeout, interfaceName)
	elapsed := time.Since(startedAt)
	warnings, next := discoverAdvice(result, err)

	if runtime.Robot {
		return robot.Wrap(cmd, cmd.OutOrStdout(), robot.WrapOpts{
			Data:     result,
			Err:      err,
			Format:   runtime.Format,
			Elapsed:  elapsed,
			Warnings: warnings,
			Next:     next,
		})
	}
	if err != nil {
		return err
	}

	for _, warning := range warnings {
		_, _ = fmt.Fprintln(cmd.ErrOrStderr(), warning)
	}
	for _, step := range next {
		_, _ = fmt.Fprintln(cmd.ErrOrStderr(), step)
	}

	return outputformat.Output(cmd.OutOrStdout(), result, runtime.Format)
}

func discoverAdvice(result discoverResult, err error) ([]string, []string) {
	var warnings []string
	for _, interfaceError := range result.InterfaceErrors {
		warnings = append(warnings, fmt.Sprintf(
			"mDNS query failed on interface %s: %s", interfaceError.Interface, interfaceError.Error,
		))
	}

	if err != nil || len(result.Devices) > 0 {
		return warnings, nil
	}

	warnings = append(warnings, "No EVB relay devices were discovered via mDNS on this network segment.")
	return warnings, append([]string(nil), discoverNoResultsNext...)
}

// discoverInterfaces returns the network interfaces mDNS queries should run
// on. When interfaceName is non-empty, only that interface is used
// (regardless of its flags). Otherwise every up, multicast-capable,
// non-loopback interface is returned.
func discoverInterfaces(interfaceName string) ([]net.Interface, error) {
	all, err := listInterfaces()
	if err != nil {
		return nil, fmt.Errorf("list network interfaces: %w", err)
	}

	interfaceName = strings.TrimSpace(interfaceName)
	if interfaceName != "" {
		for _, iface := range all {
			if iface.Name == interfaceName {
				return []net.Interface{iface}, nil
			}
		}
		return nil, fmt.Errorf("interface %q not found", interfaceName)
	}

	usable := make([]net.Interface, 0, len(all))
	for _, iface := range all {
		if isDiscoverableInterface(iface) {
			usable = append(usable, iface)
		}
	}

	return usable, nil
}

func isDiscoverableInterface(iface net.Interface) bool {
	if iface.Flags&net.FlagUp == 0 {
		return false
	}
	if iface.Flags&net.FlagLoopback != 0 {
		return false
	}
	if iface.Flags&net.FlagMulticast == 0 {
		return false
	}
	return true
}

type discoverQueryOutcome struct {
	interfaceName string
	entries       []*mdns.ServiceEntry
	err           error
}

func discoverDevices(ctx context.Context, timeout time.Duration, interfaceName string) (discoverResult, error) {
	if timeout <= 0 {
		return discoverResult{}, exitcodes.Wrap(exitcodes.BadArgument, errors.New("timeout must be greater than zero"))
	}

	interfaces, err := discoverInterfaces(interfaceName)
	if err != nil {
		return discoverResult{}, exitcodes.Wrap(exitcodes.BadArgument, err)
	}
	if len(interfaces) == 0 {
		return discoverResult{}, exitcodes.Wrap(exitcodes.NetworkError, errors.New(
			"no up, multicast-capable, non-loopback network interfaces found; use --interface to target one explicitly",
		))
	}

	queryCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	outcomes := make(chan discoverQueryOutcome, len(interfaces))
	var wg sync.WaitGroup
	for _, iface := range interfaces {
		wg.Add(1)
		go func(iface net.Interface) {
			defer wg.Done()
			outcomes <- queryDiscoverInterface(queryCtx, iface, timeout)
		}(iface)
	}

	go func() {
		wg.Wait()
		close(outcomes)
	}()

	var allEntries []*mdns.ServiceEntry
	var interfaceErrors []discoverInterfaceError
	for outcome := range outcomes {
		if outcome.err != nil {
			interfaceErrors = append(interfaceErrors, discoverInterfaceError{
				Interface: outcome.interfaceName,
				Error:     outcome.err.Error(),
			})
			continue
		}
		allEntries = append(allEntries, outcome.entries...)
	}

	sort.Slice(interfaceErrors, func(left, right int) bool {
		return interfaceErrors[left].Interface < interfaceErrors[right].Interface
	})

	result := discoverResult{
		Devices:         collectDiscoverDevices(allEntries),
		InterfaceErrors: interfaceErrors,
	}

	if len(interfaceErrors) == len(interfaces) {
		return result, exitcodes.Wrap(exitcodes.NetworkError, fmt.Errorf(
			"mdns query failed on all %d interface(s): %s",
			len(interfaces), summarizeInterfaceErrors(interfaceErrors),
		))
	}

	return result, nil
}

func queryDiscoverInterface(ctx context.Context, iface net.Interface, timeout time.Duration) discoverQueryOutcome {
	entries := make(chan *mdns.ServiceEntry, 32)
	params := mdns.DefaultParams(discoverServiceName)
	params.Timeout = timeout
	params.Entries = entries
	params.Logger = log.New(io.Discard, "", 0)
	params.Interface = &iface

	collected := make([]*mdns.ServiceEntry, 0)
	done := make(chan struct{})
	go func() {
		for entry := range entries {
			collected = append(collected, entry)
		}
		close(done)
	}()

	err := queryMDNS(ctx, params)
	close(entries)
	<-done

	return discoverQueryOutcome{interfaceName: iface.Name, entries: collected, err: err}
}

func summarizeInterfaceErrors(errs []discoverInterfaceError) string {
	parts := make([]string, 0, len(errs))
	for _, current := range errs {
		parts = append(parts, fmt.Sprintf("%s: %s", current.Interface, current.Error))
	}
	return strings.Join(parts, "; ")
}

func collectDiscoverDevices(entries []*mdns.ServiceEntry) []discoverDevice {
	devicesByKey := make(map[string]discoverDevice)

	for _, entry := range entries {
		device, ok := discoverDeviceFromEntry(entry)
		if !ok {
			continue
		}

		key := fmt.Sprintf("%s|%s|%d", device.Hostname, device.IP, device.Port)
		devicesByKey[key] = device
	}

	devices := make([]discoverDevice, 0, len(devicesByKey))
	for _, device := range devicesByKey {
		devices = append(devices, device)
	}

	sort.Slice(devices, func(left, right int) bool {
		if devices[left].Hostname != devices[right].Hostname {
			return devices[left].Hostname < devices[right].Hostname
		}
		if devices[left].IP != devices[right].IP {
			return devices[left].IP < devices[right].IP
		}
		return devices[left].Port < devices[right].Port
	})

	return devices
}

func discoverDeviceFromEntry(entry *mdns.ServiceEntry) (discoverDevice, bool) {
	if entry == nil {
		return discoverDevice{}, false
	}

	txt := append([]string(nil), entry.InfoFields...)
	if !hasTXTValue(txt, "board", discoverBoardType) {
		return discoverDevice{}, false
	}

	host := strings.TrimSuffix(entry.Host, ".")
	if host == "" {
		host = strings.TrimSuffix(entry.Name, ".")
	}

	ip := serviceEntryIP(entry)
	if host == "" || ip == "" || entry.Port == 0 {
		return discoverDevice{}, false
	}

	return discoverDevice{
		Hostname: host,
		IP:       ip,
		Port:     entry.Port,
		TXT:      txt,
	}, true
}

func serviceEntryIP(entry *mdns.ServiceEntry) string {
	switch {
	case entry.AddrV4 != nil:
		return entry.AddrV4.String()
	case entry.AddrV6IPAddr != nil && entry.AddrV6IPAddr.IP != nil:
		return entry.AddrV6IPAddr.IP.String()
	case entry.AddrV6 != nil:
		return entry.AddrV6.String()
	case entry.Addr != nil:
		return entry.Addr.String()
	default:
		return ""
	}
}

func hasTXTValue(values []string, key string, want string) bool {
	prefix := key + "="
	for _, current := range values {
		if strings.HasPrefix(current, prefix) && strings.EqualFold(strings.TrimPrefix(current, prefix), want) {
			return true
		}
	}
	return false
}

func (r discoverResult) TableOutput() (outputformat.TableData, error) {
	rows := make([][]string, 0, len(r.Devices))
	for _, device := range r.Devices {
		rows = append(rows, []string{
			device.Hostname,
			device.IP,
			fmt.Sprintf("%d", device.Port),
			strings.Join(device.TXT, ", "),
		})
	}

	return outputformat.TableData{
		Headers: []string{"HOSTNAME", "IP", "PORT", "TXT"},
		Rows:    rows,
	}, nil
}

func (r discoverResult) PlainOutput() ([]string, error) {
	lines := make([]string, 0, len(r.Devices))
	for _, device := range r.Devices {
		line := fmt.Sprintf("%s %s:%d", device.Hostname, device.IP, device.Port)
		if len(device.TXT) > 0 {
			line += " " + strings.Join(device.TXT, " ")
		}
		lines = append(lines, line)
	}

	return lines, nil
}
