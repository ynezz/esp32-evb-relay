package robot

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"

	"github.com/spf13/cobra"

	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	outputformat "example.com/esp32-evb-relay/cli/internal/format"
)

const capabilityAnnotationKey = "robot.capability"

type CommandCapability struct {
	Name         string   `json:"name,omitempty"`
	Description  string   `json:"description,omitempty"`
	Args         []string `json:"args,omitempty"`
	Flags        []string `json:"flags,omitempty"`
	OutputFields []string `json:"output_fields,omitempty"`
	Errors       []string `json:"errors,omitempty"`
	Example      string   `json:"example,omitempty"`
}

type Capabilities struct {
	V                  int                        `json:"v"`
	CLIVersion         string                     `json:"cli_version"`
	EnvelopeVersion    int                        `json:"envelope_version"`
	DefaultRobotFormat string                     `json:"default_robot_format"`
	Commands           []CommandCapability        `json:"commands"`
	ExitCodes          map[string]string          `json:"exit_codes"`
	ErrorCodes         map[string]CapabilityError `json:"error_codes"`
	StateMachine       StateMachineCapabilities   `json:"state_machine"`
	EnvironmentVars    []EnvironmentVariable      `json:"environment_variables"`
}

type CapabilityError struct {
	ExitCode    int     `json:"exit_code"`
	Retryable   bool    `json:"retryable"`
	Remediation *string `json:"remediation"`
}

type StateMachineCapabilities struct {
	ModIOSyncStates []string          `json:"modio_sync_states"`
	Transitions     map[string]string `json:"transitions"`
	BootHint        string            `json:"boot_hint"`
}

type EnvironmentVariable struct {
	Name        string `json:"name"`
	Description string `json:"description"`
}

func AnnotateCommand(cmd *cobra.Command, capability CommandCapability) {
	payload, err := json.Marshal(capability)
	if err != nil {
		panic(fmt.Errorf("marshal robot capability for %q: %w", cmd.CommandPath(), err))
	}

	if cmd.Annotations == nil {
		cmd.Annotations = make(map[string]string)
	}
	cmd.Annotations[capabilityAnnotationKey] = string(payload)
}

func BuildCapabilities(root *cobra.Command, version string) (Capabilities, error) {
	commands, err := collectCommandCapabilities(root)
	if err != nil {
		return Capabilities{}, err
	}

	return Capabilities{
		V:                  1,
		CLIVersion:         version,
		EnvelopeVersion:    EnvelopeVersion,
		DefaultRobotFormat: outputformat.TOON,
		Commands:           commands,
		ExitCodes: map[string]string{
			"0": "success",
			"1": "general error",
			"2": "network error",
			"3": "auth error",
			"4": "not found",
			"5": "bad argument",
			"6": "state error (reserved for future device-state conflicts)",
			"7": "hardware unavailable",
		},
		ErrorCodes: map[string]CapabilityError{
			"AUTH_FORBIDDEN": {
				ExitCode:    3,
				Retryable:   false,
				Remediation: stringPtr(authRemediation),
			},
			"AUTH_INVALID": {
				ExitCode:    3,
				Retryable:   false,
				Remediation: stringPtr(authRemediation),
			},
			"AUTH_REQUIRED": {
				ExitCode:    3,
				Retryable:   false,
				Remediation: stringPtr(authRemediation),
			},
			"MODIO_NOT_PRESENT": {
				ExitCode:  7,
				Retryable: false,
			},
			"MODIO_SAMPLE_UNAVAILABLE": {
				ExitCode:  7,
				Retryable: true,
			},
			"NETWORK_ERROR": {
				ExitCode:    2,
				Retryable:   true,
				Remediation: stringPtr(networkRemediation),
			},
			"PARTIAL_FAILURE": {
				ExitCode:  1,
				Retryable: false,
			},
			"RELAY_NOT_FOUND": {
				ExitCode:  4,
				Retryable: false,
			},
		},
		StateMachine: StateMachineCapabilities{
			ModIOSyncStates: []string{"synchronized", "absent"},
			Transitions: map[string]string{
				"absent -> synchronized":       "MOD-IO is physically connected and relay-state readback succeeds",
				"synchronized -> synchronized": "ESP32 reboots or MOD-IO reconnects and the firmware refreshes relay state from readback",
				"* -> absent":                  "MOD-IO is physically disconnected or probe/readback fails",
			},
			BootHint: "No bulk recovery step is required; the firmware refreshes the authoritative relay bitmap from MOD-IO during init",
		},
		EnvironmentVars: []EnvironmentVariable{
			{Name: appconfig.EnvHost, Description: "Device IP or hostname"},
			{Name: appconfig.EnvAPIToken, Description: "API authentication token"},
			{Name: appconfig.EnvRobot, Description: "Enable robot output mode"},
			{Name: appconfig.EnvTimeout, Description: "HTTP request timeout"},
		},
	}, nil
}

func collectCommandCapabilities(root *cobra.Command) ([]CommandCapability, error) {
	capabilities := make([]CommandCapability, 0)

	var walk func(*cobra.Command) error
	walk = func(cmd *cobra.Command) error {
		if capability, ok, err := commandCapability(root, cmd); err != nil {
			return err
		} else if ok {
			capabilities = append(capabilities, capability)
		}

		children := append([]*cobra.Command(nil), cmd.Commands()...)
		sort.Slice(children, func(left, right int) bool {
			return children[left].CommandPath() < children[right].CommandPath()
		})
		for _, child := range children {
			if err := walk(child); err != nil {
				return err
			}
		}

		return nil
	}

	if err := walk(root); err != nil {
		return nil, err
	}

	sort.Slice(capabilities, func(left, right int) bool {
		return capabilities[left].Name < capabilities[right].Name
	})
	return capabilities, nil
}

func commandCapability(root *cobra.Command, cmd *cobra.Command) (CommandCapability, bool, error) {
	if cmd.Annotations == nil {
		return CommandCapability{}, false, nil
	}

	payload, ok := cmd.Annotations[capabilityAnnotationKey]
	if !ok {
		return CommandCapability{}, false, nil
	}

	var capability CommandCapability
	if err := json.Unmarshal([]byte(payload), &capability); err != nil {
		return CommandCapability{}, false, fmt.Errorf("decode robot capability for %q: %w", cmd.CommandPath(), err)
	}

	if capability.Name == "" {
		capability.Name = trimmedCommandPath(root, cmd)
	}
	if capability.Description == "" {
		capability.Description = cmd.Short
	}
	if capability.Example == "" && capability.Name != "" {
		capability.Example = strings.TrimSpace(root.Name() + " " + capability.Name)
	}

	return capability, true, nil
}

func trimmedCommandPath(root *cobra.Command, cmd *cobra.Command) string {
	parts := strings.Fields(cmd.CommandPath())
	if len(parts) > 0 && parts[0] == root.Name() {
		parts = parts[1:]
	}
	return strings.Join(parts, " ")
}
