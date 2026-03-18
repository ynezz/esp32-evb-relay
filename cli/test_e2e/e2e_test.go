package test_e2e

import (
	"bytes"
	"encoding/json"
	"net/http"
	"os"
	"strconv"
	"strings"
	"testing"
)

type e2eStatusOutput struct {
	Status stubStatusResponse `json:"status"`
}

type e2eRelayBatchError struct {
	Code       string `json:"code"`
	Message    string `json:"message"`
	HTTPStatus int    `json:"http_status,omitempty"`
}

type e2eRelaySetTargetResult struct {
	Target    string              `json:"target"`
	Group     string              `json:"group"`
	ID        int                 `json:"id"`
	Requested bool                `json:"requested"`
	State     *bool               `json:"state"`
	Sync      *string             `json:"sync"`
	OK        bool                `json:"ok"`
	Error     *e2eRelayBatchError `json:"error,omitempty"`
}

type e2eRelaySetOutput struct {
	Results []e2eRelaySetTargetResult `json:"results"`
	AllOK   bool                      `json:"all_ok"`
}

type e2eInputMetadata struct {
	SampleTSMS     uint64 `json:"sample_ts_ms"`
	SampleAgeMS    uint64 `json:"sample_age_ms"`
	PollIntervalMS uint32 `json:"poll_interval_ms"`
}

type e2eDigitalInputsOutput struct {
	e2eInputMetadata
	Inputs []stubDigitalInputValue `json:"inputs"`
}

type e2eDigitalInputOutput struct {
	e2eInputMetadata
	Input stubDigitalInputValue `json:"input"`
}

type e2eAnalogInputsOutput struct {
	e2eInputMetadata
	Inputs []stubAnalogInputValue `json:"inputs"`
}

type e2eAnalogInputOutput struct {
	e2eInputMetadata
	Input stubAnalogInputValue `json:"input"`
}

type e2eOTAOutput struct {
	UploadedBytes   int64  `json:"uploaded_bytes"`
	FirmwareFile    string `json:"firmware_file"`
	RebootInSeconds int    `json:"reboot_in_seconds"`
}

type e2eDeviceContext struct {
	ModIOPresent    *bool  `json:"modio_present,omitempty"`
	ModIOSync       string `json:"modio_sync,omitempty"`
	FirmwareVersion string `json:"firmware_version,omitempty"`
}

type e2eRobotError struct {
	Code        string  `json:"code"`
	Message     string  `json:"message"`
	HTTPStatus  int     `json:"http_status,omitempty"`
	Retryable   bool    `json:"retryable"`
	Remediation *string `json:"remediation"`
}

type e2eRobotEnvelope struct {
	V             int               `json:"v"`
	Command       string            `json:"command,omitempty"`
	Timestamp     string            `json:"timestamp"`
	ElapsedMS     int64             `json:"elapsed_ms"`
	ExitCode      int               `json:"exit_code"`
	Host          string            `json:"host,omitempty"`
	DeviceContext *e2eDeviceContext `json:"device_context,omitempty"`
	Data          json.RawMessage   `json:"data,omitempty"`
	Error         *e2eRobotError    `json:"error,omitempty"`
	Warnings      []string          `json:"warnings,omitempty"`
	Next          []string          `json:"next,omitempty"`
}

func TestCLIHappyJSON(t *testing.T) {
	t.Run("status", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server)

		stdout, stderr, exitCode := runner.run("--format", "json", "status")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		payload := decodeJSON[e2eStatusOutput](t, stdout)
		if got := payload.Status.UptimeSeconds; got != 42 {
			t.Fatalf("uptime_seconds = %v, want 42", got)
		}
		if got := payload.Status.FirmwareVersion; got != stubFirmwareVersion {
			t.Fatalf("firmware_version = %q, want %q", got, stubFirmwareVersion)
		}
		if got := payload.Status.FreeHeapBytes; got != 123456 {
			t.Fatalf("free_heap_bytes = %v, want 123456", got)
		}
		if got := payload.Status.Network.Hostname; got != "lab-relay" {
			t.Fatalf("network.hostname = %q, want %q", got, "lab-relay")
		}
		if !payload.Status.ModIO.Present {
			t.Fatal("modio.present = false, want true")
		}
		if got := payload.Status.ModIO.Sync; got != "synchronized" {
			t.Fatalf("modio.sync = %q, want %q", got, "synchronized")
		}
	})

	t.Run("relay_flow", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server)

		stdout, stderr, exitCode := runner.run("--format", "json", "relay", "list")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		listPayload := decodeJSON[stubRelayListResponse](t, stdout)
		if !listPayload.ModIOPresent {
			t.Fatal("modio_present = false, want true")
		}
		if got := listPayload.ModIOSync; got != "synchronized" {
			t.Fatalf("modio_sync = %q, want %q", got, "synchronized")
		}
		if got := len(listPayload.Relays); got != 6 {
			t.Fatalf("relay count = %d, want 6", got)
		}
		assertRelayState(t, listPayload.Relays, "onboard", 1, false)
		assertRelayState(t, listPayload.Relays, "onboard", 2, true)
		assertRelayState(t, listPayload.Relays, "modio", 3, true)

		stdout, stderr, exitCode = runner.run("--format", "json", "relay", "on", "onboard:1")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		onPayload := decodeJSON[stubRelaySingleResponse](t, stdout)
		assertRelayView(t, onPayload.Relay, "onboard", 1, true)

		stdout, stderr, exitCode = runner.run("--format", "json", "relay", "off", "onboard:1")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		offPayload := decodeJSON[stubRelaySingleResponse](t, stdout)
		assertRelayView(t, offPayload.Relay, "onboard", 1, false)

		stdout, stderr, exitCode = runner.run("--format", "json", "relay", "toggle", "onboard:1")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		togglePayload := decodeJSON[stubRelaySingleResponse](t, stdout)
		assertRelayView(t, togglePayload.Relay, "onboard", 1, true)

		stdout, stderr, exitCode = runner.run("--format", "json", "relay", "set", "onboard:1=on", "modio:3=off")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		batchPayload := decodeJSON[e2eRelaySetOutput](t, stdout)
		if !batchPayload.AllOK {
			t.Fatalf("all_ok = false, want true; payload=%#v", batchPayload)
		}
		if got := len(batchPayload.Results); got != 2 {
			t.Fatalf("result count = %d, want 2", got)
		}
		assertRelayBatchResult(t, batchPayload.Results[0], "onboard:1", true, true, nil)
		assertRelayBatchResult(t, batchPayload.Results[1], "modio:3", false, false, stringPtr("synchronized"))

		server.SetModIOPresent(false)

		stdout, stderr, exitCode = runner.run("--format", "json", "relay", "list")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		absentPayload := decodeJSON[stubRelayListResponse](t, stdout)
		if absentPayload.ModIOPresent {
			t.Fatal("modio_present = true, want false")
		}
		if got := absentPayload.ModIOSync; got != "absent" {
			t.Fatalf("modio_sync = %q, want %q", got, "absent")
		}
		if got := len(absentPayload.Relays); got != 2 {
			t.Fatalf("relay count = %d, want 2", got)
		}
		for _, relay := range absentPayload.Relays {
			if relay.Group != "onboard" {
				t.Fatalf("relay = %#v, want only onboard relays", relay)
			}
		}
	})

	t.Run("input_flow", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server)

		stdout, stderr, exitCode := runner.run("--format", "json", "input", "digital")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		digitalPayload := decodeJSON[e2eDigitalInputsOutput](t, stdout)
		assertInputMetadata(t, digitalPayload.e2eInputMetadata)
		if got := len(digitalPayload.Inputs); got != 4 {
			t.Fatalf("digital input count = %d, want 4", got)
		}
		if got := digitalPayload.Inputs[1].ID; got != 2 {
			t.Fatalf("digital input[1].id = %d, want 2", got)
		}
		if got := digitalPayload.Inputs[1].State; got != false {
			t.Fatalf("digital input[1].state = %t, want false", got)
		}

		stdout, stderr, exitCode = runner.run("--format", "json", "input", "digital", "2")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		digitalSingle := decodeJSON[e2eDigitalInputOutput](t, stdout)
		assertInputMetadata(t, digitalSingle.e2eInputMetadata)
		if got := digitalSingle.Input.ID; got != 2 {
			t.Fatalf("digital input.id = %d, want 2", got)
		}
		if got := digitalSingle.Input.State; got != false {
			t.Fatalf("digital input.state = %t, want false", got)
		}

		stdout, stderr, exitCode = runner.run("--format", "json", "input", "analog")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		analogPayload := decodeJSON[e2eAnalogInputsOutput](t, stdout)
		assertInputMetadata(t, analogPayload.e2eInputMetadata)
		if got := len(analogPayload.Inputs); got != 4 {
			t.Fatalf("analog input count = %d, want 4", got)
		}
		if got := analogPayload.Inputs[2].Value; got != 512 {
			t.Fatalf("analog input[2].value = %d, want 512", got)
		}

		stdout, stderr, exitCode = runner.run("--format", "json", "input", "analog", "3")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		analogSingle := decodeJSON[e2eAnalogInputOutput](t, stdout)
		assertInputMetadata(t, analogSingle.e2eInputMetadata)
		if got := analogSingle.Input.ID; got != 3 {
			t.Fatalf("analog input.id = %d, want 3", got)
		}
		if got := analogSingle.Input.Value; got != 512 {
			t.Fatalf("analog input.value = %d, want 512", got)
		}
	})

	t.Run("ota_flash", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server)
		firmwarePath := writeDummyFirmware(t)

		stdout, stderr, exitCode := runner.run("--format", "json", "ota", "flash", firmwarePath)
		assertExitCode(t, exitCode, 0)

		payload := decodeJSON[e2eOTAOutput](t, stdout)
		if got := payload.UploadedBytes; got != 4 {
			t.Fatalf("uploaded_bytes = %d, want 4", got)
		}
		if got := payload.FirmwareFile; got != firmwarePath {
			t.Fatalf("firmware_file = %q, want %q", got, firmwarePath)
		}
		if got := payload.RebootInSeconds; got != 2 {
			t.Fatalf("reboot_in_seconds = %d, want 2", got)
		}
		assertStringContains(t, stderr, "Uploading firmware:")
		assertStringContains(t, stderr, expectedOTAWarning(2))
	})
}

func TestCLIErrorPaths(t *testing.T) {
	t.Run("auth_required_omits_device_context", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server).withAPIToken("")

		envelope, stderr, exitCode := runRobotJSON(t, runner, "status")
		assertExitCode(t, exitCode, 3)
		assertEmptyStderr(t, stderr)
		assertRobotError(t, envelope, "status", runner.host, "AUTH_REQUIRED", 3)
		if envelope.DeviceContext != nil {
			t.Fatalf("device_context = %#v, want nil", envelope.DeviceContext)
		}
		if len(bytes.TrimSpace(envelope.Data)) != 0 {
			t.Fatalf("data = %s, want empty", envelope.Data)
		}
	})

	t.Run("auth_forbidden", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server).withAPIToken("wrong-token")

		envelope, stderr, exitCode := runRobotJSON(t, runner, "status")
		assertExitCode(t, exitCode, 3)
		assertEmptyStderr(t, stderr)
		assertRobotError(t, envelope, "status", runner.host, "AUTH_FORBIDDEN", 3)
		if envelope.DeviceContext != nil {
			t.Fatalf("device_context = %#v, want nil", envelope.DeviceContext)
		}
	})

	t.Run("relay_not_found_maps_server_404", func(t *testing.T) {
		_, server := newStubServer(t)
		server.SetRelayAPIError("onboard:1", http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found")
		runner := newCLIRunner(t, server)

		envelope, stderr, exitCode := runRobotJSON(t, runner, "relay", "on", "onboard:1")
		assertExitCode(t, exitCode, 4)
		assertEmptyStderr(t, stderr)
		assertRobotError(t, envelope, "relay on", runner.host, "RELAY_NOT_FOUND", 4)
		assertDeviceContext(t, envelope.DeviceContext, true, "synchronized")
		if got := server.RequestCount(); got != 1 {
			t.Fatalf("request count = %d, want 1", got)
		}
	})

	t.Run("input_id_out_of_range_is_bad_argument", func(t *testing.T) {
		_, server := newStubServer(t)
		runner := newCLIRunner(t, server)

		envelope, stderr, exitCode := runRobotJSON(t, runner, "input", "digital", "9")
		assertExitCode(t, exitCode, 5)
		assertEmptyStderr(t, stderr)
		assertRobotError(t, envelope, "input digital", runner.host, "BAD_ARGUMENT", 5)
		if got := server.RequestCount(); got != 0 {
			t.Fatalf("request count = %d, want 0", got)
		}
	})

	t.Run("modio_absent_keeps_relay_list_but_rejects_modio_commands", func(t *testing.T) {
		_, server := newStubServer(t)
		server.SetModIOPresent(false)
		runner := newCLIRunner(t, server)

		stdout, stderr, exitCode := runner.run("--format", "json", "relay", "list")
		assertExitCode(t, exitCode, 0)
		assertEmptyStderr(t, stderr)

		listPayload := decodeJSON[stubRelayListResponse](t, stdout)
		if listPayload.ModIOPresent {
			t.Fatal("modio_present = true, want false")
		}
		if got := len(listPayload.Relays); got != 2 {
			t.Fatalf("relay count = %d, want 2", got)
		}

		envelope, stderr, exitCode := runRobotJSON(t, runner, "relay", "on", "modio:3")
		assertExitCode(t, exitCode, 7)
		assertEmptyStderr(t, stderr)
		assertRobotError(t, envelope, "relay on", runner.host, "MODIO_NOT_PRESENT", 7)
		assertDeviceContext(t, envelope.DeviceContext, false, "absent")
	})

	t.Run("invalid_relay_targets_fail_before_http", func(t *testing.T) {
		for _, invalidTarget := range []string{"foo", "onboard:", ":1"} {
			invalidTarget := invalidTarget
			t.Run(strings.ReplaceAll(invalidTarget, ":", "_"), func(t *testing.T) {
				_, server := newStubServer(t)
				runner := newCLIRunner(t, server)

				envelope, stderr, exitCode := runRobotJSON(t, runner, "relay", "on", invalidTarget)
				assertExitCode(t, exitCode, 5)
				assertEmptyStderr(t, stderr)
				assertRobotError(t, envelope, "relay on", runner.host, "BAD_ARGUMENT", 5)
				if got := server.RequestCount(); got != 0 {
					t.Fatalf("request count = %d, want 0", got)
				}
			})
		}
	})

	t.Run("relay_set_partial_failure_preserves_stdout_results", func(t *testing.T) {
		_, server := newStubServer(t)
		server.SetModIOPresent(false)
		runner := newCLIRunner(t, server)

		stdout, stderr, exitCode := runner.run("--format", "json", "relay", "set", "onboard:1=on", "modio:3=off")
		assertExitCode(t, exitCode, 1)
		assertStringContains(t, stderr, "PARTIAL_FAILURE")

		payload := decodeJSON[e2eRelaySetOutput](t, stdout)
		if payload.AllOK {
			t.Fatalf("all_ok = true, want false; payload=%#v", payload)
		}
		if got := len(payload.Results); got != 2 {
			t.Fatalf("result count = %d, want 2", got)
		}
		assertRelayBatchResult(t, payload.Results[0], "onboard:1", true, true, nil)
		assertRelayBatchFailure(t, payload.Results[1], "modio:3", false, "MODIO_NOT_PRESENT")
	})

	t.Run("network_error_returns_robot_remediation", func(t *testing.T) {
		httpServer, server := newStubServer(t)
		runner := newCLIRunner(t, server)
		httpServer.Close()

		envelope, stderr, exitCode := runRobotJSON(t, runner, "status")
		assertExitCode(t, exitCode, 2)
		assertEmptyStderr(t, stderr)
		errorDetails := assertRobotError(t, envelope, "status", runner.host, "NETWORK_ERROR", 2)
		if !errorDetails.Retryable {
			t.Fatal("retryable = false, want true")
		}
		if errorDetails.Remediation == nil || *errorDetails.Remediation == "" {
			t.Fatalf("remediation = %#v, want non-empty string", errorDetails.Remediation)
		}
		if !containsString(envelope.Next, "evb-relay discover") {
			t.Fatalf("next = %#v, want discover suggestion", envelope.Next)
		}
		if envelope.DeviceContext != nil {
			t.Fatalf("device_context = %#v, want nil", envelope.DeviceContext)
		}
	})
}

func TestCLIRobotEnvelopes(t *testing.T) {
	t.Run("success_commands_emit_structured_envelopes", func(t *testing.T) {
		testCases := []struct {
			name            string
			command         string
			args            []string
			expectedWarning string
			validateData    func(*testing.T, json.RawMessage)
		}{
			{
				name:    "status",
				command: "status",
				args:    []string{"status"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[e2eStatusOutput](t, raw)
					if got := payload.Status.FirmwareVersion; got != stubFirmwareVersion {
						t.Fatalf("firmware_version = %q, want %q", got, stubFirmwareVersion)
					}
				},
			},
			{
				name:    "relay_list",
				command: "relay list",
				args:    []string{"relay", "list"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[stubRelayListResponse](t, raw)
					if got := len(payload.Relays); got != 6 {
						t.Fatalf("relay count = %d, want 6", got)
					}
				},
			},
			{
				name:    "relay_on",
				command: "relay on",
				args:    []string{"relay", "on", "onboard:1"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[stubRelaySingleResponse](t, raw)
					assertRelayView(t, payload.Relay, "onboard", 1, true)
				},
			},
			{
				name:    "relay_off",
				command: "relay off",
				args:    []string{"relay", "off", "onboard:1"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[stubRelaySingleResponse](t, raw)
					assertRelayView(t, payload.Relay, "onboard", 1, false)
				},
			},
			{
				name:    "relay_toggle",
				command: "relay toggle",
				args:    []string{"relay", "toggle", "onboard:1"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[stubRelaySingleResponse](t, raw)
					assertRelayView(t, payload.Relay, "onboard", 1, true)
				},
			},
			{
				name:    "relay_set",
				command: "relay set",
				args:    []string{"relay", "set", "onboard:1=on", "modio:3=off"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[e2eRelaySetOutput](t, raw)
					if !payload.AllOK {
						t.Fatalf("all_ok = false, want true; payload=%#v", payload)
					}
				},
			},
			{
				name:    "input_digital",
				command: "input digital",
				args:    []string{"input", "digital"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[e2eDigitalInputsOutput](t, raw)
					if got := len(payload.Inputs); got != 4 {
						t.Fatalf("input count = %d, want 4", got)
					}
				},
			},
			{
				name:    "input_analog",
				command: "input analog",
				args:    []string{"input", "analog"},
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[e2eAnalogInputsOutput](t, raw)
					if got := len(payload.Inputs); got != 4 {
						t.Fatalf("input count = %d, want 4", got)
					}
				},
			},
			{
				name:            "ota_flash",
				command:         "ota flash",
				args:            []string{"ota", "flash"},
				expectedWarning: expectedOTAWarning(2),
				validateData: func(t *testing.T, raw json.RawMessage) {
					payload := decodeRawJSON[e2eOTAOutput](t, raw)
					if got := payload.RebootInSeconds; got != 2 {
						t.Fatalf("reboot_in_seconds = %d, want 2", got)
					}
				},
			},
		}

		for _, tc := range testCases {
			tc := tc
			t.Run(tc.name, func(t *testing.T) {
				_, server := newStubServer(t)
				runner := newCLIRunner(t, server)

				args := append([]string(nil), tc.args...)
				if tc.command == "ota flash" {
					args = append(args, writeDummyFirmware(t))
				}

				envelope, stderr, exitCode := runRobotJSON(t, runner, args...)
				assertExitCode(t, exitCode, 0)
				assertEmptyStderr(t, stderr)
				assertRobotSuccess(t, envelope, tc.command, runner.host)
				assertDeviceContext(t, envelope.DeviceContext, true, "synchronized")
				tc.validateData(t, envelope.Data)

				if tc.expectedWarning == "" {
					if len(envelope.Warnings) != 0 {
						t.Fatalf("warnings = %#v, want none", envelope.Warnings)
					}
					return
				}

				if got := len(envelope.Warnings); got != 1 {
					t.Fatalf("warning count = %d, want 1", got)
				}
				if got := envelope.Warnings[0]; got != tc.expectedWarning {
					t.Fatalf("warning = %q, want %q", got, tc.expectedWarning)
				}
			})
		}
	})

	t.Run("failure_envelope_includes_error_and_device_context", func(t *testing.T) {
		_, server := newStubServer(t)
		server.SetModIOPresent(false)
		runner := newCLIRunner(t, server)

		envelope, stderr, exitCode := runRobotJSON(t, runner, "relay", "on", "modio:1")
		assertExitCode(t, exitCode, 7)
		assertEmptyStderr(t, stderr)
		assertRobotError(t, envelope, "relay on", runner.host, "MODIO_NOT_PRESENT", 7)
		assertDeviceContext(t, envelope.DeviceContext, false, "absent")
		if len(bytes.TrimSpace(envelope.Data)) != 0 {
			t.Fatalf("data = %s, want empty", envelope.Data)
		}
	})

	t.Run("robot_rejects_human_formats_end_to_end", func(t *testing.T) {
		for _, format := range []string{"table", "plain"} {
			format := format
			t.Run(format, func(t *testing.T) {
				_, server := newStubServer(t)
				runner := newCLIRunner(t, server)

				stdout, stderr, exitCode := runner.run("--robot", "--format", format, "status")
				assertExitCode(t, exitCode, 5)
				if stdout != "" {
					t.Fatalf("stdout = %q, want empty", stdout)
				}
				assertStringContains(t, stderr, "expected toon or json")
				if got := server.RequestCount(); got != 0 {
					t.Fatalf("request count = %d, want 0", got)
				}
			})
		}
	})
}

func decodeJSON[T any](tb testing.TB, stdout string) T {
	tb.Helper()

	var payload T
	if err := json.Unmarshal([]byte(stdout), &payload); err != nil {
		tb.Fatalf("json.Unmarshal() error = %v; stdout=%s", err, stdout)
	}

	return payload
}

func decodeRawJSON[T any](tb testing.TB, raw json.RawMessage) T {
	tb.Helper()

	var payload T
	if err := json.Unmarshal(raw, &payload); err != nil {
		tb.Fatalf("json.Unmarshal() error = %v; raw=%s", err, raw)
	}

	return payload
}

func runRobotJSON(tb testing.TB, runner *cliRunner, args ...string) (e2eRobotEnvelope, string, int) {
	tb.Helper()

	commandArgs := append([]string{"--robot", "--format", "json"}, args...)
	stdout, stderr, exitCode := runner.run(commandArgs...)
	return decodeJSON[e2eRobotEnvelope](tb, stdout), stderr, exitCode
}

func assertEmptyStderr(tb testing.TB, stderr string) {
	tb.Helper()

	if stderr != "" {
		tb.Fatalf("stderr = %q, want empty", stderr)
	}
}

func assertStringContains(tb testing.TB, got string, want string) {
	tb.Helper()

	if !strings.Contains(got, want) {
		tb.Fatalf("string = %q, want substring %q", got, want)
	}
}

func assertDeviceContext(tb testing.TB, context *e2eDeviceContext, wantPresent bool, wantSync string) {
	tb.Helper()

	if context == nil {
		tb.Fatal("device_context = nil, want object")
		return
	}
	if got := context.FirmwareVersion; got != stubFirmwareVersion {
		tb.Fatalf("firmware_version = %q, want %q", got, stubFirmwareVersion)
	}
	if context.ModIOPresent == nil {
		tb.Fatal("modio_present = nil, want bool")
		return
	}
	if got := *context.ModIOPresent; got != wantPresent {
		tb.Fatalf("modio_present = %t, want %t", got, wantPresent)
	}
	if got := context.ModIOSync; got != wantSync {
		tb.Fatalf("modio_sync = %q, want %q", got, wantSync)
	}
}

func assertRobotSuccess(tb testing.TB, envelope e2eRobotEnvelope, wantCommand string, wantHost string) {
	tb.Helper()

	if got := envelope.V; got != 1 {
		tb.Fatalf("v = %d, want 1", got)
	}
	if got := envelope.Command; got != wantCommand {
		tb.Fatalf("command = %q, want %q", got, wantCommand)
	}
	if got := envelope.Host; got != wantHost {
		tb.Fatalf("host = %q, want %q", got, wantHost)
	}
	if envelope.Timestamp == "" {
		tb.Fatal("timestamp is empty")
	}
	if envelope.ElapsedMS < 0 {
		tb.Fatalf("elapsed_ms = %d, want non-negative", envelope.ElapsedMS)
	}
	if got := envelope.ExitCode; got != 0 {
		tb.Fatalf("exit_code = %d, want 0", got)
	}
	if envelope.Error != nil {
		tb.Fatalf("error = %#v, want nil", envelope.Error)
	}
	if len(bytes.TrimSpace(envelope.Data)) == 0 {
		tb.Fatal("data is empty")
	}
}

func assertRobotError(
	tb testing.TB,
	envelope e2eRobotEnvelope,
	wantCommand string,
	wantHost string,
	wantCode string,
	wantExitCode int,
) *e2eRobotError {
	tb.Helper()

	if got := envelope.V; got != 1 {
		tb.Fatalf("v = %d, want 1", got)
	}
	if got := envelope.Command; got != wantCommand {
		tb.Fatalf("command = %q, want %q", got, wantCommand)
	}
	if got := envelope.Host; got != wantHost {
		tb.Fatalf("host = %q, want %q", got, wantHost)
	}
	if envelope.Timestamp == "" {
		tb.Fatal("timestamp is empty")
	}
	if envelope.ElapsedMS < 0 {
		tb.Fatalf("elapsed_ms = %d, want non-negative", envelope.ElapsedMS)
	}
	if got := envelope.ExitCode; got != wantExitCode {
		tb.Fatalf("exit_code = %d, want %d", got, wantExitCode)
	}
	if envelope.Error == nil {
		tb.Fatal("error = nil, want object")
	}
	if got := envelope.Error.Code; got != wantCode {
		tb.Fatalf("error.code = %q, want %q", got, wantCode)
	}
	if strings.TrimSpace(envelope.Error.Message) == "" {
		tb.Fatal("error.message is empty")
	}

	return envelope.Error
}

func assertRelayState(tb testing.TB, relays []stubRelayView, group string, relayID int, wantState bool) {
	tb.Helper()

	for _, relay := range relays {
		if relay.Group == group && relay.ID == relayID {
			if relay.State != wantState {
				tb.Fatalf("relay %s:%d state = %t, want %t", group, relayID, relay.State, wantState)
			}
			return
		}
	}

	tb.Fatalf("relay %s:%d not found in %#v", group, relayID, relays)
}

func assertRelayView(tb testing.TB, relay stubRelayView, wantGroup string, wantID int, wantState bool) {
	tb.Helper()

	if relay.Group != wantGroup || relay.ID != wantID || relay.State != wantState {
		tb.Fatalf("relay = %#v, want %s:%d state=%t", relay, wantGroup, wantID, wantState)
	}
}

func assertRelayBatchResult(
	tb testing.TB,
	result e2eRelaySetTargetResult,
	wantTarget string,
	wantRequested bool,
	wantState bool,
	wantSync *string,
) {
	tb.Helper()

	if result.Target != wantTarget {
		tb.Fatalf("target = %q, want %q", result.Target, wantTarget)
	}
	if result.Requested != wantRequested {
		tb.Fatalf("requested = %t, want %t", result.Requested, wantRequested)
	}
	if !result.OK {
		tb.Fatalf("ok = false, want true; result=%#v", result)
	}
	if result.State == nil || *result.State != wantState {
		tb.Fatalf("state = %#v, want %t", result.State, wantState)
	}
	switch {
	case wantSync == nil && result.Sync != nil:
		tb.Fatalf("sync = %#v, want nil", result.Sync)
	case wantSync != nil && result.Sync == nil:
		tb.Fatalf("sync = nil, want %q", *wantSync)
	case wantSync != nil && result.Sync != nil && *result.Sync != *wantSync:
		tb.Fatalf("sync = %q, want %q", *result.Sync, *wantSync)
	}
	if result.Error != nil {
		tb.Fatalf("error = %#v, want nil", result.Error)
	}
}

func assertRelayBatchFailure(
	tb testing.TB,
	result e2eRelaySetTargetResult,
	wantTarget string,
	wantRequested bool,
	wantCode string,
) {
	tb.Helper()

	if result.Target != wantTarget {
		tb.Fatalf("target = %q, want %q", result.Target, wantTarget)
	}
	if result.Requested != wantRequested {
		tb.Fatalf("requested = %t, want %t", result.Requested, wantRequested)
	}
	if result.OK {
		tb.Fatalf("ok = true, want false; result=%#v", result)
	}
	if result.Error == nil {
		tb.Fatalf("error = nil, want %q", wantCode)
	}
	if got := result.Error.Code; got != wantCode {
		tb.Fatalf("error.code = %q, want %q", got, wantCode)
	}
}

func assertInputMetadata(tb testing.TB, metadata e2eInputMetadata) {
	tb.Helper()

	if got := metadata.SampleTSMS; got != 12345 {
		tb.Fatalf("sample_ts_ms = %d, want 12345", got)
	}
	if got := metadata.SampleAgeMS; got != 67 {
		tb.Fatalf("sample_age_ms = %d, want 67", got)
	}
	if got := metadata.PollIntervalMS; got != stubPollIntervalMS {
		tb.Fatalf("poll_interval_ms = %d, want %d", got, stubPollIntervalMS)
	}
}

func writeDummyFirmware(tb testing.TB) string {
	tb.Helper()

	path := tb.TempDir() + "/dummy.bin"
	if err := os.WriteFile(path, []byte{0xde, 0xad, 0xbe, 0xef}, 0o600); err != nil {
		tb.Fatalf("os.WriteFile() error = %v", err)
	}

	return path
}

func expectedOTAWarning(delaySeconds int) string {
	return "Device will reboot in about " + strconv.Itoa(delaySeconds) + " seconds and disconnect."
}

func containsString(values []string, want string) bool {
	for _, current := range values {
		if current == want {
			return true
		}
	}

	return false
}
