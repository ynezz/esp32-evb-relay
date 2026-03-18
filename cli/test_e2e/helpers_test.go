package test_e2e

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

var cliBinaryPath string

type cliRunner struct {
	host     string
	apiToken string
	timeout  string
}

func TestMain(m *testing.M) {
	tmpDir, err := os.MkdirTemp("", "evb-relay-cli-e2e-*")
	if err != nil {
		fmt.Fprintf(os.Stderr, "MkdirTemp() error: %v\n", err)
		os.Exit(1)
	}

	exitCode := func() int {
		var buildErr error

		cliBinaryPath, buildErr = buildCLIBinary(tmpDir)
		if buildErr != nil {
			fmt.Fprintf(os.Stderr, "buildCLIBinary() error: %v\n", buildErr)
			return 1
		}

		return m.Run()
	}()

	_ = os.RemoveAll(tmpDir)
	os.Exit(exitCode)
}

func buildCLIBinary(tmpDir string) (string, error) {
	workingDir, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("get working directory: %w", err)
	}

	moduleDir := filepath.Clean(filepath.Join(workingDir, ".."))
	binaryPath := filepath.Join(tmpDir, "evb-relay")
	command := exec.Command("go", "build", "-o", binaryPath, ".")
	command.Dir = moduleDir

	var stderr bytes.Buffer
	command.Stderr = &stderr
	if err := command.Run(); err != nil {
		return "", fmt.Errorf("go build: %w\n%s", err, stderr.String())
	}

	return binaryPath, nil
}

func newCLIRunner(tb testing.TB, server *stubServerControl) *cliRunner {
	tb.Helper()

	return &cliRunner{
		host:     server.HostPort(tb),
		apiToken: stubAPIToken,
		timeout:  "5s",
	}
}

func (r *cliRunner) run(args ...string) (string, string, int) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	commandArgs := []string{
		"--host", r.host,
		"--api-token", r.apiToken,
		"--timeout", r.timeout,
	}
	commandArgs = append(commandArgs, args...)

	command := exec.CommandContext(ctx, cliBinaryPath, commandArgs...)
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	command.Stdout = &stdout
	command.Stderr = &stderr

	err := command.Run()
	if ctx.Err() != nil {
		return stdout.String(), stderr.String(), 124
	}
	if err == nil {
		return stdout.String(), stderr.String(), 0
	}

	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return stdout.String(), stderr.String(), exitErr.ExitCode()
	}

	return stdout.String(), stderr.String(), 125
}

func parseRobotJSON(tb testing.TB, stdout string) map[string]any {
	tb.Helper()

	var payload map[string]any
	if err := json.Unmarshal([]byte(stdout), &payload); err != nil {
		tb.Fatalf("json.Unmarshal() error = %v; stdout=%s", err, stdout)
	}

	return payload
}

func assertExitCode(tb testing.TB, got int, want int) {
	tb.Helper()

	if got != want {
		tb.Fatalf("exit code = %d, want %d", got, want)
	}
}

func TestCLIStatusRobotEnvelopeAgainstStub(t *testing.T) {
	_, server := newStubServer(t)
	runner := newCLIRunner(t, server)

	stdout, stderr, exitCode := runner.run("--robot", "--format", "json", "status")
	assertExitCode(t, exitCode, 0)
	if stderr != "" {
		t.Fatalf("stderr = %q, want empty", stderr)
	}

	payload := parseRobotJSON(t, stdout)
	if got := payload["command"]; got != "status" {
		t.Fatalf("command = %#v, want %q", got, "status")
	}
	if got := payload["host"]; got != server.HostPort(t) {
		t.Fatalf("host = %#v, want %q", got, server.HostPort(t))
	}

	deviceContext, ok := payload["device_context"].(map[string]any)
	if !ok {
		t.Fatalf("device_context = %#v; want object", payload["device_context"])
	}
	if got := deviceContext["firmware_version"]; got != stubFirmwareVersion {
		t.Fatalf("firmware_version = %#v, want %q", got, stubFirmwareVersion)
	}
	if got := deviceContext["modio_present"]; got != true {
		t.Fatalf("modio_present = %#v, want true", got)
	}
	if got := deviceContext["modio_sync"]; got != "synchronized" {
		t.Fatalf("modio_sync = %#v, want %q", got, "synchronized")
	}

	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("data = %#v; want object", payload["data"])
	}
	status, ok := data["status"].(map[string]any)
	if !ok {
		t.Fatalf("data.status = %#v; want object", data["status"])
	}
	if got := status["firmware_version"]; got != stubFirmwareVersion {
		t.Fatalf("status.firmware_version = %#v, want %q", got, stubFirmwareVersion)
	}
}
