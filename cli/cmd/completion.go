package cmd

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/ynezz/esp32-evb-relay/cli/internal/robot"
)

func newCompletionCommand() *cobra.Command {
	command := &cobra.Command{
		Use:       "completion [bash|zsh|fish|powershell]",
		Short:     "Generate shell completion scripts",
		Args:      cobra.ExactArgs(1),
		ValidArgs: []string{"bash", "zsh", "fish", "powershell"},
		RunE: func(cmd *cobra.Command, args []string) error {
			root := cmd.Root()

			switch args[0] {
			case "bash":
				return root.GenBashCompletionV2(cmd.OutOrStdout(), true)
			case "zsh":
				return root.GenZshCompletion(cmd.OutOrStdout())
			case "fish":
				return root.GenFishCompletion(cmd.OutOrStdout(), true)
			case "powershell":
				return root.GenPowerShellCompletionWithDesc(cmd.OutOrStdout())
			default:
				return fmt.Errorf("unsupported shell %q", args[0])
			}
		},
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Args:         []string{"shell"},
		OutputFields: []string{"script"},
		Example:      "evb-relay completion bash",
	})

	return command
}
