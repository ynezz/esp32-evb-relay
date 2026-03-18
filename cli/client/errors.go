package client

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"

	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

type APIError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Status  int    `json:"status"`
}

type errorEnvelope struct {
	Error *APIError `json:"error"`
}

func (e *APIError) Error() string {
	switch {
	case e == nil:
		return "api error"
	case e.Code != "" && e.Message != "":
		return fmt.Sprintf("%s: %s", e.Code, e.Message)
	case e.Message != "":
		return e.Message
	case e.Code != "":
		return e.Code
	default:
		return "api error"
	}
}

func ExitCode(err error) exitcodes.Code {
	if err == nil {
		return exitcodes.Success
	}

	var apiErr *APIError
	if errors.As(err, &apiErr) {
		return apiExitCode(apiErr)
	}

	var urlErr *url.Error
	if errors.As(err, &urlErr) {
		return exitcodes.NetworkError
	}

	var networkErr net.Error
	if errors.As(err, &networkErr) {
		return exitcodes.NetworkError
	}

	return exitcodes.FromError(err)
}

func decodeAPIError(statusCode int, payload []byte) error {
	var envelope errorEnvelope
	if len(payload) > 0 {
		if err := json.Unmarshal(payload, &envelope); err == nil && envelope.Error != nil {
			if envelope.Error.Status == 0 {
				envelope.Error.Status = statusCode
			}
			return envelope.Error
		}
	}

	message := strings.TrimSpace(string(payload))
	if message == "" {
		message = http.StatusText(statusCode)
	}

	return &APIError{
		Message: message,
		Status:  statusCode,
	}
}

func apiExitCode(err *APIError) exitcodes.Code {
	if err == nil {
		return exitcodes.GeneralError
	}

	switch strings.ToUpper(err.Code) {
	case "MODIO_STATE_UNKNOWN":
		return exitcodes.StateError
	case "MODIO_NOT_PRESENT", "MODIO_SAMPLE_UNAVAILABLE":
		return exitcodes.HardwareUnavailable
	}

	switch err.Status {
	case http.StatusUnauthorized, http.StatusForbidden:
		return exitcodes.AuthError
	case http.StatusNotFound:
		return exitcodes.NotFound
	case http.StatusConflict:
		return exitcodes.StateError
	case http.StatusServiceUnavailable:
		return exitcodes.HardwareUnavailable
	default:
		return exitcodes.GeneralError
	}
}

func wrapError(err error) error {
	return exitcodes.Wrap(ExitCode(err), err)
}
