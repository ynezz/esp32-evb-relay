package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestApplyInputsTrimsAPIToken(t *testing.T) {
	t.Parallel()

	runtime := Runtime{}

	applyInputs(&runtime, new(string), Inputs{
		APIToken: StringInput{
			Value: "  secret-token\t\n",
			Set:   true,
		},
	})

	if runtime.APIToken != "secret-token" {
		t.Fatalf("APIToken = %q, want %q", runtime.APIToken, "secret-token")
	}
}

func TestLoadFileTrimsAPIToken(t *testing.T) {
	t.Parallel()

	path := filepath.Join(t.TempDir(), "config.toml")
	if err := os.WriteFile(path, []byte("api_token = \"  secret-token  \"\n"), 0o600); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}

	parsed, err := loadFile(path)
	if err != nil {
		t.Fatalf("loadFile() error = %v", err)
	}

	if !parsed.APITokenSet {
		t.Fatal("APITokenSet = false, want true")
	}
	if parsed.APIToken != "secret-token" {
		t.Fatalf("APIToken = %q, want %q", parsed.APIToken, "secret-token")
	}
}

func TestLoadEnvironmentTrimsAPIToken(t *testing.T) {
	t.Setenv(EnvAPIToken, "  secret-token\t\n")

	parsed, err := loadEnvironment()
	if err != nil {
		t.Fatalf("loadEnvironment() error = %v", err)
	}

	if !parsed.APITokenSet {
		t.Fatal("APITokenSet = false, want true")
	}
	if parsed.APIToken != "secret-token" {
		t.Fatalf("APIToken = %q, want %q", parsed.APIToken, "secret-token")
	}
}
