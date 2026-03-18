package test_e2e

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"testing"
)

const (
	stubAPIToken        = "stub-token"
	stubFirmwareVersion = "0.4.0"
	stubPollIntervalMS  = 100
)

type stubStatusResponse struct {
	UptimeSeconds   int               `json:"uptime_seconds"`
	FirmwareVersion string            `json:"firmware_version"`
	FreeHeapBytes   int               `json:"free_heap_bytes"`
	Network         stubNetworkStatus `json:"network"`
	ModIO           stubModIOStatus   `json:"modio"`
}

type stubNetworkStatus struct {
	Hostname  string `json:"hostname"`
	Connected bool   `json:"connected"`
	IP        string `json:"ip"`
	Netmask   string `json:"netmask"`
	Gateway   string `json:"gateway"`
}

type stubModIOStatus struct {
	Present bool   `json:"present"`
	Sync    string `json:"sync"`
}

type stubRelayView struct {
	Group string  `json:"group"`
	ID    int     `json:"id"`
	State bool    `json:"state"`
	Sync  *string `json:"sync"`
}

type stubRelayListResponse struct {
	ModIOPresent bool            `json:"modio_present"`
	ModIOSync    string          `json:"modio_sync"`
	Relays       []stubRelayView `json:"relays"`
}

type stubRelayArrayResponse struct {
	Relays []stubRelayView `json:"relays"`
}

type stubRelaySingleResponse struct {
	Relay stubRelayView `json:"relay"`
}

type stubInputMetadata struct {
	SampleTSMS     int `json:"sample_ts_ms"`
	StalenessMS    int `json:"staleness_ms"`
	PollIntervalMS int `json:"poll_interval_ms"`
}

type stubDigitalInputValue struct {
	ID    int  `json:"id"`
	State bool `json:"state"`
}

type stubAnalogInputValue struct {
	ID    int `json:"id"`
	Value int `json:"value"`
}

type stubDigitalInputsResponse struct {
	stubInputMetadata
	Inputs []stubDigitalInputValue `json:"inputs"`
}

type stubDigitalInputResponse struct {
	stubInputMetadata
	Input stubDigitalInputValue `json:"input"`
}

type stubAnalogInputsResponse struct {
	stubInputMetadata
	Inputs []stubAnalogInputValue `json:"inputs"`
}

type stubAnalogInputResponse struct {
	stubInputMetadata
	Input stubAnalogInputValue `json:"input"`
}

type stubOTAResponse struct {
	RebootInSeconds int `json:"reboot_in_seconds"`
}

type stubAPIErrorEnvelope struct {
	Error stubAPIError `json:"error"`
}

type stubAPIError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Status  int    `json:"status"`
}

type stubServerState struct {
	modioPresent bool
	onboard      [2]bool
	modio        [4]bool
	digital      [4]bool
	analog       [4]int
}

type stubServerControl struct {
	server *httptest.Server

	mu           sync.Mutex
	token        string
	state        stubServerState
	requestCount int
	relayErrors  map[string]stubAPIError
}

func newStubServer(tb testing.TB) (*httptest.Server, *stubServerControl) {
	tb.Helper()

	control := &stubServerControl{
		token: stubAPIToken,
	}
	control.resetLocked()

	server := httptest.NewServer(http.HandlerFunc(control.serveHTTP))
	control.server = server
	tb.Cleanup(server.Close)

	return server, control
}

func (c *stubServerControl) Reset() {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.resetLocked()
}

func (c *stubServerControl) SetModIOPresent(present bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.state.modioPresent = present
}

func (c *stubServerControl) SetRelayAPIError(target string, status int, code string, message string) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.relayErrors == nil {
		c.relayErrors = make(map[string]stubAPIError)
	}
	c.relayErrors[target] = stubAPIError{
		Code:    code,
		Message: message,
		Status:  status,
	}
}

func (c *stubServerControl) RequestCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()

	return c.requestCount
}

func (c *stubServerControl) ResetRequestCount() {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.requestCount = 0
}

func (c *stubServerControl) HostPort(tb testing.TB) string {
	tb.Helper()

	parsedURL, err := url.Parse(c.server.URL)
	if err != nil {
		tb.Fatalf("url.Parse(%q) error = %v", c.server.URL, err)
	}

	return parsedURL.Host
}

func (c *stubServerControl) resetLocked() {
	c.state = stubServerState{
		modioPresent: true,
		onboard:      [2]bool{false, true},
		modio:        [4]bool{true, false, true, false},
		digital:      [4]bool{true, false, true, false},
		analog:       [4]int{128, 256, 512, 768},
	}
	c.requestCount = 0
	c.relayErrors = make(map[string]stubAPIError)
}

func (c *stubServerControl) serveHTTP(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	c.requestCount++
	c.mu.Unlock()

	switch {
	// Mirrors rest_api_status_handler() in firmware/components/rest_api/rest_api.c.
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/status":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleStatus(w)
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleRelayList(w)
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays/onboard":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleRelayGroupList(w, "onboard")
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/relays/modio":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleRelayGroupList(w, "modio")
	case r.Method == http.MethodPut && r.URL.Path == "/api/v1/relays/modio":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleModIOBatchSet(w, r)
	case r.Method == http.MethodPut && strings.HasPrefix(r.URL.Path, "/api/v1/relays/"):
		if !c.requireAuth(w, r) {
			return
		}
		c.handleRelaySet(w, r)
	case r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/toggle"):
		if !c.requireAuth(w, r) {
			return
		}
		c.handleRelayToggle(w, r)
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/inputs/digital":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleDigitalInputs(w)
	case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/api/v1/inputs/digital/"):
		if !c.requireAuth(w, r) {
			return
		}
		c.handleDigitalInput(w, r)
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/inputs/analog":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleAnalogInputs(w)
	case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/api/v1/inputs/analog/"):
		if !c.requireAuth(w, r) {
			return
		}
		c.handleAnalogInput(w, r)
	// Mirrors rest_api_ota_handler() in firmware/components/rest_api/rest_api.c.
	case r.Method == http.MethodPost && r.URL.Path == "/api/v1/ota":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleOTA(w, r)
	// Mirrors rest_api_events_handler() and the SSE client task in rest_api.c.
	case r.Method == http.MethodGet && r.URL.Path == "/api/v1/events":
		if !c.requireAuth(w, r) {
			return
		}
		c.handleEvents(w)
	default:
		if strings.HasPrefix(r.URL.Path, "/api/v1/") && !c.requireAuth(w, r) {
			return
		}
		c.writeAPIError(w, http.StatusNotFound, "NOT_FOUND", "Endpoint not found", true)
	}
}

func (c *stubServerControl) requireAuth(w http.ResponseWriter, r *http.Request) bool {
	authorization := strings.TrimSpace(r.Header.Get("Authorization"))
	if authorization == "" {
		c.writeAPIError(w, http.StatusUnauthorized, "AUTH_REQUIRED", "Bearer token is required", false)
		return false
	}

	if authorization != "Bearer "+c.token {
		c.writeAPIError(w, http.StatusForbidden, "AUTH_FORBIDDEN", "Bearer token is invalid", false)
		return false
	}

	return true
}

func (c *stubServerControl) handleStatus(w http.ResponseWriter) {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.writeJSONLocked(w, http.StatusOK, stubStatusResponse{
		UptimeSeconds:   42,
		FirmwareVersion: stubFirmwareVersion,
		FreeHeapBytes:   123456,
		Network: stubNetworkStatus{
			Hostname:  "lab-relay",
			Connected: true,
			IP:        "192.168.1.60",
			Netmask:   "255.255.255.0",
			Gateway:   "192.168.1.1",
		},
		ModIO: stubModIOStatus{
			Present: c.state.modioPresent,
			Sync:    c.modioSyncLocked(),
		},
	})
}

func (c *stubServerControl) handleRelayList(w http.ResponseWriter) {
	c.mu.Lock()
	defer c.mu.Unlock()

	relays := c.onboardRelaysLocked()
	if c.state.modioPresent {
		relays = append(relays, c.modioRelaysLocked()...)
	}

	c.writeJSONLocked(w, http.StatusOK, stubRelayListResponse{
		ModIOPresent: c.state.modioPresent,
		ModIOSync:    c.modioSyncLocked(),
		Relays:       relays,
	})
}

func (c *stubServerControl) handleRelayGroupList(w http.ResponseWriter, group string) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if group == "modio" && !c.state.modioPresent {
		c.writeAPIErrorLocked(w,
			http.StatusServiceUnavailable,
			"MODIO_NOT_PRESENT",
			"MOD-IO is not present",
			true,
		)
		return
	}

	var relays []stubRelayView
	switch group {
	case "onboard":
		relays = c.onboardRelaysLocked()
	case "modio":
		relays = c.modioRelaysLocked()
	default:
		c.writeAPIErrorLocked(w, http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found", true)
		return
	}

	c.writeJSONLocked(w, http.StatusOK, stubRelayArrayResponse{Relays: relays})
}

func (c *stubServerControl) handleRelaySet(w http.ResponseWriter, r *http.Request) {
	group, relayID, isToggle, ok := parseRelayPath(r.URL.Path)
	if !ok || isToggle {
		c.writeAPIError(w, http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found", true)
		return
	}

	var requestBody struct {
		State *bool `json:"state"`
	}
	if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil || requestBody.State == nil {
		c.writeAPIError(w, http.StatusBadRequest, "INVALID_RELAY_STATE", "Relay state must be a boolean", true)
		return
	}

	c.handleRelayMutation(w, group, relayID, func(current bool) bool {
		return *requestBody.State
	})
}

func (c *stubServerControl) handleRelayToggle(w http.ResponseWriter, r *http.Request) {
	group, relayID, isToggle, ok := parseRelayPath(r.URL.Path)
	if !ok || !isToggle {
		c.writeAPIError(w, http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found", true)
		return
	}

	c.handleRelayMutation(w, group, relayID, func(current bool) bool {
		return !current
	})
}

func (c *stubServerControl) handleRelayMutation(
	w http.ResponseWriter,
	group string,
	relayID int,
	nextState func(current bool) bool,
) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if apiErr, ok := c.relayErrors[relayTargetKey(group, relayID)]; ok {
		c.writeAPIErrorLocked(w, apiErr.Status, apiErr.Code, apiErr.Message, true)
		return
	}

	switch group {
	case "onboard":
		if relayID < 1 || relayID > len(c.state.onboard) {
			c.writeAPIErrorLocked(w, http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found", true)
			return
		}
		c.state.onboard[relayID-1] = nextState(c.state.onboard[relayID-1])
		c.writeJSONLocked(w, http.StatusOK, stubRelaySingleResponse{
			Relay: stubRelayView{
				Group: "onboard",
				ID:    relayID,
				State: c.state.onboard[relayID-1],
				Sync:  nil,
			},
		})
	case "modio":
		if relayID < 1 || relayID > len(c.state.modio) {
			c.writeAPIErrorLocked(w, http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found", true)
			return
		}
		if !c.state.modioPresent {
			c.writeAPIErrorLocked(w,
				http.StatusServiceUnavailable,
				"MODIO_NOT_PRESENT",
				"MOD-IO is not present",
				true,
			)
			return
		}
		c.state.modio[relayID-1] = nextState(c.state.modio[relayID-1])
		sync := c.modioSyncLocked()
		c.writeJSONLocked(w, http.StatusOK, stubRelaySingleResponse{
			Relay: stubRelayView{
				Group: "modio",
				ID:    relayID,
				State: c.state.modio[relayID-1],
				Sync:  stringPtr(sync),
			},
		})
	default:
		c.writeAPIErrorLocked(w, http.StatusNotFound, "RELAY_NOT_FOUND", "Relay not found", true)
	}
}

func (c *stubServerControl) handleModIOBatchSet(w http.ResponseWriter, r *http.Request) {
	var requestBody struct {
		States []bool `json:"states"`
	}
	if err := json.NewDecoder(r.Body).Decode(&requestBody); err != nil || len(requestBody.States) != len(c.state.modio) {
		c.writeAPIError(w, http.StatusBadRequest, "INVALID_RELAY_STATE", "Request body must contain exactly 4 relay states", true)
		return
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if !c.state.modioPresent {
		c.writeAPIErrorLocked(w,
			http.StatusServiceUnavailable,
			"MODIO_NOT_PRESENT",
			"MOD-IO is not present",
			true,
		)
		return
	}

	copy(c.state.modio[:], requestBody.States)
	c.writeJSONLocked(w, http.StatusOK, stubRelayArrayResponse{Relays: c.modioRelaysLocked()})
}

func (c *stubServerControl) handleDigitalInputs(w http.ResponseWriter) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if !c.state.modioPresent {
		c.writeAPIErrorLocked(w, http.StatusServiceUnavailable, "MODIO_NOT_PRESENT", "MOD-IO is not present", true)
		return
	}

	inputs := make([]stubDigitalInputValue, 0, len(c.state.digital))
	for index, state := range c.state.digital {
		inputs = append(inputs, stubDigitalInputValue{ID: index + 1, State: state})
	}

	c.writeJSONLocked(w, http.StatusOK, stubDigitalInputsResponse{
		stubInputMetadata: c.inputMetadata(),
		Inputs:            inputs,
	})
}

func (c *stubServerControl) handleDigitalInput(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if !c.state.modioPresent {
		c.writeAPIErrorLocked(w, http.StatusServiceUnavailable, "MODIO_NOT_PRESENT", "MOD-IO is not present", true)
		return
	}

	inputID, ok := parseTrailingID(r.URL.Path)
	if !ok || inputID < 1 || inputID > len(c.state.digital) {
		c.writeAPIErrorLocked(w, http.StatusNotFound, "INPUT_NOT_FOUND", "Input not found", true)
		return
	}

	c.writeJSONLocked(w, http.StatusOK, stubDigitalInputResponse{
		stubInputMetadata: c.inputMetadata(),
		Input:             stubDigitalInputValue{ID: inputID, State: c.state.digital[inputID-1]},
	})
}

func (c *stubServerControl) handleAnalogInputs(w http.ResponseWriter) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if !c.state.modioPresent {
		c.writeAPIErrorLocked(w, http.StatusServiceUnavailable, "MODIO_NOT_PRESENT", "MOD-IO is not present", true)
		return
	}

	inputs := make([]stubAnalogInputValue, 0, len(c.state.analog))
	for index, value := range c.state.analog {
		inputs = append(inputs, stubAnalogInputValue{ID: index + 1, Value: value})
	}

	c.writeJSONLocked(w, http.StatusOK, stubAnalogInputsResponse{
		stubInputMetadata: c.inputMetadata(),
		Inputs:            inputs,
	})
}

func (c *stubServerControl) handleAnalogInput(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if !c.state.modioPresent {
		c.writeAPIErrorLocked(w, http.StatusServiceUnavailable, "MODIO_NOT_PRESENT", "MOD-IO is not present", true)
		return
	}

	inputID, ok := parseTrailingID(r.URL.Path)
	if !ok || inputID < 1 || inputID > len(c.state.analog) {
		c.writeAPIErrorLocked(w, http.StatusNotFound, "INPUT_NOT_FOUND", "Input not found", true)
		return
	}

	c.writeJSONLocked(w, http.StatusOK, stubAnalogInputResponse{
		stubInputMetadata: c.inputMetadata(),
		Input:             stubAnalogInputValue{ID: inputID, Value: c.state.analog[inputID-1]},
	})
}

func (c *stubServerControl) handleOTA(w http.ResponseWriter, r *http.Request) {
	if _, err := io.ReadAll(r.Body); err != nil {
		c.writeAPIError(w, http.StatusBadRequest, "INVALID_BODY", "Failed to read request body", true)
		return
	}

	c.writeJSON(w, http.StatusOK, stubOTAResponse{RebootInSeconds: 2})
}

func (c *stubServerControl) handleEvents(w http.ResponseWriter) {
	c.applyDeviceContextHeaders(w)
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Content-Type", "text/event-stream")
	w.WriteHeader(http.StatusOK)

	_, _ = io.WriteString(w, ":heartbeat\n\n")
	_, _ = io.WriteString(w, "event: relay_changed\n")
	_, _ = io.WriteString(w, "data: {\"group\":\"onboard\",\"id\":1,\"state\":true}\n\n")

	if flusher, ok := w.(http.Flusher); ok {
		flusher.Flush()
	}
}

func (c *stubServerControl) writeJSON(w http.ResponseWriter, statusCode int, payload any) {
	c.applyDeviceContextHeaders(w)
	c.writeJSONLocked(w, statusCode, payload)
}

func (c *stubServerControl) writeJSONLocked(w http.ResponseWriter, statusCode int, payload any) {
	c.applyDeviceContextHeadersLocked(w)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	_ = json.NewEncoder(w).Encode(payload)
}

func (c *stubServerControl) writeAPIError(
	w http.ResponseWriter,
	statusCode int,
	code string,
	message string,
	includeDeviceContext bool,
) {
	if includeDeviceContext {
		c.applyDeviceContextHeaders(w)
	}
	c.writeAPIErrorLocked(w, statusCode, code, message, false)
}

func (c *stubServerControl) writeAPIErrorLocked(
	w http.ResponseWriter,
	statusCode int,
	code string,
	message string,
	includeDeviceContext bool,
) {
	if includeDeviceContext {
		c.applyDeviceContextHeadersLocked(w)
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	_ = json.NewEncoder(w).Encode(stubAPIErrorEnvelope{
		Error: stubAPIError{
			Code:    code,
			Message: message,
			Status:  statusCode,
		},
	})
}

func (c *stubServerControl) applyDeviceContextHeaders(w http.ResponseWriter) {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.applyDeviceContextHeadersLocked(w)
}

func (c *stubServerControl) applyDeviceContextHeadersLocked(w http.ResponseWriter) {
	w.Header().Set("X-FW-Version", stubFirmwareVersion)
	w.Header().Set("X-ModIO-Present", strconv.FormatBool(c.state.modioPresent))
	w.Header().Set("X-ModIO-Sync", c.modioSyncLocked())
}

func (c *stubServerControl) onboardRelaysLocked() []stubRelayView {
	relays := make([]stubRelayView, 0, len(c.state.onboard))
	for index, state := range c.state.onboard {
		relays = append(relays, stubRelayView{
			Group: "onboard",
			ID:    index + 1,
			State: state,
			Sync:  nil,
		})
	}

	return relays
}

func (c *stubServerControl) modioRelaysLocked() []stubRelayView {
	relays := make([]stubRelayView, 0, len(c.state.modio))
	sync := c.modioSyncLocked()
	for index, state := range c.state.modio {
		relays = append(relays, stubRelayView{
			Group: "modio",
			ID:    index + 1,
			State: state,
			Sync:  stringPtr(sync),
		})
	}

	return relays
}

func (c *stubServerControl) modioSyncLocked() string {
	if !c.state.modioPresent {
		return "absent"
	}

	return "synchronized"
}

func (c *stubServerControl) inputMetadata() stubInputMetadata {
	return stubInputMetadata{
		SampleTSMS:     12345,
		StalenessMS:    67,
		PollIntervalMS: stubPollIntervalMS,
	}
}

func parseRelayPath(path string) (group string, relayID int, isToggle bool, ok bool) {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	if len(parts) < 5 || parts[0] != "api" || parts[1] != "v1" || parts[2] != "relays" {
		return "", 0, false, false
	}

	group = parts[3]
	relayID, err := strconv.Atoi(parts[4])
	if err != nil {
		return "", 0, false, false
	}

	if len(parts) == 6 && parts[5] == "toggle" {
		return group, relayID, true, true
	}

	return group, relayID, false, len(parts) == 5
}

func parseTrailingID(path string) (int, bool) {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	if len(parts) == 0 {
		return 0, false
	}

	value, err := strconv.Atoi(parts[len(parts)-1])
	if err != nil {
		return 0, false
	}

	return value, true
}

func stringPtr(value string) *string {
	return &value
}

func relayTargetKey(group string, relayID int) string {
	return group + ":" + strconv.Itoa(relayID)
}

func TestStubServerAuthFailureOmitsDeviceContextHeaders(t *testing.T) {
	server, _ := newStubServer(t)

	request, err := http.NewRequest(http.MethodGet, server.URL+"/api/v1/status", nil)
	if err != nil {
		t.Fatalf("http.NewRequest() error = %v", err)
	}

	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatalf("Do() error = %v", err)
	}
	defer func() {
		_ = response.Body.Close()
	}()

	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", response.StatusCode, http.StatusUnauthorized)
	}
	if got := response.Header.Get("X-FW-Version"); got != "" {
		t.Fatalf("X-FW-Version = %q, want empty", got)
	}
	if got := response.Header.Get("X-ModIO-Present"); got != "" {
		t.Fatalf("X-ModIO-Present = %q, want empty", got)
	}
	if got := response.Header.Get("X-ModIO-Sync"); got != "" {
		t.Fatalf("X-ModIO-Sync = %q, want empty", got)
	}
}

func TestStubServerRelayListDropsModIOWhenAbsent(t *testing.T) {
	server, control := newStubServer(t)
	control.SetModIOPresent(false)

	request, err := http.NewRequest(http.MethodGet, server.URL+"/api/v1/relays", nil)
	if err != nil {
		t.Fatalf("http.NewRequest() error = %v", err)
	}
	request.Header.Set("Authorization", "Bearer "+stubAPIToken)

	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatalf("Do() error = %v", err)
	}
	defer func() {
		_ = response.Body.Close()
	}()

	if response.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want %d", response.StatusCode, http.StatusOK)
	}

	var payload stubRelayListResponse
	if err := json.NewDecoder(response.Body).Decode(&payload); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if payload.ModIOPresent {
		t.Fatal("modio_present = true, want false")
	}
	if payload.ModIOSync != "absent" {
		t.Fatalf("modio_sync = %q, want %q", payload.ModIOSync, "absent")
	}
	if len(payload.Relays) != 2 {
		t.Fatalf("relay count = %d, want 2", len(payload.Relays))
	}
	for _, relay := range payload.Relays {
		if relay.Group != "onboard" {
			t.Fatalf("relay = %#v, want only onboard relays", relay)
		}
	}
}

func TestStubServerStatusResponseIsContractAccurate(t *testing.T) {
	server, _ := newStubServer(t)

	request, err := http.NewRequest(http.MethodGet, server.URL+"/api/v1/status", nil)
	if err != nil {
		t.Fatalf("http.NewRequest() error = %v", err)
	}
	request.Header.Set("Authorization", "Bearer "+stubAPIToken)

	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatalf("Do() error = %v", err)
	}
	defer func() {
		_ = response.Body.Close()
	}()

	if response.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want %d", response.StatusCode, http.StatusOK)
	}
	if got := response.Header.Get("X-FW-Version"); got != stubFirmwareVersion {
		t.Fatalf("X-FW-Version = %q, want %q", got, stubFirmwareVersion)
	}

	var payload stubStatusResponse
	if err := json.NewDecoder(response.Body).Decode(&payload); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if payload.Network.Hostname != "lab-relay" {
		t.Fatalf("network.hostname = %q, want %q", payload.Network.Hostname, "lab-relay")
	}
	if !payload.ModIO.Present {
		t.Fatal("modio.present = false, want true")
	}
	if payload.ModIO.Sync != "synchronized" {
		t.Fatalf("modio.sync = %q, want %q", payload.ModIO.Sync, "synchronized")
	}
	if payload.FirmwareVersion != stubFirmwareVersion {
		t.Fatalf("firmware_version = %q, want %q", payload.FirmwareVersion, stubFirmwareVersion)
	}
}
