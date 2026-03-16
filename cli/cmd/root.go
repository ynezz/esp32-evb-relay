package cmd

import (
    "context"
    "encoding/json"
    "errors"
    "time"

    "github.com/spf13/cobra"

    appconfig "example.com/esp32-evb-relay/cli/internal/config"
    "example.com/esp32-evb-relay/cli/internal/exitcodes"
    outputformat "example.com/esp32-evb-relay/cli/internal/format"
)

type configContextKey struct{}

type persistentFlags struct {
    host              string
    apiToken          string
    format            string
    timeout           time.Duration
    robot             bool
    robotCapabilities bool
}

type flagDescriptor struct {
    Name        string `json:"name"`
    Shorthand   string `json:"shorthand,omitempty"`
    Type        string `json:"type"`
    Default     string `json:"default,omitempty"`
    Environment string `json:"environment,omitempty"`
    Description string `json:"description"`
}

type cliCapabilities struct {
    Name             string           `json:"name"`
    Version          string           `json:"version"`
    ConfigPath       string           `json:"config_path,omitempty"`
    ConfigPrecedence []string         `json:"config_precedence"`
    HumanFormats     []string         `json:"human_formats"`
    RobotDefault     string           `json:"robot_default_format"`
    RobotOverrides   []string         `json:"robot_allowed_overrides"`
    PlannedCommands  []string         `json:"planned_commands"`
    GlobalFlags      []flagDescriptor `json:"global_flags"`
}

func Execute(version string) error {
    return newRootCommand(version).Execute()
}

func newRootCommand(version string) *cobra.Command {
    flags := persistentFlags{
        timeout: appconfig.DefaultTimeout,
    }

    cmd := &cobra.Command{
        Use:           "evb-relay",
        Short:         "Control the ESP32-EVB relay controller",
        SilenceErrors: true,
        SilenceUsage:  true,
        Args:          cobra.NoArgs,
        Version:       version,
        PersistentPreRunE: func(cmd *cobra.Command, _ []string) error {
            if versionRequested(cmd) {
                return nil
            }

            resolved, err := appconfig.Resolve(appconfig.Inputs{
                Host: appconfig.StringInput{
                    Value: flags.host,
                    Set:   cmd.Flags().Changed("host"),
                },
                APIToken: appconfig.StringInput{
                    Value: flags.apiToken,
                    Set:   cmd.Flags().Changed("api-token"),
                },
                Format: appconfig.StringInput{
                    Value: flags.format,
                    Set:   cmd.Flags().Changed("format"),
                },
                Timeout: appconfig.DurationInput{
                    Value: flags.timeout,
                    Set:   cmd.Flags().Changed("timeout"),
                },
                Robot: appconfig.BoolInput{
                    Value: flags.robot,
                    Set:   cmd.Flags().Changed("robot"),
                },
            })
            if err != nil {
                return classifyConfigError(err)
            }

            currentContext := cmd.Context()
            if currentContext == nil {
                currentContext = context.Background()
            }

            cmd.SetContext(context.WithValue(currentContext, configContextKey{}, resolved))
            return nil
        },
        RunE: func(cmd *cobra.Command, _ []string) error {
            if flags.robotCapabilities {
                return printCapabilities(cmd, version)
            }

            return cmd.Help()
        },
    }

    cmd.SetVersionTemplate("{{printf \"%s\\n\" .Version}}")

    cmd.PersistentFlags().StringVarP(&flags.host, "host", "H", "", "Device IP or hostname")
    cmd.PersistentFlags().StringVarP(&flags.apiToken, "api-token", "k", "", "API authentication token")
    cmd.PersistentFlags().StringVarP(&flags.format, "format", "f", "", "Output format: table, json, plain; robot mode defaults to TOON and only accepts json as an explicit override")
    cmd.PersistentFlags().DurationVarP(&flags.timeout, "timeout", "t", appconfig.DefaultTimeout, "HTTP timeout")
    cmd.PersistentFlags().BoolVar(&flags.robot, "robot", false, "Enable robot mode output")
    cmd.PersistentFlags().BoolVar(&flags.robotCapabilities, "robot-capabilities", false, "Dump CLI capabilities as JSON and exit")

    return cmd
}

func printCapabilities(cmd *cobra.Command, version string) error {
    capabilities := cliCapabilities{
        Name:             "evb-relay",
        Version:          version,
        ConfigPath:       configPath(cmd),
        ConfigPrecedence: []string{"flags", "environment", "config_file"},
        HumanFormats:     outputformat.HumanFormats(),
        RobotDefault:     outputformat.TOON,
        RobotOverrides:   []string{outputformat.JSON},
        PlannedCommands:  []string{"relay", "input", "status", "config", "discover", "ota", "completion"},
        GlobalFlags: []flagDescriptor{
            {
                Name:        "host",
                Shorthand:   "H",
                Type:        "string",
                Environment: appconfig.EnvHost,
                Description: "Device IP or hostname",
            },
            {
                Name:        "api-token",
                Shorthand:   "k",
                Type:        "string",
                Environment: appconfig.EnvAPIToken,
                Description: "API authentication token",
            },
            {
                Name:        "format",
                Shorthand:   "f",
                Type:        "string",
                Default:     outputformat.Table,
                Description: "Human output format: table, json, or plain",
            },
            {
                Name:        "timeout",
                Shorthand:   "t",
                Type:        "duration",
                Default:     appconfig.DefaultTimeout.String(),
                Environment: appconfig.EnvTimeout,
                Description: "HTTP request timeout",
            },
            {
                Name:        "robot",
                Type:        "bool",
                Default:     "false",
                Environment: appconfig.EnvRobot,
                Description: "Enable robot output mode",
            },
            {
                Name:        "robot-capabilities",
                Type:        "bool",
                Default:     "false",
                Description: "Print the CLI contract as JSON and exit",
            },
            {
                Name:        "version",
                Type:        "bool",
                Default:     "false",
                Description: "Print version metadata and exit",
            },
        },
    }

    encoder := json.NewEncoder(cmd.OutOrStdout())
    encoder.SetIndent("", "  ")
    return encoder.Encode(capabilities)
}

func configPath(cmd *cobra.Command) string {
    if resolved, ok := ConfigFromContext(cmd); ok {
        return resolved.ConfigPath
    }

    path, err := appconfig.DefaultPath()
    if err != nil {
        return ""
    }

    return path
}

func ConfigFromContext(cmd *cobra.Command) (appconfig.Runtime, bool) {
    if cmd.Context() == nil {
        return appconfig.Runtime{}, false
    }

    resolved, ok := cmd.Context().Value(configContextKey{}).(appconfig.Runtime)
    return resolved, ok
}

func classifyConfigError(err error) error {
    var configErr appconfig.Error
    if errors.As(err, &configErr) {
        if configErr.Kind == appconfig.ErrorKindIO {
            return exitcodes.Wrap(exitcodes.GeneralError, err)
        }
        return exitcodes.Wrap(exitcodes.BadArgument, err)
    }

    return exitcodes.Wrap(exitcodes.GeneralError, err)
}

func versionRequested(cmd *cobra.Command) bool {
    flag := cmd.Flags().Lookup("version")
    if flag == nil {
        return false
    }

    return flag.Changed
}
