package robot

import (
	"encoding/json"
	"errors"
	"io"
	"time"
)

type StreamWriter struct {
	encoder *json.Encoder
}

type streamHeader struct {
	V             int            `json:"v"`
	Stream        string         `json:"stream"`
	Host          string         `json:"host"`
	StartedAt     string         `json:"started_at"`
	DeviceContext *DeviceContext `json:"device_context,omitempty"`
}

type streamEvent struct {
	Event      string `json:"event"`
	Data       any    `json:"data,omitempty"`
	Reason     string `json:"reason,omitempty"`
	ReceivedAt string `json:"received_at"`
}

func NewStreamWriter(w io.Writer) (*StreamWriter, error) {
	if w == nil {
		return nil, errors.New("stream writer output is required")
	}

	encoder := json.NewEncoder(w)
	encoder.SetEscapeHTML(false)
	return &StreamWriter{encoder: encoder}, nil
}

func (w *StreamWriter) WriteHeader(stream, host string, startedAt time.Time, deviceContext *DeviceContext) error {
	return w.encoder.Encode(streamHeader{
		V:             1,
		Stream:        stream,
		Host:          host,
		StartedAt:     startedAt.UTC().Format(time.RFC3339Nano),
		DeviceContext: deviceContext,
	})
}

func (w *StreamWriter) WriteEvent(event string, data any, receivedAt time.Time) error {
	return w.encoder.Encode(streamEvent{
		Event:      event,
		Data:       data,
		ReceivedAt: receivedAt.UTC().Format(time.RFC3339Nano),
	})
}

func (w *StreamWriter) WriteStreamEnd(reason string, receivedAt time.Time) error {
	return w.encoder.Encode(streamEvent{
		Event:      "stream_end",
		Reason:     reason,
		ReceivedAt: receivedAt.UTC().Format(time.RFC3339Nano),
	})
}
