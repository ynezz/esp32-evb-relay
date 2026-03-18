package client

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

const apiBasePath = "/api/v1"

type Config struct {
	Host     string
	APIToken string
	Timeout  time.Duration
}

type Client struct {
	baseURL    *url.URL
	apiToken   string
	httpClient *http.Client
}

type Result struct {
	StatusCode    int
	Elapsed       time.Duration
	DeviceContext *DeviceContext
}

type DeviceContext struct {
	FirmwareVersion string `json:"firmware_version,omitempty"`
	ModIOPresent    *bool  `json:"modio_present,omitempty"`
	ModIOSync       string `json:"modio_sync,omitempty"`
}

func New(config Config) (*Client, error) {
	baseURL, err := normalizeBaseURL(config.Host)
	if err != nil {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, err)
	}

	timeout := config.Timeout
	switch {
	case timeout <= 0:
		return nil, exitcodes.Wrap(exitcodes.BadArgument, errors.New("timeout must be greater than zero"))
	}

	return &Client{
		baseURL:  baseURL,
		apiToken: config.APIToken,
		httpClient: &http.Client{
			Timeout: timeout,
		},
	}, nil
}

func (c *Client) DoJSON(ctx context.Context, method, endpoint string, requestBody any, responseBody any) (Result, error) {
	request, err := c.newRequest(ctx, method, endpoint, requestBody)
	if err != nil {
		return Result{}, err
	}

	startedAt := time.Now()
	response, err := c.httpClient.Do(request)
	elapsed := time.Since(startedAt)
	if err != nil {
		return Result{Elapsed: elapsed}, wrapError(err)
	}
	defer response.Body.Close()

	result := Result{
		StatusCode:    response.StatusCode,
		Elapsed:       elapsed,
		DeviceContext: deviceContextFromHeaders(response.Header),
	}

	payload, err := io.ReadAll(response.Body)
	if err != nil {
		return result, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("read response body: %w", err))
	}

	if response.StatusCode < http.StatusOK || response.StatusCode >= http.StatusMultipleChoices {
		return result, wrapError(decodeAPIError(response.StatusCode, payload))
	}

	if responseBody != nil && len(bytes.TrimSpace(payload)) > 0 {
		if err := json.Unmarshal(payload, responseBody); err != nil {
			return result, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("decode response body: %w", err))
		}
	}

	return result, nil
}

func (c *Client) newRequest(ctx context.Context, method, endpoint string, requestBody any) (*http.Request, error) {
	normalizedMethod := strings.ToUpper(strings.TrimSpace(method))
	if normalizedMethod == "" {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, errors.New("http method is required"))
	}

	endpointURL, err := c.endpointURL(endpoint)
	if err != nil {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, err)
	}

	var requestBodyReader io.Reader
	if requestBody != nil {
		payload, err := json.Marshal(requestBody)
		if err != nil {
			return nil, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("encode request body: %w", err))
		}
		requestBodyReader = bytes.NewReader(payload)
	}

	request, err := http.NewRequestWithContext(ctx, normalizedMethod, endpointURL.String(), requestBodyReader)
	if err != nil {
		return nil, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("create request: %w", err))
	}

	request.Header.Set("Accept", "application/json")
	if requestBody != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	if c.apiToken != "" {
		request.Header.Set("Authorization", "Bearer "+c.apiToken)
	}

	return request, nil
}

func (c *Client) endpointURL(endpoint string) (*url.URL, error) {
	trimmed := strings.TrimSpace(endpoint)
	if trimmed == "" {
		return nil, errors.New("endpoint path is required")
	}
	if strings.Contains(trimmed, "://") {
		return nil, errors.New("endpoint path must be relative")
	}

	trimmed = strings.TrimPrefix(trimmed, apiBasePath)
	trimmed = strings.TrimPrefix(trimmed, "/")

	relativeURL, err := url.Parse(trimmed)
	if err != nil {
		return nil, fmt.Errorf("parse endpoint path: %w", err)
	}

	baseURL := *c.baseURL
	if !strings.HasSuffix(baseURL.Path, "/") {
		baseURL.Path += "/"
	}

	return baseURL.ResolveReference(relativeURL), nil
}

func normalizeBaseURL(host string) (*url.URL, error) {
	trimmed := strings.TrimSpace(host)
	if trimmed == "" {
		return nil, errors.New("host is required")
	}
	if !strings.Contains(trimmed, "://") {
		trimmed = "http://" + trimmed
	}

	parsedURL, err := url.Parse(trimmed)
	if err != nil {
		return nil, fmt.Errorf("parse host: %w", err)
	}
	if parsedURL.Hostname() == "" {
		return nil, errors.New("host must include a hostname or IP address")
	}
	if parsedURL.RawQuery != "" || parsedURL.Fragment != "" {
		return nil, errors.New("host must not include query parameters or fragments")
	}

	normalizedPath := strings.TrimSuffix(parsedURL.Path, "/")
	switch normalizedPath {
	case "", "/":
		parsedURL.Path = apiBasePath
	case apiBasePath:
		parsedURL.Path = apiBasePath
	default:
		return nil, fmt.Errorf("host path must be empty or %s", apiBasePath)
	}

	parsedURL.RawQuery = ""
	parsedURL.Fragment = ""
	return parsedURL, nil
}

func deviceContextFromHeaders(headers http.Header) *DeviceContext {
	context := &DeviceContext{}

	if value := strings.TrimSpace(headers.Get("X-FW-Version")); value != "" {
		context.FirmwareVersion = value
	}
	if value := strings.TrimSpace(headers.Get("X-ModIO-Present")); value != "" {
		parsed, err := strconv.ParseBool(value)
		if err == nil {
			context.ModIOPresent = &parsed
		}
	}
	if value := strings.TrimSpace(headers.Get("X-ModIO-Sync")); value != "" {
		context.ModIOSync = value
	}

	if context.FirmwareVersion == "" && context.ModIOPresent == nil && context.ModIOSync == "" {
		return nil
	}

	return context
}
