package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/ynezz/esp32-evb-relay/cli/cmd"
	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
)

func main() {
	if err := cmd.Execute(); err != nil {
		if message := strings.TrimSpace(err.Error()); message != "" {
			fmt.Fprintln(os.Stderr, message)
		}
		os.Exit(int(exitcodes.FromError(err)))
	}
}
