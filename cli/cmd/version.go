package cmd

import (
	"fmt"
	"runtime/debug"
)

var (
	Version = "0.0.0-dev"
	Commit  = "unknown"
	Date    = "unknown"
)

const (
	defaultVersion = "0.0.0-dev"
	defaultCommit  = "unknown"
	defaultDate    = "unknown"
)

// formattedVersion renders the CLI's version metadata for `--version` and
// `--robot-capabilities`. Release builds get Version/Commit/Date injected
// via goreleaser ldflags (see cli/.goreleaser.yaml); anything built without
// those flags (`go build`, `go install ...@version`, `go run .`) is left at
// the package defaults above, so it falls back to the Go module build info
// embedded by the toolchain.
func formattedVersion() string {
	version, commit, date := Version, Commit, Date
	if info, ok := debug.ReadBuildInfo(); ok {
		version, commit, date = resolveBuildInfoFallback(version, commit, date, info)
	}
	return fmt.Sprintf("%s\ncommit: %s\nbuilt: %s", version, commit, date)
}

// resolveBuildInfoFallback fills in any of version/commit/date that are
// still at their ldflags defaults using info, Go's embedded module build
// info. It is a pure function of its inputs so the fallback logic can be
// unit tested without depending on how the test binary itself was built.
func resolveBuildInfoFallback(version, commit, date string, info *debug.BuildInfo) (string, string, string) {
	if info == nil {
		return version, commit, date
	}

	if version == defaultVersion && info.Main.Version != "" && info.Main.Version != "(devel)" {
		version = info.Main.Version
	}

	if commit == defaultCommit {
		if revision, ok := buildSetting(info, "vcs.revision"); ok && revision != "" {
			if modified, _ := buildSetting(info, "vcs.modified"); modified == "true" {
				revision += "-dirty"
			}
			commit = revision
		}
	}

	if date == defaultDate {
		if buildTime, ok := buildSetting(info, "vcs.time"); ok && buildTime != "" {
			date = buildTime
		}
	}

	return version, commit, date
}

// buildSetting looks up a key in info.Settings (e.g. "vcs.revision").
func buildSetting(info *debug.BuildInfo, key string) (string, bool) {
	for _, setting := range info.Settings {
		if setting.Key == key {
			return setting.Value, true
		}
	}
	return "", false
}
