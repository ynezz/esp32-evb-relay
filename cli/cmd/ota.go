package cmd

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/spf13/cobra"

	"example.com/esp32-evb-relay/cli/client"
	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
	"example.com/esp32-evb-relay/cli/internal/robot"
)

const (
	otaDefaultTimeout      = 2 * time.Minute
	otaRebootDelaySeconds  = 2
	otaProgressPercentStep = 10
	otaCommandName         = "ota flash"
)

type otaFlashResult struct {
	UploadedBytes   int64  `json:"uploaded_bytes"`
	FirmwareFile    string `json:"firmware_file"`
	RebootInSeconds int    `json:"reboot_in_seconds"`
}

type otaUploadResponse struct {
	RebootInSeconds int `json:"reboot_in_seconds"`
}

type firmwareUpload struct {
	Path string
	Size int64
	File *os.File
}

func newOTACommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "ota",
		Short: "Manage over-the-air firmware updates",
	}

	command.AddCommand(newOTAFlashCommand())
	return command
}

func newOTAFlashCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "flash <firmware.bin>",
		Short: "Upload a firmware binary and trigger a reboot",
		Args:  cobra.ExactArgs(1),
		RunE:  runOTAFlash,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: []string{"--host", "--api-token", "--format", "--timeout", "--robot"},
		Args:  []string{"firmware_file"},
		OutputFields: []string{
			"uploaded_bytes",
			"firmware_file",
			"reboot_in_seconds",
		},
		Errors: []string{
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
		},
		Example: "evb-relay ota flash firmware/build/esp32-evb-relay.bin",
	})

	return command
}

func runOTAFlash(cmd *cobra.Command, args []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	startedAt := time.Now()
	host := runtime.Host

	firmware, err := openFirmwareBinary(args[0])
	if err != nil {
		return wrapOTARobotResult(cmd, runtime, host, nil, nil, startedAt, err)
	}
	defer func() {
		_ = closeFirmwareUpload(firmware)
	}()

	uploadClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  otaTimeout(runtime.Timeout),
	})
	if err != nil {
		return wrapOTARobotResult(cmd, runtime, host, nil, nil, startedAt, err)
	}
	host = uploadClient.Host()

	reporter := newUploadProgressReporter(cmd.ErrOrStderr(), runtime.Robot, firmware.Size)
	requestBody := io.Reader(firmware.File)
	if reporter != nil {
		reporter.Start()
		requestBody = &progressReader{
			reader:   firmware.File,
			reporter: reporter,
		}
	}

	var response otaUploadResponse
	result, err := uploadClient.UploadBinary(cmd.Context(), "/ota", requestBody, firmware.Size, &response)
	if reporter != nil {
		reporter.Finish(err)
	}

	payload := &otaFlashResult{
		UploadedBytes:   firmware.Size,
		FirmwareFile:    firmware.Path,
		RebootInSeconds: otaRebootDelaySeconds,
	}
	if response.RebootInSeconds > 0 {
		payload.RebootInSeconds = response.RebootInSeconds
	}
	if err != nil {
		payload = nil
	}

	return wrapOTARobotResult(
		cmd,
		runtime,
		host,
		payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
	)
}

func wrapOTARobotResult(
	cmd *cobra.Command,
	runtime appconfig.Runtime,
	host string,
	payload *otaFlashResult,
	deviceContext *robot.DeviceContext,
	startedAt time.Time,
	err error,
) error {
	elapsed := time.Since(startedAt)

	if runtime.Robot {
		warnings := []string(nil)
		data := any(nil)
		if err == nil && payload != nil {
			data = payload
			warnings = []string{otaRebootWarning(payload.RebootInSeconds)}
		}

		return robot.Wrap(cmd, cmd.OutOrStdout(), robot.WrapOpts{
			Command:       otaCommandName,
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

	if payload != nil {
		if outputErr := outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format); outputErr != nil {
			return outputErr
		}

		_, writeErr := fmt.Fprintln(cmd.ErrOrStderr(), otaRebootWarning(payload.RebootInSeconds))
		return writeErr
	}

	return outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format)
}

func openFirmwareBinary(path string) (*firmwareUpload, error) {
	trimmed := filepath.Clean(path)
	if trimmed == "." || trimmed == "" {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, errors.New("firmware path is required"))
	}

	info, err := os.Stat(trimmed)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("firmware file %q does not exist", trimmed))
		}
		return nil, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("stat firmware file %q: %w", trimmed, err))
	}
	if info.IsDir() {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("firmware path %q is a directory", trimmed))
	}
	if info.Size() <= 0 {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("firmware file %q is empty", trimmed))
	}

	file, err := os.Open(trimmed)
	if err != nil {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("open firmware file %q: %w", trimmed, err))
	}

	return &firmwareUpload{
		Path: trimmed,
		Size: info.Size(),
		File: file,
	}, nil
}

func otaTimeout(timeout time.Duration) time.Duration {
	if timeout == appconfig.DefaultTimeout {
		return otaDefaultTimeout
	}

	return timeout
}

func otaRebootWarning(delaySeconds int) string {
	return fmt.Sprintf("Device will reboot in about %d seconds and disconnect.", delaySeconds)
}

func closeFirmwareUpload(firmware *firmwareUpload) error {
	if firmware == nil || firmware.File == nil {
		return nil
	}

	if err := firmware.File.Close(); err != nil {
		return exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("close firmware file %q: %w", firmware.Path, err))
	}

	firmware.File = nil
	return nil
}

type progressReader struct {
	reader   io.Reader
	reporter *uploadProgressReporter
}

func (r *progressReader) Read(p []byte) (int, error) {
	n, err := r.reader.Read(p)
	if n > 0 && r.reporter != nil {
		r.reporter.Advance(n)
	}
	return n, err
}

type uploadProgressReporter struct {
	out        io.Writer
	total      int64
	written    int64
	lastBucket int
}

func newUploadProgressReporter(out io.Writer, robotMode bool, total int64) *uploadProgressReporter {
	if robotMode || out == nil || total <= 0 {
		return nil
	}

	return &uploadProgressReporter{
		out:        out,
		total:      total,
		lastBucket: -1,
	}
}

func (r *uploadProgressReporter) Start() {
	r.printProgress()
}

func (r *uploadProgressReporter) Advance(read int) {
	r.written += int64(read)

	bucket := int(r.written*100/r.total) / otaProgressPercentStep
	if r.written >= r.total {
		bucket = 100 / otaProgressPercentStep
	}
	if bucket <= r.lastBucket {
		return
	}

	r.printProgress()
}

func (r *uploadProgressReporter) Finish(err error) {
	if err != nil {
		_, _ = fmt.Fprintf(r.out, "Upload failed after %d/%d bytes.\n", r.written, r.total)
		return
	}

	if r.written < r.total {
		r.written = r.total
		r.printProgress()
	}
}

func (r *uploadProgressReporter) printProgress() {
	percent := int(r.written * 100 / r.total)
	if r.written >= r.total {
		percent = 100
	}

	r.lastBucket = percent / otaProgressPercentStep
	_, _ = fmt.Fprintf(r.out, "Uploading firmware: %d%% (%d/%d bytes)\n", percent, r.written, r.total)
}

func (r otaFlashResult) TableOutput() (outputformat.TableData, error) {
	return outputformat.TableData{
		Headers: []string{"FIRMWARE FILE", "UPLOADED BYTES", "REBOOT IN (S)"},
		Rows: [][]string{{
			r.FirmwareFile,
			fmt.Sprintf("%d", r.UploadedBytes),
			fmt.Sprintf("%d", r.RebootInSeconds),
		}},
	}, nil
}

func (r otaFlashResult) PlainOutput() ([]string, error) {
	return []string{
		fmt.Sprintf("firmware_file=%s", r.FirmwareFile),
		fmt.Sprintf("uploaded_bytes=%d", r.UploadedBytes),
		fmt.Sprintf("reboot_in_seconds=%d", r.RebootInSeconds),
	}, nil
}
