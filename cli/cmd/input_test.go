package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync"
	"testing"
	"time"
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
