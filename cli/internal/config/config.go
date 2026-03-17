package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/BurntSushi/toml"

	outputformat "example.com/esp32-evb-relay/cli/internal/format"
)

const (
	EnvHost     = "EVB_RELAY_HOST"
	EnvAPIToken = "EVB_RELAY_API_TOKEN"
	EnvRobot    = "EVB_RELAY_ROBOT"
	EnvTimeout  = "EVB_RELAY_TIMEOUT"

	configDirName  = "evb-relay"
	configFileName = "config.toml"
)

var DefaultTimeout = 10 * time.Second

type ErrorKind string

const (
	ErrorKindInvalid ErrorKind = "invalid"
	ErrorKindIO      ErrorKind = "io"
)

type Error struct {
	Kind ErrorKind
	Err  error
}

func (e Error) Error() string {
	return e.Err.Error()
}

func (e Error) Unwrap() error {
	return e.Err
}

type StringInput struct {
	Value string
	Set   bool
}

type BoolInput struct {
	Value bool
	Set   bool
}

type DurationInput struct {
	Value time.Duration
	Set   bool
}

type Inputs struct {
	Host     StringInput
	APIToken StringInput
	Format   StringInput
	Timeout  DurationInput
	Robot    BoolInput
}

type Runtime struct {
	Host       string
	APIToken   string
	Format     string
	Timeout    time.Duration
	Robot      bool
	ConfigPath string
}

type partialConfig struct {
	Host        string
	HostSet     bool
	APIToken    string
	APITokenSet bool
	Format      string
	FormatSet   bool
	Timeout     time.Duration
	TimeoutSet  bool
	Robot       bool
	RobotSet    bool
}

type fileConfig struct {
	Host     string `toml:"host"`
	APIToken string `toml:"api_token"`
	Format   string `toml:"format"`
	Timeout  string `toml:"timeout"`
	Robot    *bool  `toml:"robot"`
}

func DefaultPath() (string, error) {
	basePath, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}

	return filepath.Join(basePath, configDirName, configFileName), nil
}

func Resolve(inputs Inputs) (Runtime, error) {
	path, pathErr := DefaultPath()

	fileValues, err := loadFile(path)
	if err != nil {
		return Runtime{}, err
	}

	envValues, err := loadEnvironment()
	if err != nil {
		return Runtime{}, err
	}

	resolved := Runtime{
		Timeout:    DefaultTimeout,
		ConfigPath: path,
	}

	requestedFormat := ""
	applyPartial(&resolved, &requestedFormat, fileValues)
	applyPartial(&resolved, &requestedFormat, envValues)
	applyInputs(&resolved, &requestedFormat, inputs)

	effectiveFormat, err := outputformat.Resolve(requestedFormat, resolved.Robot)
	if err != nil {
		return Runtime{}, wrapError(ErrorKindInvalid, err)
	}

	if resolved.Timeout <= 0 {
		return Runtime{}, newError(ErrorKindInvalid, "timeout must be greater than zero")
	}

	if pathErr != nil {
		resolved.ConfigPath = ""
	}

	resolved.Format = effectiveFormat
	return resolved, nil
}

func applyPartial(target *Runtime, requestedFormat *string, value partialConfig) {
	if value.HostSet {
		target.Host = value.Host
	}
	if value.APITokenSet {
		target.APIToken = value.APIToken
	}
	if value.FormatSet {
		*requestedFormat = value.Format
	}
	if value.TimeoutSet {
		target.Timeout = value.Timeout
	}
	if value.RobotSet {
		target.Robot = value.Robot
	}
}

func applyInputs(target *Runtime, requestedFormat *string, inputs Inputs) {
	if inputs.Host.Set {
		target.Host = strings.TrimSpace(inputs.Host.Value)
	}
	if inputs.APIToken.Set {
		target.APIToken = strings.TrimSpace(inputs.APIToken.Value)
	}
	if inputs.Format.Set {
		*requestedFormat = inputs.Format.Value
	}
	if inputs.Timeout.Set {
		target.Timeout = inputs.Timeout.Value
	}
	if inputs.Robot.Set {
		target.Robot = inputs.Robot.Value
	}
}

func loadFile(path string) (partialConfig, error) {
	if path == "" {
		return partialConfig{}, nil
	}

	if _, err := os.Stat(path); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return partialConfig{}, nil
		}
		return partialConfig{}, wrapf(ErrorKindIO, "stat config file %q: %w", path, err)
	}

	var decoded fileConfig
	if _, err := toml.DecodeFile(path, &decoded); err != nil {
		return partialConfig{}, wrapf(ErrorKindInvalid, "decode config file %q: %w", path, err)
	}

	parsed := partialConfig{}

	if trimmed := strings.TrimSpace(decoded.Host); trimmed != "" {
		parsed.Host = trimmed
		parsed.HostSet = true
	}
	if trimmed := strings.TrimSpace(decoded.APIToken); trimmed != "" {
		parsed.APIToken = trimmed
		parsed.APITokenSet = true
	}
	if trimmed := strings.TrimSpace(decoded.Format); trimmed != "" {
		parsed.Format = trimmed
		parsed.FormatSet = true
	}
	if trimmed := strings.TrimSpace(decoded.Timeout); trimmed != "" {
		timeout, err := time.ParseDuration(trimmed)
		if err != nil {
			return partialConfig{}, wrapf(ErrorKindInvalid, "parse timeout in %q: %w", path, err)
		}
		parsed.Timeout = timeout
		parsed.TimeoutSet = true
	}
	if decoded.Robot != nil {
		parsed.Robot = *decoded.Robot
		parsed.RobotSet = true
	}

	return parsed, nil
}

func loadEnvironment() (partialConfig, error) {
	parsed := partialConfig{}

	if value, ok := lookupTrimmed(EnvHost); ok {
		parsed.Host = value
		parsed.HostSet = true
	}
	if value, ok := lookupTrimmed(EnvAPIToken); ok {
		parsed.APIToken = value
		parsed.APITokenSet = true
	}
	if value, ok := os.LookupEnv(EnvTimeout); ok && strings.TrimSpace(value) != "" {
		timeout, err := time.ParseDuration(strings.TrimSpace(value))
		if err != nil {
			return partialConfig{}, wrapf(ErrorKindInvalid, "parse %s: %w", EnvTimeout, err)
		}
		parsed.Timeout = timeout
		parsed.TimeoutSet = true
	}
	if value, ok := os.LookupEnv(EnvRobot); ok && strings.TrimSpace(value) != "" {
		robot, err := strconv.ParseBool(strings.TrimSpace(value))
		if err != nil {
			return partialConfig{}, wrapf(ErrorKindInvalid, "parse %s: %w", EnvRobot, err)
		}
		parsed.Robot = robot
		parsed.RobotSet = true
	}

	return parsed, nil
}

func lookupTrimmed(key string) (string, bool) {
	value, ok := os.LookupEnv(key)
	if !ok {
		return "", false
	}

	value = strings.TrimSpace(value)
	if value == "" {
		return "", false
	}

	return value, true
}

func newError(kind ErrorKind, message string) error {
	return Error{Kind: kind, Err: errors.New(message)}
}

func wrapError(kind ErrorKind, err error) error {
	if err == nil {
		return nil
	}

	return Error{Kind: kind, Err: err}
}

func wrapf(kind ErrorKind, format string, args ...any) error {
	return Error{Kind: kind, Err: fmt.Errorf(format, args...)}
}
