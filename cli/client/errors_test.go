package client

import (
	"net/http"
	"testing"

	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
)

// TestExitCodeMapsBadRequestToBadArgument is a regression test for
// evb-1ydi: the CLI's documented exit-code contract (README.md's
// "Exit Codes" section) maps bad-argument failures to exit code 5, but
// apiExitCode() had no case for HTTP 400 and fell through to the
// http.StatusServiceUnavailable/default branch, returning
// exitcodes.GeneralError (1) instead. That matched the 2026-03-25 manual
// rerun where `evb-relay config set poll_interval_ms=10` returned
// INVALID_CONFIG_VALUE (HTTP 400) with exit code 1.
func TestExitCodeMapsBadRequestToBadArgument(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		err  *APIError
		want exitcodes.Code
	}{
		{
			name: "invalid config value",
			err:  &APIError{Code: "INVALID_CONFIG_VALUE", Message: "poll_interval_ms must be >= 50", Status: http.StatusBadRequest},
			want: exitcodes.BadArgument,
		},
		{
			name: "bad request without a canonical code",
			err:  &APIError{Message: "malformed request", Status: http.StatusBadRequest},
			want: exitcodes.BadArgument,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()

			if got := ExitCode(tt.err); got != tt.want {
				t.Fatalf("ExitCode(%#v) = %d, want %d", tt.err, got, tt.want)
			}
		})
	}
}
