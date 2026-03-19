package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync"
	"testing"
	"time"

	"example.com/esp32-evb-relay/cli/internal/exitcodes"
)

func TestInputWatchOutputsNDJSONStream(t *testing.T) {
	t.Parallel()

	eventsWritten := make(chan struct{})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/events" {
			t.Errorf("request path = %q, want %q", r.URL.Path, "/api/v1/events")
			http.Error(w, "unexpected path", http.StatusNotFound)
			return
		}
		if got := r.Header.Get("Accept"); got != "text/event-stream" {
			t.Errorf("Accept header = %q, want %q", got, "text/event-stream")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer stream-token" {
			t.Errorf("Authorization header = %q, want %q", got, "Bearer stream-token")
		}

		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("X-FW-Version", "0.3.1")
		w.Header().Set("X-ModIO-Present", "true")
		w.Header().Set("X-ModIO-Sync", "synchronized")

		flusher, ok := w.(http.Flusher)
		if !ok {
			t.Error("response writer does not support flushing")
			return
		}

		_, _ = w.Write([]byte("event: digital_input\n"))
		_, _ = w.Write([]byte("data: {\"id\":2,\"state\":true,\"ts_ms\":12345}\n\n"))
		_, _ = w.Write([]byte("event: heartbeat\n"))
		_, _ = w.Write([]byte("data: {\"ts_ms\":42345}\n\n"))
		flusher.Flush()
		close(eventsWritten)

		<-r.Context().Done()
	}))
	defer server.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	command := newRootCommand()
	stdout := newObservedBuffer()
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetContext(ctx)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "stream-token",
		"--robot",
		"input", "watch",
	})

	done := make(chan error, 1)
	go func() {
		done <- command.Execute()
	}()

	<-eventsWritten
	stdout.WaitForLineCount(t, 3, time.Second)
	cancel()

	if err := <-done; err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	lines := bytes.Split(bytes.TrimSpace(stdout.Bytes()), []byte{'\n'})
	if len(lines) != 4 {
		t.Fatalf("stream line count = %d, want 4; output=%s", len(lines), stdout.String())
	}

	var header map[string]any
	if err := json.Unmarshal(lines[0], &header); err != nil {
		t.Fatalf("header json.Unmarshal() error = %v", err)
	}
	if got := header["stream"]; got != "events" {
		t.Fatalf("header stream = %#v, want %q", got, "events")
	}
	parsedURL, err := url.Parse(server.URL)
	if err != nil {
		t.Fatalf("url.Parse() error = %v", err)
	}
	if got := header["host"]; got != parsedURL.Host {
		t.Fatalf("header host = %#v, want %q", got, parsedURL.Host)
	}

	deviceContext, ok := header["device_context"].(map[string]any)
	if !ok {
		t.Fatalf("header device_context = %#v; want object", header["device_context"])
	}
	if got := deviceContext["firmware_version"]; got != "0.3.1" {
		t.Fatalf("firmware_version = %#v, want %q", got, "0.3.1")
	}
	if got := deviceContext["modio_present"]; got != true {
		t.Fatalf("modio_present = %#v, want true", got)
	}
	if got := deviceContext["modio_sync"]; got != "synchronized" {
		t.Fatalf("modio_sync = %#v, want %q", got, "synchronized")
	}

	var firstEvent map[string]any
	if err := json.Unmarshal(lines[1], &firstEvent); err != nil {
		t.Fatalf("first event json.Unmarshal() error = %v", err)
	}
	if got := firstEvent["event"]; got != "digital_input" {
		t.Fatalf("first event name = %#v, want %q", got, "digital_input")
	}

	var secondEvent map[string]any
	if err := json.Unmarshal(lines[2], &secondEvent); err != nil {
		t.Fatalf("second event json.Unmarshal() error = %v", err)
	}
	if got := secondEvent["event"]; got != "heartbeat" {
		t.Fatalf("second event name = %#v, want %q", got, "heartbeat")
	}

	var endEvent map[string]any
	if err := json.Unmarshal(lines[3], &endEvent); err != nil {
		t.Fatalf("end event json.Unmarshal() error = %v", err)
	}
	if got := endEvent["event"]; got != "stream_end" {
		t.Fatalf("end event name = %#v, want %q", got, "stream_end")
	}
	if got := endEvent["reason"]; got != "client_disconnect" {
		t.Fatalf("end event reason = %#v, want %q", got, "client_disconnect")
	}
}

func TestInputWatchStaysNDJSONWhenRobotJSONIsRequested(t *testing.T) {
	eventsWritten := make(chan struct{})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("X-FW-Version", "0.3.1")
		w.Header().Set("X-ModIO-Present", "true")
		w.Header().Set("X-ModIO-Sync", "synchronized")

		flusher, ok := w.(http.Flusher)
		if !ok {
			t.Error("response writer does not support flushing")
			return
		}

		_, _ = w.Write([]byte("event: digital_input\n"))
		_, _ = w.Write([]byte("data: {\"id\":2,\"state\":true,\"ts_ms\":12345}\n\n"))
		flusher.Flush()
		close(eventsWritten)

		<-r.Context().Done()
	}))
	defer server.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	command := newRootCommand()
	stdout := newObservedBuffer()
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetContext(ctx)
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "stream-token",
		"--robot",
		"--format", "json",
		"input", "watch",
	})

	done := make(chan error, 1)
	go func() {
		done <- command.Execute()
	}()

	<-eventsWritten
	stdout.WaitForLineCount(t, 2, time.Second)
	cancel()

	if err := <-done; err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	lines := bytes.Split(bytes.TrimSpace(stdout.Bytes()), []byte{'\n'})
	if len(lines) != 3 {
		t.Fatalf("stream line count = %d, want 3; output=%s", len(lines), stdout.String())
	}

	var header map[string]any
	if err := json.Unmarshal(lines[0], &header); err != nil {
		t.Fatalf("header json.Unmarshal() error = %v", err)
	}
	if got := header["stream"]; got != "events" {
		t.Fatalf("header stream = %#v, want %q", got, "events")
	}
	if _, exists := header["command"]; exists {
		t.Fatalf("header unexpectedly looks like a robot envelope: %#v", header)
	}
}

func TestInputWatchRejectsHumanMode(t *testing.T) {
	t.Parallel()

	for _, args := range [][]string{
		{
			"--host", "http://127.0.0.1:1",
			"--api-token", "stream-token",
			"input", "watch",
		},
		{
			"--host", "http://127.0.0.1:1",
			"--api-token", "stream-token",
			"--format", "json",
			"input", "watch",
		},
	} {
		args := args
		name := "default"
		if len(args) > 6 {
			name = "json"
		}

		t.Run(name, func(t *testing.T) {
			command := newRootCommand()
			stdout := &bytes.Buffer{}
			command.SetOut(stdout)
			command.SetErr(&bytes.Buffer{})
			command.SetArgs(args)

			err := command.Execute()
			if err == nil {
				t.Fatal("Execute() succeeded; want bad-argument error")
			}
			if got := exitcodes.FromError(err); got != exitcodes.BadArgument {
				t.Fatalf("exit code = %d, want %d", got, exitcodes.BadArgument)
			}
			if err.Error() != "input watch requires --robot because it emits an NDJSON stream" {
				t.Fatalf("error = %q, want input-watch robot guidance", err.Error())
			}
			if stdout.Len() != 0 {
				t.Fatalf("stdout = %q, want empty", stdout.String())
			}
		})
	}
}

func TestInputDigitalOutputsJSONWithSampleAgeField(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/inputs/digital" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/inputs/digital")
		}
		if got := r.Header.Get("Authorization"); got != "Bearer input-token" {
			t.Fatalf("Authorization header = %q, want %q", got, "Bearer input-token")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(
			w,
			`{"sample_ts_ms":12345,"staleness_ms":67,"poll_interval_ms":100,"inputs":[{"id":1,"state":true},{"id":2,"state":false},{"id":3,"state":true},{"id":4,"state":false}]}`,
		)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "input-token",
		"--format", "json",
		"input", "digital",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if got := payload["sample_ts_ms"]; got != float64(12345) {
		t.Fatalf("sample_ts_ms = %#v, want 12345", got)
	}
	if got := payload["sample_age_ms"]; got != float64(67) {
		t.Fatalf("sample_age_ms = %#v, want 67", got)
	}
	if _, exists := payload["staleness_ms"]; exists {
		t.Fatalf("unexpected raw staleness_ms field in CLI payload: %#v", payload)
	}
	inputs, ok := payload["inputs"].([]any)
	if !ok || len(inputs) != 4 {
		t.Fatalf("inputs = %#v, want 4 items", payload["inputs"])
	}
}

func TestInputAnalogSingleOutputsJSON(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/inputs/analog/2" {
			t.Fatalf("request path = %q, want %q", r.URL.Path, "/api/v1/inputs/analog/2")
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(
			w,
			`{"sample_ts_ms":5000,"staleness_ms":25,"poll_interval_ms":250,"input":{"id":2,"value":512}}`,
		)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "input-token",
		"--format", "json",
		"input", "analog", "2",
	})

	if err := command.Execute(); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	var payload analogInputResult
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if payload.Input.ID != 2 || payload.Input.Value != 512 {
		t.Fatalf("input payload = %#v, want id=2 value=512", payload.Input)
	}
	if payload.SampleAgeMS != 25 {
		t.Fatalf("sample_age_ms = %d, want 25", payload.SampleAgeMS)
	}
}

func TestInputDigitalRobotEnvelopeSurfacesSampleUnavailable(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = io.WriteString(w, `{"error":{"code":"MODIO_SAMPLE_UNAVAILABLE","message":"MOD-IO sample is not available yet","status":503}}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "input-token",
		"--robot",
		"--format", "json",
		"input", "digital",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want sample-unavailable error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.HardwareUnavailable {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.HardwareUnavailable)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	errorPayload, ok := payload["error"].(map[string]any)
	if !ok {
		t.Fatalf("error = %#v; want object", payload["error"])
	}
	if got := errorPayload["code"]; got != "MODIO_SAMPLE_UNAVAILABLE" {
		t.Fatalf("error.code = %#v, want %q", got, "MODIO_SAMPLE_UNAVAILABLE")
	}
}

func TestInputAnalogRobotEnvelopeSurfacesInputNotFound(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		_, _ = io.WriteString(w, `{"error":{"code":"INPUT_NOT_FOUND","message":"Input not found","status":404}}`)
	}))
	defer server.Close()

	command := newRootCommand()
	stdout := &bytes.Buffer{}
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "input-token",
		"--robot",
		"--format", "json",
		"input", "analog", "9",
	})

	err := command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want input-not-found error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.BadArgument {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.BadArgument)
	}

	stdout.Reset()
	command = newRootCommand()
	command.SetOut(stdout)
	command.SetErr(&bytes.Buffer{})
	command.SetArgs([]string{
		"--host", server.URL,
		"--api-token", "input-token",
		"--robot",
		"--format", "json",
		"input", "analog", "4",
	})

	err = command.Execute()
	if err == nil {
		t.Fatal("Execute() succeeded; want input-not-found error")
	}
	if got := exitcodes.FromError(err); got != exitcodes.NotFound {
		t.Fatalf("exit code = %d, want %d", got, exitcodes.NotFound)
	}

	var payload map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	errorPayload, ok := payload["error"].(map[string]any)
	if !ok {
		t.Fatalf("error = %#v; want object", payload["error"])
	}
	if got := errorPayload["code"]; got != "INPUT_NOT_FOUND" {
		t.Fatalf("error.code = %#v, want %q", got, "INPUT_NOT_FOUND")
	}
}

type observedBuffer struct {
	mu     sync.Mutex
	buffer bytes.Buffer
	lines  int
	waitCh chan struct{}
}

func newObservedBuffer() *observedBuffer {
	return &observedBuffer{
		waitCh: make(chan struct{}, 16),
	}
}

func (b *observedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()

	written, err := b.buffer.Write(p)
	lineCount := bytes.Count(p[:written], []byte{'\n'})
	b.lines += lineCount
	for i := 0; i < lineCount; i++ {
		select {
		case b.waitCh <- struct{}{}:
		default:
		}
	}
	return written, err
}

func (b *observedBuffer) Bytes() []byte {
	b.mu.Lock()
	defer b.mu.Unlock()

	return append([]byte(nil), b.buffer.Bytes()...)
}

func (b *observedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()

	return b.buffer.String()
}

func (b *observedBuffer) WaitForLineCount(t *testing.T, want int, timeout time.Duration) {
	t.Helper()

	deadline := time.NewTimer(timeout)
	defer deadline.Stop()

	for {
		b.mu.Lock()
		lines := b.lines
		b.mu.Unlock()
		if lines >= want {
			return
		}

		select {
		case <-b.waitCh:
		case <-deadline.C:
			t.Fatalf("timed out waiting for %d stream lines; got %d; output=%s", want, lines, b.String())
		}
	}
}
