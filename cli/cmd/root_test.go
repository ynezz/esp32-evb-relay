package cmd

import (
	"bytes"
	"encoding/json"
	"testing"
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
