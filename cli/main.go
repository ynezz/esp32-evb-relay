package main

import (
	"fmt"
	"os"
	"strings"

	"example.com/esp32-evb-relay/cli/cmd"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

func main() {
	if err := cmd.Execute(); err != nil {
		if message := strings.TrimSpace(err.Error()); message != "" {
			fmt.Fprintln(os.Stderr, message)
		}
		os.Exit(int(exitcodes.FromError(err)))
	}
}
