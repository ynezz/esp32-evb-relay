package cmd

import (
	"errors"
	"fmt"
	"math"
	"net/http"
	"strconv"
	"time"

	"github.com/spf13/cobra"

	"example.com/esp32-evb-relay/cli/client"
	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
	"example.com/esp32-evb-relay/cli/internal/robot"
)

const statusCommandName = "status"

type deviceNetworkStatus struct {
	Hostname  string `json:"hostname"`
	Connected bool   `json:"connected"`
	Transport string `json:"transport"`
	IP        string `json:"ip"`
	Netmask   string `json:"netmask"`
	Gateway   string `json:"gateway"`
}

type deviceModIOStatus struct {
	Present bool   `json:"present"`
	Sync    string `json:"sync"`
}

type deviceStatus struct {
	UptimeSeconds   float64             `json:"uptime_seconds"`
	FirmwareVersion string              `json:"firmware_version"`
	FreeHeapBytes   float64             `json:"free_heap_bytes"`
	Network         deviceNetworkStatus `json:"network"`
	ModIO           deviceModIOStatus   `json:"modio"`
}

type statusResult struct {
	Status deviceStatus `json:"status"`
}

func newStatusCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "status",
		Short: "Show device health and system information",
		Args:  cobra.NoArgs,
		RunE:  runStatus,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: []string{"--host", "--api-token", "--format", "--timeout", "--robot"},
		OutputFields: []string{
			"status.uptime_seconds",
			"status.firmware_version",
			"status.free_heap_bytes",
			"status.network.hostname",
			"status.network.connected",
			"status.network.transport",
			"status.network.ip",
			"status.network.netmask",
			"status.network.gateway",
			"status.modio.present",
			"status.modio.sync",
		},
		Errors: []string{
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
		},
		Example: "evb-relay status",
	})

	return command
}

func runStatus(cmd *cobra.Command, _ []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	startedAt := time.Now()
	host := runtime.Host

	statusClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return wrapStatusResult(cmd, runtime, host, nil, nil, startedAt, err)
	}
	host = statusClient.Host()

	var payload statusResult
	result, err := statusClient.DoJSON(cmd.Context(), http.MethodGet, "/status", nil, &payload.Status)
	if err != nil {
		return wrapStatusResult(
			cmd,
			runtime,
			host,
			nil,
			robot.FromClientDeviceContext(result.DeviceContext),
			startedAt,
			err,
		)
	}

	return wrapStatusResult(
		cmd,
		runtime,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		nil,
	)
}

func wrapStatusResult(
	cmd *cobra.Command,
	runtime appconfig.Runtime,
	host string,
	payload *statusResult,
	deviceContext *robot.DeviceContext,
	startedAt time.Time,
	err error,
) error {
	elapsed := time.Since(startedAt)
	data := any(nil)
	if err == nil {
		data = payload
	}

	if runtime.Robot {
		return robot.Wrap(cmd, cmd.OutOrStdout(), robot.WrapOpts{
			Command:       statusCommandName,
			Data:          data,
			DeviceContext: deviceContext,
			Err:           err,
			Format:        runtime.Format,
			Host:          host,
			Elapsed:       elapsed,
		})
	}

	if err != nil {
		return err
	}

	return outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format)
}

func (r statusResult) TableOutput() (outputformat.TableData, error) {
	return outputformat.TableData{
		Headers: []string{
			"UPTIME (S)",
			"FW VERSION",
			"FREE HEAP (B)",
			"HOSTNAME",
			"CONNECTED",
			"TRANSPORT",
			"IP",
			"NETMASK",
			"GATEWAY",
			"MODIO PRESENT",
			"MODIO SYNC",
		},
		Rows: [][]string{{
			formatStatusNumber(r.Status.UptimeSeconds),
			r.Status.FirmwareVersion,
			formatStatusNumber(r.Status.FreeHeapBytes),
			r.Status.Network.Hostname,
			strconv.FormatBool(r.Status.Network.Connected),
			r.Status.Network.Transport,
			r.Status.Network.IP,
			r.Status.Network.Netmask,
			r.Status.Network.Gateway,
			strconv.FormatBool(r.Status.ModIO.Present),
			r.Status.ModIO.Sync,
		}},
	}, nil
}

func (r statusResult) PlainOutput() ([]string, error) {
	return []string{
		fmt.Sprintf("uptime_seconds=%s", formatStatusNumber(r.Status.UptimeSeconds)),
		fmt.Sprintf("firmware_version=%s", r.Status.FirmwareVersion),
		fmt.Sprintf("free_heap_bytes=%s", formatStatusNumber(r.Status.FreeHeapBytes)),
		fmt.Sprintf("network.hostname=%s", r.Status.Network.Hostname),
		fmt.Sprintf("network.connected=%t", r.Status.Network.Connected),
		fmt.Sprintf("network.transport=%s", r.Status.Network.Transport),
		fmt.Sprintf("network.ip=%s", r.Status.Network.IP),
		fmt.Sprintf("network.netmask=%s", r.Status.Network.Netmask),
		fmt.Sprintf("network.gateway=%s", r.Status.Network.Gateway),
		fmt.Sprintf("modio.present=%t", r.Status.ModIO.Present),
		fmt.Sprintf("modio.sync=%s", r.Status.ModIO.Sync),
	}, nil
}

func formatStatusNumber(value float64) string {
	if value == math.Trunc(value) {
		return strconv.FormatInt(int64(value), 10)
	}

	return strconv.FormatFloat(value, 'f', -1, 64)
}
