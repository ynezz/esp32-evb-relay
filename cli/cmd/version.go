package cmd

import "fmt"

var (
	Version = "dev"
	Commit  = "unknown"
	Date    = "unknown"
)

func formattedVersion() string {
	return fmt.Sprintf("%s\ncommit: %s\nbuilt: %s", Version, Commit, Date)
}
