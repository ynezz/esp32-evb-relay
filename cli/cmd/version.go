package cmd

import "fmt"

var (
    Version = "0.0.0-dev"
    Commit  = "unknown"
    Date    = "unknown"
)

func formattedVersion() string {
	return fmt.Sprintf("%s\ncommit: %s\nbuilt: %s", Version, Commit, Date)
}
