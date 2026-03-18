package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"time"

	"github.com/spf13/cobra"

	appconfig "example.com/esp32-evb-relay/cli/internal/config"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	"example.com/esp32-evb-relay/cli/internal/robot"
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

func Execute() error {
	return newRootCommand().Execute()
}

func newRootCommand() *cobra.Command {
	cobra.EnableTraverseRunHooks = true

	flags := persistentFlags{
		timeout: appconfig.DefaultTimeout,
	}

	cmd := &cobra.Command{
		Use:           "evb-relay",
		Short:         "Control the ESP32-EVB relay controller",
		SilenceErrors: true,
		SilenceUsage:  true,
		Args:          cobra.NoArgs,
		Version:       formattedVersion(),
		PersistentPreRunE: func(cmd *cobra.Command, _ []string) error {
			if versionRequested(cmd) || capabilitiesRequested(cmd) || completionRequested(cmd) {
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
				return printCapabilities(cmd)
			}

			return cmd.Help()
		},
	}

	cmd.SetVersionTemplate("{{printf \"%s\\n\" .Version}}")
	cmd.AddCommand(
		newCompletionCommand(),
		newConfigCommand(),
		newDiscoverCommand(),
		newInputCommand(),
		newOTACommand(),
		newStatusCommand(),
	)

	cmd.PersistentFlags().StringVarP(&flags.host, "host", "H", "", "Device IP or hostname")
	cmd.PersistentFlags().StringVarP(&flags.apiToken, "api-token", "k", "", "API authentication token")
	cmd.PersistentFlags().StringVarP(&flags.format, "format", "f", "", "Output format: table, json, plain; robot mode defaults to TOON and only accepts json as an explicit override")
	cmd.PersistentFlags().DurationVarP(&flags.timeout, "timeout", "t", appconfig.DefaultTimeout, "HTTP timeout")
	cmd.PersistentFlags().BoolVar(&flags.robot, "robot", false, "Enable robot mode output")
	cmd.PersistentFlags().BoolVar(&flags.robotCapabilities, "robot-capabilities", false, "Dump CLI capabilities as JSON and exit")

	return cmd
}

func printCapabilities(cmd *cobra.Command) error {
	capabilities, err := robot.BuildCapabilities(cmd.Root(), Version)
	if err != nil {
		return exitcodes.Wrap(exitcodes.GeneralError, err)
	}

	encoder := json.NewEncoder(cmd.OutOrStdout())
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	return encoder.Encode(capabilities)
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

func capabilitiesRequested(cmd *cobra.Command) bool {
	flag := cmd.Flags().Lookup("robot-capabilities")
	if flag == nil {
		return false
	}

	return flag.Changed
}

func completionRequested(cmd *cobra.Command) bool {
	for current := cmd; current != nil; current = current.Parent() {
		if current.Name() == "completion" {
			return true
		}
	}

	return false
}
