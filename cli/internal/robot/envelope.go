package robot

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/spf13/cobra"

	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
	"example.com/esp32-evb-relay/cli/internal/toon"
)

const EnvelopeVersion = 1

type WrapOpts struct {
	Command       string
	Data          any
	DeviceContext *DeviceContext
	Err           error
	Format        string
	Host          string
	Timestamp     time.Time
	Elapsed       time.Duration
	Warnings      []string
	Next          []string
}

type envelope struct {
	V             int            `json:"v"`
	Command       string         `json:"command,omitempty"`
	Timestamp     string         `json:"timestamp"`
	ElapsedMS     int64          `json:"elapsed_ms"`
	ExitCode      int            `json:"exit_code"`
	Host          string         `json:"host,omitempty"`
	DeviceContext *DeviceContext `json:"device_context,omitempty"`
	Data          any            `json:"data,omitempty"`
	Error         *ErrorDetails  `json:"error,omitempty"`
	Warnings      []string       `json:"warnings,omitempty"`
	Next          []string       `json:"next,omitempty"`
}

func Wrap(cmd *cobra.Command, w io.Writer, opts WrapOpts) error {
	if w == nil {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("robot output writer is required"))
	}

	format := strings.ToLower(strings.TrimSpace(opts.Format))
	if format == "" {
		format = outputformat.TOON
	}
	if format != outputformat.TOON && format != outputformat.JSON {
		return exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("invalid robot format %q: expected toon or json", opts.Format))
	}

	envelopePayload := envelope{
		V:             EnvelopeVersion,
		Command:       resolveCommandName(cmd, opts.Command),
		Timestamp:     resolveTimestamp(opts.Timestamp),
		ElapsedMS:     opts.Elapsed.Milliseconds(),
		ExitCode:      int(exitcodes.Success),
		Host:          strings.TrimSpace(opts.Host),
		DeviceContext: opts.DeviceContext,
		Data:          opts.Data,
		Warnings:      cloneStrings(opts.Warnings),
		Next:          cloneStrings(opts.Next),
	}

	if opts.Err != nil {
		errorDetails, exitCode, suggestedNext := buildErrorDetails(opts.Err)
		envelopePayload.ExitCode = int(exitCode)
		envelopePayload.Error = &errorDetails
		envelopePayload.Next = mergeSuggestions(envelopePayload.Next, suggestedNext)
	}

	if err := writeEnvelope(w, envelopePayload, format); err != nil {
		return exitcodes.Wrap(exitcodes.GeneralError, err)
	}

	if opts.Err != nil {
		return silentExitError{code: exitcodes.Code(envelopePayload.ExitCode)}
	}

	return nil
}

func writeEnvelope(w io.Writer, payload envelope, format string) error {
	switch format {
	case outputformat.JSON:
		encoder := json.NewEncoder(w)
		encoder.SetEscapeHTML(false)
		encoder.SetIndent("", "  ")
		return encoder.Encode(payload)
	case outputformat.TOON:
		return toon.Encode(w, payload)
	default:
		return fmt.Errorf("unsupported robot format %q", format)
	}
}

func resolveCommandName(cmd *cobra.Command, explicit string) string {
	if explicit = strings.TrimSpace(explicit); explicit != "" {
		return explicit
	}
	if cmd == nil {
		return ""
	}

	parts := strings.Fields(cmd.CommandPath())
	if len(parts) > 1 {
		parts = parts[1:]
	}
	parts = append(parts, cmd.Flags().Args()...)
	return strings.Join(parts, " ")
}

func resolveTimestamp(ts time.Time) string {
	if ts.IsZero() {
		ts = time.Now().UTC()
	} else {
		ts = ts.UTC()
	}

	return ts.Format(time.RFC3339Nano)
}

func cloneStrings(values []string) []string {
	if len(values) == 0 {
		return nil
	}

	cloned := make([]string, len(values))
	copy(cloned, values)
	return cloned
}

func mergeSuggestions(values []string, additions []string) []string {
	if len(additions) == 0 {
		return values
	}

	seen := make(map[string]struct{}, len(values)+len(additions))
	merged := make([]string, 0, len(values)+len(additions))
	for _, current := range append(append([]string(nil), values...), additions...) {
		current = strings.TrimSpace(current)
		if current == "" {
			continue
		}
		if _, exists := seen[current]; exists {
			continue
		}
		seen[current] = struct{}{}
		merged = append(merged, current)
	}

	if len(merged) == 0 {
		return nil
	}

	return merged
}
