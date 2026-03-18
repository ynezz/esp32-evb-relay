package cmd

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"sort"
	"strings"
	"time"

	"github.com/hashicorp/mdns"
	"github.com/spf13/cobra"

	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
	"example.com/esp32-evb-relay/cli/internal/robot"
)

const (
	discoverServiceName    = "_http._tcp"
	discoverBoardType      = "esp32-evb"
	discoverDefaultTimeout = 4 * time.Second
)

var queryMDNS = mdns.QueryContext

type discoverDevice struct {
	Hostname string   `json:"hostname"`
	IP       string   `json:"ip"`
	Port     int      `json:"port"`
	TXT      []string `json:"txt"`
}

type discoverResult struct {
	Devices []discoverDevice `json:"devices"`
}

func newDiscoverCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "discover",
		Short: "Browse for EVB relay devices via mDNS",
		Args:  cobra.NoArgs,
		RunE:  runDiscover,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		OutputFields: []string{
			"devices[].hostname",
			"devices[].ip",
			"devices[].port",
			"devices[].txt",
		},
		Errors:  []string{"NETWORK_ERROR"},
		Example: "evb-relay discover",
	})

	return command
}

func runDiscover(cmd *cobra.Command, _ []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	timeout := runtime.Timeout
	if timeout == appconfig.DefaultTimeout {
		timeout = discoverDefaultTimeout
	}

	startedAt := time.Now()
	result, err := discoverDevices(cmd.Context(), timeout)
	elapsed := time.Since(startedAt)

	if runtime.Robot {
		return robot.Wrap(cmd, cmd.OutOrStdout(), robot.WrapOpts{
			Data:    result,
			Err:     err,
			Format:  runtime.Format,
			Elapsed: elapsed,
		})
	}
	if err != nil {
		return err
	}

	return outputformat.Output(cmd.OutOrStdout(), result, runtime.Format)
}

func discoverDevices(ctx context.Context, timeout time.Duration) (discoverResult, error) {
	if timeout <= 0 {
		return discoverResult{}, exitcodes.Wrap(exitcodes.BadArgument, errors.New("timeout must be greater than zero"))
	}

	queryCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	entries := make(chan *mdns.ServiceEntry, 32)
	params := mdns.DefaultParams(discoverServiceName)
	params.Timeout = timeout
	params.Entries = entries
	params.Logger = log.New(io.Discard, "", 0)

	errCh := make(chan error, 1)
	go func() {
		errCh <- queryMDNS(queryCtx, params)
		close(entries)
	}()

	result := discoverResult{
		Devices: collectDiscoverDevices(entries),
	}

	return result, <-errCh
}

func collectDiscoverDevices(entries <-chan *mdns.ServiceEntry) []discoverDevice {
	devicesByKey := make(map[string]discoverDevice)

	for entry := range entries {
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
