package robot

import (
	"errors"
	"net"
	"net/url"
	"strings"

	"example.com/esp32-evb-relay/cli/client"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

const (
	authRemediation    = "Provide a valid API token via --api-token, EVB_RELAY_API_TOKEN, or the CLI config file"
	networkRemediation = "Retry the command; if the target host is stale or unknown, re-run evb-relay discover"
)

var networkNext = []string{"evb-relay discover"}

type errorPolicy struct {
	Retryable   bool
	Remediation *string
	Next        []string
}

type ErrorDetails struct {
	Code        string  `json:"code"`
	Message     string  `json:"message"`
	HTTPStatus  int     `json:"http_status,omitempty"`
	Retryable   bool    `json:"retryable"`
	Remediation *string `json:"remediation"`
}

type silentExitError struct {
	code exitcodes.Code
}

func (e silentExitError) Error() string {
	return ""
}

func (e silentExitError) ExitCode() exitcodes.Code {
	return e.code
}

func buildErrorDetails(err error) (ErrorDetails, exitcodes.Code, []string) {
	exitCode := client.ExitCode(err)
	details := ErrorDetails{
		Code:      fallbackErrorCode(exitCode),
		Message:   strings.TrimSpace(err.Error()),
		Retryable: false,
	}

	var apiErr *client.APIError
	if errors.As(err, &apiErr) {
		if code := strings.ToUpper(strings.TrimSpace(apiErr.Code)); code != "" {
			details.Code = code
		} else {
			details.Code = apiStatusCode(apiErr.Status, exitCode)
		}
		if apiErr.Message != "" {
			details.Message = apiErr.Message
		}
		if apiErr.Status != 0 {
			details.HTTPStatus = apiErr.Status
		}
	}

	if isNetworkError(err) {
		details.Retryable = true
		details.Remediation = stringPtr(networkRemediation)
		return details, exitCode, append([]string(nil), networkNext...)
	}

	if policy, ok := knownErrorPolicies()[details.Code]; ok {
		details.Retryable = policy.Retryable
		details.Remediation = policy.Remediation
		return details, exitCode, append([]string(nil), policy.Next...)
	}

	return details, exitCode, nil
}

func knownErrorPolicies() map[string]errorPolicy {
	return map[string]errorPolicy{
		"MODIO_NOT_PRESENT": {
			Retryable: false,
		},
		"MODIO_SAMPLE_UNAVAILABLE": {
			Retryable: true,
		},
		"RELAY_NOT_FOUND": {
			Retryable: false,
		},
		"INPUT_NOT_FOUND": {
			Retryable: false,
		},
		"AUTH_REQUIRED": {
			Retryable:   false,
			Remediation: stringPtr(authRemediation),
		},
		"AUTH_FORBIDDEN": {
			Retryable:   false,
			Remediation: stringPtr(authRemediation),
		},
		"AUTH_INVALID": {
			Retryable:   false,
			Remediation: stringPtr(authRemediation),
		},
		"PARTIAL_FAILURE": {
			Retryable: false,
		},
	}
}

func fallbackErrorCode(code exitcodes.Code) string {
	switch code {
	case exitcodes.NetworkError:
		return "NETWORK_ERROR"
	case exitcodes.AuthError:
		return "AUTH_ERROR"
	case exitcodes.NotFound:
		return "NOT_FOUND"
	case exitcodes.BadArgument:
		return "BAD_ARGUMENT"
	case exitcodes.StateError:
		return "STATE_ERROR"
	case exitcodes.HardwareUnavailable:
		return "HARDWARE_UNAVAILABLE"
	default:
		return "GENERAL_ERROR"
	}
}

func apiStatusCode(status int, code exitcodes.Code) string {
	switch status {
	case 401:
		return "AUTH_REQUIRED"
	case 403:
		return "AUTH_FORBIDDEN"
	default:
		return fallbackErrorCode(code)
	}
}

func isNetworkError(err error) bool {
	var urlErr *url.Error
	if errors.As(err, &urlErr) {
		return true
	}

	var netErr net.Error
	return errors.As(err, &netErr)
}

func stringPtr(value string) *string {
	return &value
}
