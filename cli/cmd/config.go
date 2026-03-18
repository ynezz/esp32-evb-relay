package cmd

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/cobra"

	"example.com/esp32-evb-relay/cli/client"
	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
	"example.com/esp32-evb-relay/cli/internal/robot"
)

const (
	configShowCommandName = "config show"
	configSetCommandName  = "config set"
)

type configSnapshot struct {
	PollIntervalMS  uint32 `json:"poll_interval_ms"`
	Hostname        string `json:"hostname"`
	ModIOBootPolicy string `json:"modio_boot_policy"`
	APITokenSet     bool   `json:"api_token_set"`
}

type configShowResult struct {
	Config configSnapshot `json:"config"`
}

type configChange struct {
	Key  string `json:"key"`
	Old  any    `json:"old"`
	New  any    `json:"new"`
	Live bool   `json:"live"`
}

type configSetResult struct {
	Changes         []configChange `json:"changes"`
	RestartRequired bool           `json:"restart_required"`
}

func newConfigCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "config",
		Short: "Read and update device configuration",
	}

	command.AddCommand(newConfigShowCommand(), newConfigSetCommand())
	return command
}

func newConfigShowCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "show",
		Short: "Read the current device configuration",
		Args:  cobra.NoArgs,
		RunE:  runConfigShow,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		OutputFields: []string{
			"config.poll_interval_ms",
			"config.hostname",
			"config.modio_boot_policy",
			"config.api_token_set",
		},
		Errors:  []string{"NETWORK_ERROR", "AUTH_REQUIRED", "AUTH_FORBIDDEN", "AUTH_INVALID"},
		Example: "evb-relay config show",
	})

	return command
}

func newConfigSetCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "set key=value [key=value...]",
		Short: "Update one or more device configuration fields",
		Args:  cobra.MinimumNArgs(1),
		RunE:  runConfigSet,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Args: []string{"key=value"},
		OutputFields: []string{
			"changes[].key",
			"changes[].old",
			"changes[].new",
			"changes[].live",
			"restart_required",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"AUTH_INVALID",
		},
		Example: "evb-relay config set poll_interval_ms=200 hostname=lab-relay",
	})

	return command
}

func runConfigShow(cmd *cobra.Command, _ []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	startedAt := time.Now()
	host := runtime.Host

	configClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return wrapConfigResult(cmd, runtime, configShowCommandName, host, nil, nil, startedAt, err, nil)
	}
	host = configClient.Host()

	var payload configShowResult
	result, err := configClient.DoJSON(cmd.Context(), http.MethodGet, "/config", nil, &payload)

	return wrapConfigResult(
		cmd,
		runtime,
		configShowCommandName,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
		nil,
	)
}

func runConfigSet(cmd *cobra.Command, args []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	startedAt := time.Now()
	host := runtime.Host

	requestBody, err := parseConfigAssignments(args)
	if err != nil {
		return wrapConfigResult(cmd, runtime, configSetCommandName, host, nil, nil, startedAt, err, nil)
	}

	configClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return wrapConfigResult(cmd, runtime, configSetCommandName, host, nil, nil, startedAt, err, nil)
	}
	host = configClient.Host()

	var payload configSetResult
	result, err := configClient.DoJSON(cmd.Context(), http.MethodPut, "/config", requestBody, &payload)

	warnings := []string(nil)
	if err == nil && payload.RestartRequired {
		warnings = []string{"Restart the device for all changes to take full effect."}
	}

	return wrapConfigResult(
		cmd,
		runtime,
		configSetCommandName,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
		warnings,
	)
}

func wrapConfigResult(
	cmd *cobra.Command,
	runtime appconfig.Runtime,
	commandName string,
	host string,
	payload any,
	deviceContext *robot.DeviceContext,
	startedAt time.Time,
	err error,
	warnings []string,
) error {
	elapsed := time.Since(startedAt)
	data := payload
	if err != nil {
		data = nil
	}

	if runtime.Robot {
		return robot.Wrap(cmd, cmd.OutOrStdout(), robot.WrapOpts{
			Command:       commandName,
			Data:          data,
			DeviceContext: deviceContext,
			Err:           err,
			Format:        runtime.Format,
			Host:          host,
			Elapsed:       elapsed,
			Warnings:      warnings,
		})
	}

	if err != nil {
		return err
	}

	for _, warning := range warnings {
		_, _ = fmt.Fprintln(cmd.ErrOrStderr(), warning)
	}

	return outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format)
}

type configValueParser func(raw string) (any, error)

func parseConfigAssignments(values []string) (map[string]any, error) {
	if len(values) == 0 {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, errors.New("at least one key=value assignment is required"))
	}

	parsers := configAssignmentParsers()
	requestBody := make(map[string]any, len(values))

	for _, assignment := range values {
		key, rawValue, found := strings.Cut(assignment, "=")
		if !found {
			return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("invalid assignment %q: expected key=value", assignment))
		}

		key = strings.TrimSpace(key)
		parser, ok := parsers[key]
		if !ok {
			return nil, exitcodes.Wrap(
				exitcodes.BadArgument,
				fmt.Errorf("unsupported config key %q (supported: %s)", key, strings.Join(supportedConfigKeys(), ", ")),
			)
		}
		if _, exists := requestBody[key]; exists {
			return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("duplicate config key %q", key))
		}

		parsedValue, err := parser(rawValue)
		if err != nil {
			return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("invalid value for %q: %w", key, err))
		}
		requestBody[key] = parsedValue
	}

	return requestBody, nil
}

func configAssignmentParsers() map[string]configValueParser {
	return map[string]configValueParser{
		"api_token": func(raw string) (any, error) {
			return raw, nil
		},
		"hostname": func(raw string) (any, error) {
			trimmed := strings.TrimSpace(raw)
			if trimmed == "" {
				return nil, errors.New("hostname must not be empty")
			}
			return trimmed, nil
		},
		"modio_boot_policy": func(raw string) (any, error) {
			normalized := strings.ToLower(strings.TrimSpace(raw))
			switch normalized {
			case "leave_unchanged", "all_off":
				return normalized, nil
			default:
				return nil, errors.New("expected leave_unchanged or all_off")
			}
		},
		"poll_interval_ms": func(raw string) (any, error) {
			value, err := strconv.ParseUint(strings.TrimSpace(raw), 10, 32)
			if err != nil {
				return nil, errors.New("expected an unsigned integer")
			}
			if value == 0 {
				return nil, errors.New("must be greater than zero")
			}
			return uint32(value), nil
		},
	}
}

func supportedConfigKeys() []string {
	keys := make([]string, 0, len(configAssignmentParsers()))
	for key := range configAssignmentParsers() {
		keys = append(keys, key)
	}
	slices.Sort(keys)
	return keys
}

func (r configShowResult) TableOutput() (outputformat.TableData, error) {
	return outputformat.TableData{
		Headers: []string{"POLL INTERVAL (MS)", "HOSTNAME", "MODIO BOOT POLICY", "API TOKEN SET"},
		Rows: [][]string{{
			fmt.Sprintf("%d", r.Config.PollIntervalMS),
			r.Config.Hostname,
			r.Config.ModIOBootPolicy,
			strconv.FormatBool(r.Config.APITokenSet),
		}},
	}, nil
}

func (r configShowResult) PlainOutput() ([]string, error) {
	return []string{
		fmt.Sprintf("poll_interval_ms=%d", r.Config.PollIntervalMS),
		fmt.Sprintf("hostname=%s", r.Config.Hostname),
		fmt.Sprintf("modio_boot_policy=%s", r.Config.ModIOBootPolicy),
		fmt.Sprintf("api_token_set=%t", r.Config.APITokenSet),
	}, nil
}

func (r configSetResult) TableOutput() (outputformat.TableData, error) {
	rows := make([][]string, 0, len(r.Changes))
	for _, change := range r.Changes {
		rows = append(rows, []string{
			change.Key,
			formatConfigValue(change.Old),
			formatConfigValue(change.New),
			strconv.FormatBool(change.Live),
		})
	}

	return outputformat.TableData{
		Headers: []string{"KEY", "OLD", "NEW", "LIVE"},
		Rows:    rows,
	}, nil
}

func (r configSetResult) PlainOutput() ([]string, error) {
	lines := make([]string, 0, len(r.Changes)+1)
	for _, change := range r.Changes {
		lines = append(lines, fmt.Sprintf(
			"%s: %s -> %s (live=%t)",
			change.Key,
			formatConfigValue(change.Old),
			formatConfigValue(change.New),
			change.Live,
		))
	}
	lines = append(lines, fmt.Sprintf("restart_required=%t", r.RestartRequired))
	return lines, nil
}

func formatConfigValue(value any) string {
	switch typed := value.(type) {
	case nil:
		return "-"
	case string:
		if typed == "" {
			return `""`
		}
		return typed
	case bool:
		return strconv.FormatBool(typed)
	case float64:
		if typed == math.Trunc(typed) {
			return strconv.FormatInt(int64(typed), 10)
		}
		return strconv.FormatFloat(typed, 'f', -1, 64)
	default:
		encoded, err := json.Marshal(typed)
		if err != nil {
			return fmt.Sprintf("%v", typed)
		}
		return string(encoded)
	}
}
