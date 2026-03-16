package main

import (
    "fmt"
    "os"

    "example.com/esp32-evb-relay/cli/cmd"
    "example.com/esp32-evb-relay/cli/internal/exitcodes"
)

var version = "dev"

func main() {
    if err := cmd.Execute(version); err != nil {
        fmt.Fprintln(os.Stderr, err)
        os.Exit(int(exitcodes.FromError(err)))
    }
}
