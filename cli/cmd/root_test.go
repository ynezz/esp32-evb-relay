package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/hashicorp/mdns"
	"github.com/spf13/cobra"

	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

func withVersionMetadata(version string, commit string, date string, fn func()) {
	oldVersion := Version
	oldCommit := Commit
	oldDate := Date

	Version = version
	Commit = commit
	Date = date

	defer func() {
		Version = oldVersion
		Commit = oldCommit
		Date = oldDate
	}()

	fn()
}

func TestFormattedVersion(t *testing.T) {
	withVersionMetadata("1.2.3", "abc123", "2026-03-17T00:00:00Z", func() {
		got := formattedVersion()
		want := "1.2.3\ncommit: abc123\nbuilt: 2026-03-17T00:00:00Z"
		if got != want {
			t.Fatalf("formattedVersion() = %q, want %q", got, want)
		}
	})
}

func TestDefaultVersionUsesDevSemverFallback(t *testing.T) {
	if Version != "0.0.0-dev" {
		t.Fatalf("Version = %q, want %q", Version, "0.0.0-dev")
	}
}

func TestVersionFlagPrintsVersionMetadata(t *testing.T) {
	withVersionMetadata("1.2.3", "abc123", "2026-03-17T00:00:00Z", func() {
		cmd := newRootCommand()
		stdout := &bytes.Buffer{}

		cmd.SetOut(stdout)
		cmd.SetErr(&bytes.Buffer{})
		cmd.SetArgs([]string{"--version"})

		if err := cmd.Execute(); err != nil {
			t.Fatalf("Execute() returned error: %v", err)
		}

		want := "1.2.3\ncommit: abc123\nbuilt: 2026-03-17T00:00:00Z\n"
		if stdout.String() != want {
			t.Fatalf("version output = %q, want %q", stdout.String(), want)
		}
	})
}

func TestRobotCapabilitiesExposeCLIVersion(t *testing.T) {
	withVersionMetadata("1.2.3", "abc123", "2026-03-17T00:00:00Z", func() {
		cmd := newRootCommand()
		stdout := &bytes.Buffer{}
		var payload map[string]any

		cmd.SetOut(stdout)
		cmd.SetErr(&bytes.Buffer{})
		cmd.SetArgs([]string{"--robot-capabilities"})

		if err := cmd.Execute(); err != nil {
			t.Fatalf("Execute() returned error: %v", err)
		}

		if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
			t.Fatalf("json.Unmarshal() returned error: %v", err)
		}

		if got := payload["cli_version"]; got != "1.2.3" {
			t.Fatalf("cli_version = %#v, want %q", got, "1.2.3")
		}
		if got := payload["v"]; got != float64(1) {
			t.Fatalf("v = %#v, want 1", got)
		}
		if got := payload["envelope_version"]; got != float64(1) {
			t.Fatalf("envelope_version = %#v, want 1", got)
		}
		if got := payload["default_robot_format"]; got != "toon" {
			t.Fatalf("default_robot_format = %#v, want %q", got, "toon")
		}

		if _, exists := payload["version"]; exists {
			t.Fatalf("unexpected legacy version field in capabilities payload")
		}
	})
}

func TestNewRootCommandEnablesTraverseRunHooks(t *testing.T) {
	previous := cobra.EnableTraverseRunHooks
	cobra.EnableTraverseRunHooks = false
	t.Cleanup(func() {
		cobra.EnableTraverseRunHooks = previous
	})

	newRootCommand()

	if !cobra.EnableTraverseRunHooks {
		t.Fatal("cobra.EnableTraverseRunHooks = false, want true")
	}
}

func TestRobotCapabilitiesIgnoreInvalidTimeoutEnv(t *testing.T) {
	withVersionMetadata("1.2.3", "abc123", "2026-03-17T00:00:00Z", func() {
		t.Setenv(appconfig.EnvTimeout, "garbage")
		t.Setenv("XDG_CONFIG_HOME", t.TempDir())

		cmd := newRootCommand()
		stdout := &bytes.Buffer{}
		var payload map[string]any

		cmd.SetOut(stdout)
		cmd.SetErr(&bytes.Buffer{})
		cmd.SetArgs([]string{"--robot-capabilities"})

		if err := cmd.Execute(); err != nil {
			t.Fatalf("Execute() returned error: %v", err)
		}

		if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
			t.Fatalf("json.Unmarshal() returned error: %v", err)
		}

		if got := payload["v"]; got != float64(1) {
			t.Fatalf("v = %#v, want 1", got)
		}
	})
}

func TestRobotCapabilitiesIgnoreInvalidConfigFile(t *testing.T) {
	withVersionMetadata("1.2.3", "abc123", "2026-03-17T00:00:00Z", func() {
		configHome := t.TempDir()
		configDir := filepath.Join(configHome, "evb-relay")
		configPath := filepath.Join(configDir, "config.toml")
		stdout := &bytes.Buffer{}
		var payload map[string]any

		t.Setenv("XDG_CONFIG_HOME", configHome)
		if err := os.MkdirAll(configDir, 0o755); err != nil {
			t.Fatalf("os.MkdirAll() error = %v", err)
		}
		if err := os.WriteFile(configPath, []byte("timeout = [\n"), 0o600); err != nil {
			t.Fatalf("os.WriteFile() error = %v", err)
		}

		cmd := newRootCommand()
		cmd.SetOut(stdout)
		cmd.SetErr(&bytes.Buffer{})
		cmd.SetArgs([]string{"--robot-capabilities"})

		if err := cmd.Execute(); err != nil {
			t.Fatalf("Execute() returned error: %v", err)
		}

		if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
			t.Fatalf("json.Unmarshal() returned error: %v", err)
		}

		if got := payload["v"]; got != float64(1) {
			t.Fatalf("v = %#v, want 1", got)
		}
	})
}

func TestRobotCapabilitiesExposeCommandAndContractMetadata(t *testing.T) {
	cmd := newRootCommand()
	stdout := &bytes.Buffer{}
	var payload struct {
		Commands             []map[string]any `json:"commands"`
		ExitCodes            map[string]any   `json:"exit_codes"`
		ErrorCodes           map[string]any   `json:"error_codes"`
		EnvironmentVariables []map[string]any `json:"environment_variables"`
		StateMachine         map[string]any   `json:"state_machine"`
	}

	cmd.SetOut(stdout)
	cmd.SetErr(&bytes.Buffer{})
	cmd.SetArgs([]string{"--robot-capabilities"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("Execute() returned error: %v", err)
	}

	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() returned error: %v", err)
	}

	if len(payload.Commands) == 0 {
		t.Fatal("commands metadata is empty")
	}

	commandNames := make([]string, 0, len(payload.Commands))
	for _, command := range payload.Commands {
		name, _ := command["name"].(string)
		commandNames = append(commandNames, name)
	}
	if !containsString(commandNames, "completion") {
		t.Fatalf("commands missing completion: %#v", commandNames)
	}
	if !containsString(commandNames, "config show") {
		t.Fatalf("commands missing config show: %#v", commandNames)
	}
	if !containsString(commandNames, "config set") {
		t.Fatalf("commands missing config set: %#v", commandNames)
	}
	if !containsString(commandNames, "input watch") {
		t.Fatalf("commands missing input watch: %#v", commandNames)
	}
	if !containsString(commandNames, "input digital") {
		t.Fatalf("commands missing input digital: %#v", commandNames)
	}
	if !containsString(commandNames, "input analog") {
		t.Fatalf("commands missing input analog: %#v", commandNames)
	}
	if !containsString(commandNames, "ota flash") {
		t.Fatalf("commands missing ota flash: %#v", commandNames)
	}
	if !containsString(commandNames, "relay list") {
		t.Fatalf("commands missing relay list: %#v", commandNames)
	}
	if !containsString(commandNames, "relay on") {
		t.Fatalf("commands missing relay on: %#v", commandNames)
	}
	if !containsString(commandNames, "relay off") {
		t.Fatalf("commands missing relay off: %#v", commandNames)
	}
	if !containsString(commandNames, "relay toggle") {
		t.Fatalf("commands missing relay toggle: %#v", commandNames)
	}
	if !containsString(commandNames, "relay set") {
		t.Fatalf("commands missing relay set: %#v", commandNames)
	}
	if !containsString(commandNames, "status") {
		t.Fatalf("commands missing status: %#v", commandNames)
	}

	if got := payload.ExitCodes["7"]; got != "hardware unavailable" {
		t.Fatalf("exit_codes[7] = %#v, want %q", got, "hardware unavailable")
	}

	errorDetails, ok := payload.ErrorCodes["MODIO_NOT_PRESENT"].(map[string]any)
	if !ok {
		t.Fatalf("error_codes.MODIO_NOT_PRESENT = %#v; want object", payload.ErrorCodes["MODIO_NOT_PRESENT"])
	}
	if got := errorDetails["exit_code"]; got != float64(7) {
		t.Fatalf("MODIO_NOT_PRESENT exit_code = %#v, want 7", got)
	}

	authForbidden, ok := payload.ErrorCodes["AUTH_FORBIDDEN"].(map[string]any)
	if !ok {
		t.Fatalf("error_codes.AUTH_FORBIDDEN = %#v; want object", payload.ErrorCodes["AUTH_FORBIDDEN"])
	}
	if got := authForbidden["exit_code"]; got != float64(3) {
		t.Fatalf("AUTH_FORBIDDEN exit_code = %#v, want 3", got)
	}

	envNames := make([]string, 0, len(payload.EnvironmentVariables))
	for _, item := range payload.EnvironmentVariables {
		if name, _ := item["name"].(string); name != "" {
			envNames = append(envNames, name)
		}
	}
	if !containsString(envNames, appconfig.EnvAPIToken) {
		t.Fatalf("environment_variables missing %q: %#v", appconfig.EnvAPIToken, envNames)
	}

	if _, ok := payload.StateMachine["modio_sync_states"]; !ok {
		t.Fatalf("state_machine missing modio_sync_states: %#v", payload.StateMachine)
	}
}

func TestCompletionCommandGeneratesBashScript(t *testing.T) {
	cmd := newRootCommand()
	stdout := &bytes.Buffer{}

	cmd.SetOut(stdout)
	cmd.SetErr(&bytes.Buffer{})
	cmd.SetArgs([]string{"completion", "bash"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("Execute() returned error: %v", err)
	}

	output := stdout.String()
	if !strings.Contains(output, "__start_evb-relay") {
		t.Fatalf("completion output missing cobra entrypoint: %q", output)
	}
}

func TestCompletionCommandIgnoresInvalidTimeoutEnv(t *testing.T) {
	t.Setenv(appconfig.EnvTimeout, "garbage")
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())

	cmd := newRootCommand()
	stdout := &bytes.Buffer{}

	cmd.SetOut(stdout)
	cmd.SetErr(&bytes.Buffer{})
	cmd.SetArgs([]string{"completion", "bash"})

	if err := cmd.Execute(); err != nil {
		t.Fatalf("Execute() returned error: %v", err)
	}

	if stdout.Len() == 0 {
		t.Fatal("completion output is empty")
	}
}

func TestRobotModeRejectsHumanFormatsBeforeCommandExecution(t *testing.T) {
	for _, format := range []string{"table", "plain"} {
		format := format
		t.Run(format, func(t *testing.T) {
			previous := queryMDNS
			queryMDNS = func(_ context.Context, _ *mdns.QueryParam) error {
				t.Fatal("queryMDNS was called despite invalid robot format")
				return nil
			}
			t.Cleanup(func() {
				queryMDNS = previous
			})

			command := newRootCommand()
			stdout := &bytes.Buffer{}
			command.SetOut(stdout)
			command.SetErr(&bytes.Buffer{})
			command.SetArgs([]string{"--robot", "--format", format, "discover"})

			err := command.Execute()
			if err == nil {
				t.Fatal("Execute() succeeded; want error")
			}
			if got := exitcodes.FromError(err); got != exitcodes.BadArgument {
				t.Fatalf("exit code = %d, want %d", got, exitcodes.BadArgument)
			}
			if !strings.Contains(err.Error(), "expected toon or json") {
				t.Fatalf("error = %q, want robot format guidance", err.Error())
			}
			if stdout.Len() != 0 {
				t.Fatalf("stdout = %q, want empty", stdout.String())
			}
		})
	}
}

func containsString(values []string, want string) bool {
	for _, current := range values {
		if current == want {
			return true
		}
	}

	return false
}
