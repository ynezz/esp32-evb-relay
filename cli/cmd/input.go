package cmd

import (
	"context"
	"errors"
	"fmt"
	"io"
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

const (
	streamReconnectDelay    = 250 * time.Millisecond
	inputDigitalCommandName = "input digital"
	inputAnalogCommandName  = "input analog"
	modioInputCount         = 4
)

var inputCommandFlags = []string{"--host", "--api-token", "--format", "--timeout", "--robot"}

type inputMetadata struct {
	SampleTSMS     uint64 `json:"sample_ts_ms"`
	SampleAgeMS    uint64 `json:"sample_age_ms"`
	PollIntervalMS uint32 `json:"poll_interval_ms"`
}

type apiInputMetadata struct {
	SampleTSMS     uint64 `json:"sample_ts_ms"`
	StalenessMS    uint64 `json:"staleness_ms"`
	PollIntervalMS uint32 `json:"poll_interval_ms"`
}

type digitalInputValue struct {
	ID    int  `json:"id"`
	State bool `json:"state"`
}

type analogInputValue struct {
	ID    int `json:"id"`
	Value int `json:"value"`
}

type digitalInputsResult struct {
	inputMetadata
	Inputs []digitalInputValue `json:"inputs"`
}

type digitalInputResult struct {
	inputMetadata
	Input digitalInputValue `json:"input"`
}

type analogInputsResult struct {
	inputMetadata
	Inputs []analogInputValue `json:"inputs"`
}

type analogInputResult struct {
	inputMetadata
	Input analogInputValue `json:"input"`
}

type apiDigitalInputsResponse struct {
	apiInputMetadata
	Inputs []digitalInputValue `json:"inputs"`
}

type apiDigitalInputResponse struct {
	apiInputMetadata
	Input digitalInputValue `json:"input"`
}

type apiAnalogInputsResponse struct {
	apiInputMetadata
	Inputs []analogInputValue `json:"inputs"`
}

type apiAnalogInputResponse struct {
	apiInputMetadata
	Input analogInputValue `json:"input"`
}

func newInputCommand() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "input",
		Short: "Read device inputs",
	}

	cmd.AddCommand(
		newInputDigitalCommand(),
		newInputAnalogCommand(),
		newInputWatchCommand(),
	)
	return cmd
}

func newInputDigitalCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "digital [id]",
		Short: "Read the latest digital input snapshot",
		Args:  cobra.MaximumNArgs(1),
		RunE:  runInputDigital,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: inputCommandFlags,
		Args:  []string{"id"},
		OutputFields: []string{
			"inputs[].id",
			"inputs[].state",
			"input.id",
			"input.state",
			"sample_ts_ms",
			"sample_age_ms",
			"poll_interval_ms",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"AUTH_INVALID",
			"INPUT_NOT_FOUND",
			"MODIO_NOT_PRESENT",
			"MODIO_SAMPLE_UNAVAILABLE",
		},
		Example: "evb-relay input digital 2",
	})

	return command
}

func newInputAnalogCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "analog [id]",
		Short: "Read the latest analog input snapshot",
		Args:  cobra.MaximumNArgs(1),
		RunE:  runInputAnalog,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: inputCommandFlags,
		Args:  []string{"id"},
		OutputFields: []string{
			"inputs[].id",
			"inputs[].value",
			"input.id",
			"input.value",
			"sample_ts_ms",
			"sample_age_ms",
			"poll_interval_ms",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"AUTH_INVALID",
			"INPUT_NOT_FOUND",
			"MODIO_NOT_PRESENT",
			"MODIO_SAMPLE_UNAVAILABLE",
		},
		Example: "evb-relay input analog",
	})

	return command
}

func newInputWatchCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "watch",
		Short: "Watch the device event stream",
		RunE:  runInputWatch,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: []string{"--host", "--api-token", "--timeout", "--robot"},
		OutputFields: []string{
			"stream",
			"started_at",
			"device_context",
			"event",
			"data",
			"reason",
			"received_at",
		},
		Errors:  []string{"NETWORK_ERROR", "AUTH_REQUIRED", "AUTH_FORBIDDEN", "AUTH_INVALID"},
		Example: "evb-relay --robot input watch",
	})

	return command
}

func runInputDigital(cmd *cobra.Command, args []string) error {
	runtime, inputClient, host, startedAt, err := newInputRuntime(cmd)
	if err != nil {
		return wrapInputResult(cmd, runtime, inputDigitalCommandName, host, nil, nil, startedAt, err)
	}

	if len(args) == 0 {
		var apiPayload apiDigitalInputsResponse
		result, err := inputClient.DoJSON(cmd.Context(), http.MethodGet, "/inputs/digital", nil, &apiPayload)
		payload := apiPayload.toResult()
		return wrapInputResult(
			cmd,
			runtime,
			inputDigitalCommandName,
			host,
			&payload,
			robot.FromClientDeviceContext(result.DeviceContext),
			startedAt,
			err,
		)
	}

	inputID, err := parseInputID(args[0])
	if err != nil {
		return wrapInputResult(cmd, runtime, inputDigitalCommandName, host, nil, nil, startedAt, err)
	}

	var apiPayload apiDigitalInputResponse
	result, err := inputClient.DoJSON(
		cmd.Context(),
		http.MethodGet,
		fmt.Sprintf("/inputs/digital/%d", inputID),
		nil,
		&apiPayload,
	)
	payload := apiPayload.toResult()
	return wrapInputResult(
		cmd,
		runtime,
		inputDigitalCommandName,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
	)
}

func runInputAnalog(cmd *cobra.Command, args []string) error {
	runtime, inputClient, host, startedAt, err := newInputRuntime(cmd)
	if err != nil {
		return wrapInputResult(cmd, runtime, inputAnalogCommandName, host, nil, nil, startedAt, err)
	}

	if len(args) == 0 {
		var apiPayload apiAnalogInputsResponse
		result, err := inputClient.DoJSON(cmd.Context(), http.MethodGet, "/inputs/analog", nil, &apiPayload)
		payload := apiPayload.toResult()
		return wrapInputResult(
			cmd,
			runtime,
			inputAnalogCommandName,
			host,
			&payload,
			robot.FromClientDeviceContext(result.DeviceContext),
			startedAt,
			err,
		)
	}

	inputID, err := parseInputID(args[0])
	if err != nil {
		return wrapInputResult(cmd, runtime, inputAnalogCommandName, host, nil, nil, startedAt, err)
	}

	var apiPayload apiAnalogInputResponse
	result, err := inputClient.DoJSON(
		cmd.Context(),
		http.MethodGet,
		fmt.Sprintf("/inputs/analog/%d", inputID),
		nil,
		&apiPayload,
	)
	payload := apiPayload.toResult()
	return wrapInputResult(
		cmd,
		runtime,
		inputAnalogCommandName,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
	)
}

func runInputWatch(cmd *cobra.Command, _ []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	streamClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return err
	}

	streamWriter, err := robot.NewStreamWriter(cmd.OutOrStdout())
	if err != nil {
		return exitcodes.Wrap(exitcodes.GeneralError, err)
	}

	startedAt := time.Now().UTC()
	headerWritten := false

	for {
		err = streamClient.WatchEventsOnce(
			cmd.Context(),
			func(result client.Result) error {
				if headerWritten {
					return nil
				}
				headerWritten = true
				return streamWriter.WriteHeader(
					"events",
					streamClient.Host(),
					startedAt,
					robot.FromClientDeviceContext(result.DeviceContext),
				)
			},
			func(event client.StreamEvent) error {
				return streamWriter.WriteEvent(event.Event, event.Data, time.Now().UTC())
			},
		)

		if cmd.Context().Err() != nil {
			if headerWritten {
				return streamWriter.WriteStreamEnd("client_disconnect", time.Now().UTC())
			}
			return nil
		}

		if errors.Is(err, io.EOF) || exitcodes.FromError(err) == exitcodes.NetworkError {
			if !sleepContext(cmd.Context(), streamReconnectDelay) {
				if headerWritten {
					return streamWriter.WriteStreamEnd("client_disconnect", time.Now().UTC())
				}
				return nil
			}
			continue
		}

		return err
	}
}

func newInputRuntime(cmd *cobra.Command) (appconfig.Runtime, *client.Client, string, time.Time, error) {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return appconfig.Runtime{}, nil, "", time.Time{}, exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	startedAt := time.Now()
	inputClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return runtime, nil, runtime.Host, startedAt, err
	}

	return runtime, inputClient, inputClient.Host(), startedAt, nil
}

func wrapInputResult(
	cmd *cobra.Command,
	runtime appconfig.Runtime,
	commandName string,
	host string,
	payload any,
	deviceContext *robot.DeviceContext,
	startedAt time.Time,
	err error,
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
		})
	}

	if err != nil {
		return err
	}

	return outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format)
}

func parseInputID(raw string) (int, error) {
	inputID, err := strconv.Atoi(raw)
	if err != nil || inputID < 1 || inputID > modioInputCount {
		return 0, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("input id %q must be in range 1-%d", raw, modioInputCount))
	}

	return inputID, nil
}

func (m apiInputMetadata) toMetadata() inputMetadata {
	return inputMetadata{
		SampleTSMS:     m.SampleTSMS,
		SampleAgeMS:    m.StalenessMS,
		PollIntervalMS: m.PollIntervalMS,
	}
}

func (r apiDigitalInputsResponse) toResult() digitalInputsResult {
	return digitalInputsResult{
		inputMetadata: r.toMetadata(),
		Inputs:        r.Inputs,
	}
}

func (r apiDigitalInputResponse) toResult() digitalInputResult {
	return digitalInputResult{
		inputMetadata: r.toMetadata(),
		Input:         r.Input,
	}
}

func (r apiAnalogInputsResponse) toResult() analogInputsResult {
	return analogInputsResult{
		inputMetadata: r.toMetadata(),
		Inputs:        r.Inputs,
	}
}

func (r apiAnalogInputResponse) toResult() analogInputResult {
	return analogInputResult{
		inputMetadata: r.toMetadata(),
		Input:         r.Input,
	}
}

func (r digitalInputsResult) TableOutput() (outputformat.TableData, error) {
	rows := make([][]string, 0, len(r.Inputs))
	for _, input := range r.Inputs {
		rows = append(rows, []string{
			strconv.Itoa(input.ID),
			strconv.FormatBool(input.State),
			formatInputUint(r.SampleTSMS),
			formatInputUint(r.SampleAgeMS),
			strconv.FormatUint(uint64(r.PollIntervalMS), 10),
		})
	}

	return outputformat.TableData{
		Headers: []string{"ID", "STATE", "SAMPLE TS (MS)", "SAMPLE AGE (MS)", "POLL (MS)"},
		Rows:    rows,
	}, nil
}

func (r digitalInputsResult) PlainOutput() ([]string, error) {
	lines := metadataPlainLines(r.inputMetadata)
	for _, input := range r.Inputs {
		lines = append(lines, fmt.Sprintf("input.%d.state=%t", input.ID, input.State))
	}
	return lines, nil
}

func (r digitalInputResult) TableOutput() (outputformat.TableData, error) {
	return outputformat.TableData{
		Headers: []string{"ID", "STATE", "SAMPLE TS (MS)", "SAMPLE AGE (MS)", "POLL (MS)"},
		Rows: [][]string{{
			strconv.Itoa(r.Input.ID),
			strconv.FormatBool(r.Input.State),
			formatInputUint(r.SampleTSMS),
			formatInputUint(r.SampleAgeMS),
			strconv.FormatUint(uint64(r.PollIntervalMS), 10),
		}},
	}, nil
}

func (r digitalInputResult) PlainOutput() ([]string, error) {
	lines := metadataPlainLines(r.inputMetadata)
	lines = append(lines, fmt.Sprintf("input.id=%d", r.Input.ID))
	lines = append(lines, fmt.Sprintf("input.state=%t", r.Input.State))
	return lines, nil
}

func (r analogInputsResult) TableOutput() (outputformat.TableData, error) {
	rows := make([][]string, 0, len(r.Inputs))
	for _, input := range r.Inputs {
		rows = append(rows, []string{
			strconv.Itoa(input.ID),
			strconv.Itoa(input.Value),
			formatInputUint(r.SampleTSMS),
			formatInputUint(r.SampleAgeMS),
			strconv.FormatUint(uint64(r.PollIntervalMS), 10),
		})
	}

	return outputformat.TableData{
		Headers: []string{"ID", "VALUE", "SAMPLE TS (MS)", "SAMPLE AGE (MS)", "POLL (MS)"},
		Rows:    rows,
	}, nil
}

func (r analogInputsResult) PlainOutput() ([]string, error) {
	lines := metadataPlainLines(r.inputMetadata)
	for _, input := range r.Inputs {
		lines = append(lines, fmt.Sprintf("input.%d.value=%d", input.ID, input.Value))
	}
	return lines, nil
}

func (r analogInputResult) TableOutput() (outputformat.TableData, error) {
	return outputformat.TableData{
		Headers: []string{"ID", "VALUE", "SAMPLE TS (MS)", "SAMPLE AGE (MS)", "POLL (MS)"},
		Rows: [][]string{{
			strconv.Itoa(r.Input.ID),
			strconv.Itoa(r.Input.Value),
			formatInputUint(r.SampleTSMS),
			formatInputUint(r.SampleAgeMS),
			strconv.FormatUint(uint64(r.PollIntervalMS), 10),
		}},
	}, nil
}

func (r analogInputResult) PlainOutput() ([]string, error) {
	lines := metadataPlainLines(r.inputMetadata)
	lines = append(lines, fmt.Sprintf("input.id=%d", r.Input.ID))
	lines = append(lines, fmt.Sprintf("input.value=%d", r.Input.Value))
	return lines, nil
}

func metadataPlainLines(metadata inputMetadata) []string {
	return []string{
		fmt.Sprintf("sample_ts_ms=%s", formatInputUint(metadata.SampleTSMS)),
		fmt.Sprintf("sample_age_ms=%s", formatInputUint(metadata.SampleAgeMS)),
		fmt.Sprintf("poll_interval_ms=%d", metadata.PollIntervalMS),
	}
}

func formatInputUint(value uint64) string {
	return strconv.FormatUint(value, 10)
}

func sleepContext(ctx context.Context, delay time.Duration) bool {
	timer := time.NewTimer(delay)
	defer timer.Stop()

	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
