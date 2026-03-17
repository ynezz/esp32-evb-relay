package main

import (
	"fmt"
	"os"

	"example.com/esp32-evb-relay/cli/cmd"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

func main() {
	if err := cmd.Execute(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(int(exitcodes.FromError(err)))
	}
}
