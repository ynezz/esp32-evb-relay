package cmd

import (
	"runtime/debug"
	"strings"
	"testing"
)

func TestResolveBuildInfoFallbackNilInfoReturnsInputsUnchanged(t *testing.T) {
	t.Parallel()

	version, commit, date := resolveBuildInfoFallback(defaultVersion, defaultCommit, defaultDate, nil)
	if version != defaultVersion || commit != defaultCommit || date != defaultDate {
		t.Fatalf("resolveBuildInfoFallback(nil) = (%q, %q, %q), want inputs unchanged", version, commit, date)
	}
}

func TestResolveBuildInfoFallbackAppliesModuleVersion(t *testing.T) {
	t.Parallel()

	info := &debug.BuildInfo{
		Main: debug.Module{Version: "v1.4.0"},
	}

	version, commit, date := resolveBuildInfoFallback(defaultVersion, defaultCommit, defaultDate, info)
	if version != "v1.4.0" {
		t.Fatalf("version = %q, want %q", version, "v1.4.0")
	}
	if commit != defaultCommit {
		t.Fatalf("commit = %q, want unchanged %q", commit, defaultCommit)
	}
	if date != defaultDate {
		t.Fatalf("date = %q, want unchanged %q", date, defaultDate)
	}
}

func TestResolveBuildInfoFallbackIgnoresDevelMainVersion(t *testing.T) {
	t.Parallel()

	info := &debug.BuildInfo{
		Main: debug.Module{Version: "(devel)"},
	}

	version, _, _ := resolveBuildInfoFallback(defaultVersion, defaultCommit, defaultDate, info)
	if version != defaultVersion {
		t.Fatalf("version = %q, want unchanged %q", version, defaultVersion)
	}
}

func TestResolveBuildInfoFallbackAppliesCleanVCSRevision(t *testing.T) {
	t.Parallel()

	info := &debug.BuildInfo{
		Settings: []debug.BuildSetting{
			{Key: "vcs.revision", Value: "abc1234"},
			{Key: "vcs.modified", Value: "false"},
			{Key: "vcs.time", Value: "2026-09-24T00:00:00Z"},
		},
	}

	version, commit, date := resolveBuildInfoFallback(defaultVersion, defaultCommit, defaultDate, info)
	if version != defaultVersion {
		t.Fatalf("version = %q, want unchanged %q", version, defaultVersion)
	}
	if commit != "abc1234" {
		t.Fatalf("commit = %q, want %q", commit, "abc1234")
	}
	if date != "2026-09-24T00:00:00Z" {
		t.Fatalf("date = %q, want %q", date, "2026-09-24T00:00:00Z")
	}
}

func TestResolveBuildInfoFallbackAppendsDirtySuffixWhenModified(t *testing.T) {
	t.Parallel()

	info := &debug.BuildInfo{
		Settings: []debug.BuildSetting{
			{Key: "vcs.revision", Value: "abc1234"},
			{Key: "vcs.modified", Value: "true"},
		},
	}

	_, commit, _ := resolveBuildInfoFallback(defaultVersion, defaultCommit, defaultDate, info)
	if commit != "abc1234-dirty" {
		t.Fatalf("commit = %q, want %q", commit, "abc1234-dirty")
	}
}

func TestResolveBuildInfoFallbackLeavesInjectedMetadataAlone(t *testing.T) {
	t.Parallel()

	info := &debug.BuildInfo{
		Main: debug.Module{Version: "v9.9.9"},
		Settings: []debug.BuildSetting{
			{Key: "vcs.revision", Value: "deadbeef"},
			{Key: "vcs.modified", Value: "true"},
			{Key: "vcs.time", Value: "2026-09-24T00:00:00Z"},
		},
	}

	version, commit, date := resolveBuildInfoFallback("1.2.3", "abc123", "2026-03-17T00:00:00Z", info)
	if version != "1.2.3" {
		t.Fatalf("version = %q, want injected value preserved %q", version, "1.2.3")
	}
	if commit != "abc123" {
		t.Fatalf("commit = %q, want injected value preserved %q", commit, "abc123")
	}
	if date != "2026-03-17T00:00:00Z" {
		t.Fatalf("date = %q, want injected value preserved %q", date, "2026-03-17T00:00:00Z")
	}
}

func TestBuildSettingReturnsFalseWhenKeyMissing(t *testing.T) {
	t.Parallel()

	info := &debug.BuildInfo{}
	if _, ok := buildSetting(info, "vcs.revision"); ok {
		t.Fatal("buildSetting() ok = true, want false for empty settings")
	}
}

func TestFormattedVersionFallsBackToBuildInfoWhenAtDefaults(t *testing.T) {
	withVersionMetadata(defaultVersion, defaultCommit, defaultDate, func() {
		// The test binary is itself a Go build, so debug.ReadBuildInfo()
		// succeeds here; this only checks formattedVersion() exercises the
		// fallback path without panicking and keeps the expected shape.
		// The exact commit/date values depend on how `go test` built the
		// binary, so resolveBuildInfoFallback's own tests above cover the
		// value-level assertions.
		got := formattedVersion()
		if got == "" {
			t.Fatal("formattedVersion() = \"\", want non-empty output")
		}
		if !strings.Contains(got, "commit: ") || !strings.Contains(got, "built: ") {
			t.Fatalf("formattedVersion() = %q, want commit/built fields present", got)
		}
	})
}
