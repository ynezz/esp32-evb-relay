package client

import (
	"bufio"
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

type StreamEvent struct {
	Event string
	Data  any
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
	var requestBodyReader io.Reader
	contentLength := int64(-1)
	if requestBody != nil {
		payload, err := json.Marshal(requestBody)
		if err != nil {
			return Result{}, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("encode request body: %w", err))
		}
		requestBodyReader = bytes.NewReader(payload)
		contentLength = int64(len(payload))
	}

	request, err := c.newRequest(ctx, method, endpoint, requestBodyReader, "application/json", "application/json", contentLength)
	if err != nil {
		return Result{}, err
	}

	return c.do(request, responseBody)
}

func (c *Client) Host() string {
	return c.baseURL.Host
}

func (c *Client) UploadBinary(ctx context.Context, endpoint string, body io.Reader, size int64, responseBody any) (Result, error) {
	if body == nil {
		return Result{}, exitcodes.Wrap(exitcodes.BadArgument, errors.New("upload body is required"))
	}
	if size <= 0 {
		return Result{}, exitcodes.Wrap(exitcodes.BadArgument, errors.New("upload size must be greater than zero"))
	}

	request, err := c.newRequest(ctx, http.MethodPost, endpoint, body, "application/octet-stream", "application/json", size)
	if err != nil {
		return Result{}, err
	}

	return c.do(request, responseBody)
}

func (c *Client) WatchEventsOnce(
	ctx context.Context,
	onStart func(Result) error,
	onEvent func(StreamEvent) error,
) error {
	if onStart == nil {
		return exitcodes.Wrap(exitcodes.BadArgument, errors.New("stream start callback is required"))
	}
	if onEvent == nil {
		return exitcodes.Wrap(exitcodes.BadArgument, errors.New("stream event callback is required"))
	}

	request, err := c.newRequest(ctx, http.MethodGet, "/events", nil, "", "text/event-stream", -1)
	if err != nil {
		return err
	}

	streamHTTPClient := *c.httpClient
	streamHTTPClient.Timeout = 0

	response, err := streamHTTPClient.Do(request)
	if err != nil {
		return wrapError(err)
	}
	defer response.Body.Close()

	result := Result{
		StatusCode:    response.StatusCode,
		DeviceContext: deviceContextFromHeaders(response.Header),
	}

	if response.StatusCode < http.StatusOK || response.StatusCode >= http.StatusMultipleChoices {
		payload, readErr := io.ReadAll(response.Body)
		if readErr != nil {
			return exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("read response body: %w", readErr))
		}
		return wrapError(decodeAPIError(response.StatusCode, payload))
	}

	if err := onStart(result); err != nil {
		return err
	}

	return decodeEventStream(ctx, response.Body, onEvent)
}

func (c *Client) do(request *http.Request, responseBody any) (Result, error) {
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

func (c *Client) newRequest(
	ctx context.Context,
	method string,
	endpoint string,
	requestBody io.Reader,
	contentType string,
	accept string,
	contentLength int64,
) (*http.Request, error) {
	normalizedMethod := strings.ToUpper(strings.TrimSpace(method))
	if normalizedMethod == "" {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, errors.New("http method is required"))
	}

	endpointURL, err := c.endpointURL(endpoint)
	if err != nil {
		return nil, exitcodes.Wrap(exitcodes.BadArgument, err)
	}

	request, err := http.NewRequestWithContext(ctx, normalizedMethod, endpointURL.String(), requestBody)
	if err != nil {
		return nil, exitcodes.Wrap(exitcodes.GeneralError, fmt.Errorf("create request: %w", err))
	}

	if strings.TrimSpace(accept) == "" {
		accept = "application/json"
	}
	request.Header.Set("Accept", accept)
	if strings.TrimSpace(contentType) != "" {
		request.Header.Set("Content-Type", contentType)
	}
	if contentLength >= 0 {
		request.ContentLength = contentLength
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
	deviceContext := &DeviceContext{}

	if value := strings.TrimSpace(headers.Get("X-FW-Version")); value != "" {
		deviceContext.FirmwareVersion = value
	}
	if value := strings.TrimSpace(headers.Get("X-ModIO-Present")); value != "" {
		parsed, err := strconv.ParseBool(value)
		if err == nil {
			deviceContext.ModIOPresent = &parsed
		}
	}
	if value := strings.TrimSpace(headers.Get("X-ModIO-Sync")); value != "" {
		deviceContext.ModIOSync = value
	}

	if deviceContext.FirmwareVersion == "" && deviceContext.ModIOPresent == nil && deviceContext.ModIOSync == "" {
		return nil
	}

	return deviceContext
}

func decodeEventStream(ctx context.Context, body io.Reader, onEvent func(StreamEvent) error) error {
	scanner := bufio.NewScanner(body)
	eventName := ""
	dataLines := make([]string, 0, 1)

	emit := func() error {
		if len(dataLines) == 0 {
			eventName = ""
			return nil
		}

		payload := strings.Join(dataLines, "\n")
		data := any(payload)
		if json.Valid([]byte(payload)) {
			data = json.RawMessage(payload)
		}

		name := eventName
		if name == "" {
			name = "message"
		}

		eventName = ""
		dataLines = dataLines[:0]
		return onEvent(StreamEvent{
			Event: name,
			Data:  data,
		})
	}

	for scanner.Scan() {
		line := strings.TrimRight(scanner.Text(), "\r")
		if line == "" {
			if err := emit(); err != nil {
				return err
			}
			continue
		}
		if strings.HasPrefix(line, ":") {
			continue
		}

		field, value, found := strings.Cut(line, ":")
		if !found {
			value = ""
		}
		value = strings.TrimPrefix(value, " ")

		switch field {
		case "event":
			eventName = value
		case "data":
			dataLines = append(dataLines, value)
		}
	}

	if err := scanner.Err(); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return wrapError(err)
	}

	if err := emit(); err != nil {
		return err
	}

	if ctx.Err() != nil {
		return ctx.Err()
	}

	return io.EOF
}
