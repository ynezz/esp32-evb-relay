package cmd

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/spf13/cobra"

	appconfig "example.com/esp32-evb-relay/cli/internal/config"
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

		if got := payload["name"]; got != "evb-relay" {
			t.Fatalf("name = %#v, want %q", got, "evb-relay")
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

		if got := payload["name"]; got != "evb-relay" {
			t.Fatalf("name = %#v, want %q", got, "evb-relay")
		}
	})
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
