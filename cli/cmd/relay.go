package cmd

import (
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/cobra"

	"github.com/ynezz/esp32-evb-relay/cli/client"
	appconfig "github.com/ynezz/esp32-evb-relay/cli/internal/config"
	"github.com/ynezz/esp32-evb-relay/cli/internal/exitcodes"
	outputformat "github.com/ynezz/esp32-evb-relay/cli/internal/format"
	"github.com/ynezz/esp32-evb-relay/cli/internal/robot"
)

const (
	relayListCommandName   = "relay list"
	relayOnCommandName     = "relay on"
	relayOffCommandName    = "relay off"
	relayToggleCommandName = "relay toggle"
	relaySetCommandName    = "relay set"

	onboardRelayCount = 2
	modioRelayCount   = 4
)

var relayCommandFlags = []string{"--host", "--api-token", "--format", "--timeout", "--robot"}

type relayGroup string

const (
	relayGroupOnboard relayGroup = "onboard"
	relayGroupModIO   relayGroup = "modio"
)

type relayTarget struct {
	Group relayGroup
	ID    int
}

type relayAssignment struct {
	Target relayTarget
	State  bool
}

type relayView struct {
	Group string  `json:"group"`
	ID    int     `json:"id"`
	State bool    `json:"state"`
	Sync  *string `json:"sync"`
}

type relayListResult struct {
	ModIOPresent bool        `json:"modio_present"`
	ModIOSync    string      `json:"modio_sync"`
	Relays       []relayView `json:"relays"`
}

type relaySingleResult struct {
	Relay relayView `json:"relay"`
}

type relayBatchError struct {
	Code       string `json:"code"`
	Message    string `json:"message"`
	HTTPStatus int    `json:"http_status,omitempty"`
}

type relaySetTargetResult struct {
	Target    string           `json:"target"`
	Group     string           `json:"group"`
	ID        int              `json:"id"`
	Requested bool             `json:"requested"`
	State     *bool            `json:"state"`
	Sync      *string          `json:"sync"`
	OK        bool             `json:"ok"`
	Error     *relayBatchError `json:"error,omitempty"`
}

type relaySetResult struct {
	Results []relaySetTargetResult `json:"results"`
	AllOK   bool                   `json:"all_ok"`
}

type relayArrayResponse struct {
	Relays []relayView `json:"relays"`
}

func newRelayCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "relay",
		Short: "Read and control relay state",
	}

	command.AddCommand(
		newRelayListCommand(),
		newRelayOnCommand(),
		newRelayOffCommand(),
		newRelayToggleCommand(),
		newRelaySetCommand(),
	)

	return command
}

func newRelayListCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "list",
		Short: "List onboard and MOD-IO relay state",
		Args:  cobra.NoArgs,
		RunE:  runRelayList,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Flags: relayCommandFlags,
		OutputFields: []string{
			"modio_present",
			"modio_sync",
			"relays[].group",
			"relays[].id",
			"relays[].state",
			"relays[].sync",
		},
		Errors: []string{
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
		},
		Example: "evb-relay relay list",
	})

	return command
}

func newRelayOnCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "on <target>",
		Short: "Turn a relay on",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			return runRelaySetState(cmd, args[0], true)
		},
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Args:  []string{"target"},
		Flags: relayCommandFlags,
		OutputFields: []string{
			"relay.group",
			"relay.id",
			"relay.state",
			"relay.sync",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"RELAY_NOT_FOUND",
			"MODIO_NOT_PRESENT",
			"MODIO_STATE_UNKNOWN",
		},
		Example: "evb-relay relay on onboard:1",
	})

	return command
}

func newRelayOffCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "off <target>",
		Short: "Turn a relay off",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			return runRelaySetState(cmd, args[0], false)
		},
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Args:  []string{"target"},
		Flags: relayCommandFlags,
		OutputFields: []string{
			"relay.group",
			"relay.id",
			"relay.state",
			"relay.sync",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"RELAY_NOT_FOUND",
			"MODIO_NOT_PRESENT",
			"MODIO_STATE_UNKNOWN",
		},
		Example: "evb-relay relay off modio:3",
	})

	return command
}

func newRelayToggleCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "toggle <target>",
		Short: "Toggle a relay",
		Args:  cobra.ExactArgs(1),
		RunE:  runRelayToggle,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Args:  []string{"target"},
		Flags: relayCommandFlags,
		OutputFields: []string{
			"relay.group",
			"relay.id",
			"relay.state",
			"relay.sync",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"RELAY_NOT_FOUND",
			"MODIO_NOT_PRESENT",
			"MODIO_STATE_UNKNOWN",
		},
		Example: "evb-relay relay toggle onboard:2",
	})

	return command
}

func newRelaySetCommand() *cobra.Command {
	command := &cobra.Command{
		Use:   "set <target=state> [target=state...]",
		Short: "Apply one or more relay state changes",
		Args:  cobra.MinimumNArgs(1),
		RunE:  runRelaySet,
	}

	robot.AnnotateCommand(command, robot.CommandCapability{
		Args:  []string{"target=state"},
		Flags: relayCommandFlags,
		OutputFields: []string{
			"results[].target",
			"results[].group",
			"results[].id",
			"results[].requested",
			"results[].state",
			"results[].sync",
			"results[].ok",
			"results[].error.code",
			"results[].error.message",
			"all_ok",
		},
		Errors: []string{
			"BAD_ARGUMENT",
			"NETWORK_ERROR",
			"AUTH_REQUIRED",
			"AUTH_FORBIDDEN",
			"RELAY_NOT_FOUND",
			"MODIO_NOT_PRESENT",
			"MODIO_STATE_UNKNOWN",
			"PARTIAL_FAILURE",
		},
		Example: "evb-relay relay set onboard:1=on modio:3=off",
	})

	return command
}

func runRelayList(cmd *cobra.Command, _ []string) error {
	runtime, relayClient, host, startedAt, err := newRelayRuntime(cmd)
	if err != nil {
		return wrapRelayResult(cmd, runtime, relayListCommandName, host, nil, nil, startedAt, err, false)
	}

	var payload relayListResult
	result, err := relayClient.DoJSON(cmd.Context(), http.MethodGet, "/relays", nil, &payload)

	return wrapRelayResult(
		cmd,
		runtime,
		relayListCommandName,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
		false,
	)
}

func runRelaySetState(cmd *cobra.Command, rawTarget string, state bool) error {
	runtime, relayClient, host, startedAt, err := newRelayRuntime(cmd)
	if err != nil {
		return wrapRelayResult(cmd, runtime, commandNameForRelayState(state), host, nil, nil, startedAt, err, false)
	}

	target, err := parseRelayTarget(rawTarget)
	if err != nil {
		return wrapRelayResult(cmd, runtime, commandNameForRelayState(state), host, nil, nil, startedAt, err, false)
	}

	payload, result, err := setRelayState(cmd, relayClient, target, state)
	return wrapRelayResult(
		cmd,
		runtime,
		commandNameForRelayState(state),
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
		false,
	)
}

func runRelayToggle(cmd *cobra.Command, args []string) error {
	runtime, relayClient, host, startedAt, err := newRelayRuntime(cmd)
	if err != nil {
		return wrapRelayResult(cmd, runtime, relayToggleCommandName, host, nil, nil, startedAt, err, false)
	}

	target, err := parseRelayTarget(args[0])
	if err != nil {
		return wrapRelayResult(cmd, runtime, relayToggleCommandName, host, nil, nil, startedAt, err, false)
	}

	payload, result, err := toggleRelay(cmd, relayClient, target)

	return wrapRelayResult(
		cmd,
		runtime,
		relayToggleCommandName,
		host,
		&payload,
		robot.FromClientDeviceContext(result.DeviceContext),
		startedAt,
		err,
		false,
	)
}

func runRelaySet(cmd *cobra.Command, args []string) error {
	runtime, relayClient, host, startedAt, err := newRelayRuntime(cmd)
	if err != nil {
		return wrapRelayResult(cmd, runtime, relaySetCommandName, host, nil, nil, startedAt, err, true)
	}

	assignments, err := parseRelayAssignments(args)
	if err != nil {
		return wrapRelayResult(cmd, runtime, relaySetCommandName, host, nil, nil, startedAt, err, true)
	}

	payload, deviceContext, err := runRelayBatchSet(cmd, relayClient, assignments)
	return wrapRelayResult(
		cmd,
		runtime,
		relaySetCommandName,
		host,
		&payload,
		deviceContext,
		startedAt,
		err,
		true,
	)
}

func newRelayRuntime(cmd *cobra.Command) (appconfig.Runtime, *client.Client, string, time.Time, error) {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return appconfig.Runtime{}, nil, "", time.Time{}, exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	startedAt := time.Now()
	relayClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return runtime, nil, runtime.Host, startedAt, err
	}

	return runtime, relayClient, relayClient.Host(), startedAt, nil
}

func wrapRelayResult(
	cmd *cobra.Command,
	runtime appconfig.Runtime,
	commandName string,
	host string,
	payload any,
	deviceContext *robot.DeviceContext,
	startedAt time.Time,
	err error,
	preserveDataOnError bool,
) error {
	elapsed := time.Since(startedAt)
	data := payload
	if err != nil && !preserveDataOnError {
		data = nil
	}

	if runtime.Robot {
		return robot.Wrap(cmd, cmd.OutOrStdout(), robot.WrapOpts{
			Command:       commandName,
			Data:          data,
			DeviceContext: deviceContext,
			Err:           err,
			Format:        runtime.Format,
			Host:          host,
			Elapsed:       elapsed,
		})
	}

	if err == nil {
		return outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format)
	}

	if preserveDataOnError && payload != nil {
		if outputErr := outputformat.Output(cmd.OutOrStdout(), payload, runtime.Format); outputErr != nil {
			return outputErr
		}
	}

	return err
}

func commandNameForRelayState(state bool) string {
	if state {
		return relayOnCommandName
	}

	return relayOffCommandName
}

func parseRelayTarget(raw string) (relayTarget, error) {
	group, idText, maxID, err := parseRelayTargetParts(raw)
	if err != nil {
		return relayTarget{}, err
	}
	if strings.EqualFold(idText, "all") {
		return relayTarget{}, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("relay target %q uses :all shorthand, which is not supported by this command yet", raw))
	}

	id, err := parseRelayID(raw, group, idText, maxID)
	if err != nil {
		return relayTarget{}, err
	}
	return relayTarget{Group: group, ID: id}, nil
}

func parseRelayAssignments(values []string) ([]relayAssignment, error) {
	assignments := make([]relayAssignment, 0, len(values))
	seen := make(map[string]struct{}, len(values))

	for _, value := range values {
		targetRaw, stateRaw, found := strings.Cut(value, "=")
		if !found {
			return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("invalid relay assignment %q: expected <target>=<state>", value))
		}

		targets, err := expandRelayTargets(targetRaw)
		if err != nil {
			return nil, err
		}

		state, err := parseRelayState(stateRaw)
		if err != nil {
			return nil, err
		}

		for _, target := range targets {
			key := target.String()
			if _, exists := seen[key]; exists {
				return nil, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("duplicate relay target %q", key))
			}
			seen[key] = struct{}{}

			assignments = append(assignments, relayAssignment{
				Target: target,
				State:  state,
			})
		}
	}

	return assignments, nil
}

func parseRelayState(raw string) (bool, error) {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case "on", "true":
		return true, nil
	case "off", "false":
		return false, nil
	default:
		return false, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("unsupported relay state %q: expected on/off", strings.TrimSpace(raw)))
	}
}

func expandRelayTargets(raw string) ([]relayTarget, error) {
	group, idText, maxID, err := parseRelayTargetParts(raw)
	if err != nil {
		return nil, err
	}

	if strings.EqualFold(idText, "all") {
		targets := make([]relayTarget, 0, maxID)
		for id := 1; id <= maxID; id++ {
			targets = append(targets, relayTarget{Group: group, ID: id})
		}
		return targets, nil
	}

	id, err := parseRelayID(raw, group, idText, maxID)
	if err != nil {
		return nil, err
	}

	return []relayTarget{{Group: group, ID: id}}, nil
}

func parseRelayTargetParts(raw string) (relayGroup, string, int, error) {
	trimmed := strings.TrimSpace(raw)
	groupRaw, idRaw, found := strings.Cut(trimmed, ":")
	if !found {
		return "", "", 0, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("invalid relay target %q: expected <group>:<id>", raw))
	}

	group := relayGroup(strings.ToLower(strings.TrimSpace(groupRaw)))
	switch group {
	case relayGroupOnboard:
		return group, strings.TrimSpace(idRaw), onboardRelayCount, nil
	case relayGroupModIO:
		return group, strings.TrimSpace(idRaw), modioRelayCount, nil
	default:
		return "", "", 0, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("unsupported relay group %q", strings.TrimSpace(groupRaw)))
	}
}

func parseRelayID(raw string, group relayGroup, idText string, maxID int) (int, error) {
	id, err := strconv.Atoi(idText)
	if err != nil || id < 1 || id > maxID {
		return 0, exitcodes.Wrap(exitcodes.BadArgument, fmt.Errorf("invalid relay target %q: %s ids must be in range 1-%d", raw, group, maxID))
	}

	return id, nil
}

func (t relayTarget) String() string {
	return fmt.Sprintf("%s:%d", t.Group, t.ID)
}

func setRelayState(cmd *cobra.Command, relayClient *client.Client, target relayTarget, state bool) (relaySingleResult, client.Result, error) {
	var payload relaySingleResult
	result, err := relayClient.DoJSON(
		cmd.Context(),
		http.MethodPut,
		fmt.Sprintf("/relays/%s/%d", target.Group, target.ID),
		map[string]bool{"state": state},
		&payload,
	)
	return payload, result, err
}

func toggleRelay(cmd *cobra.Command, relayClient *client.Client, target relayTarget) (relaySingleResult, client.Result, error) {
	var payload relaySingleResult
	result, err := relayClient.DoJSON(
		cmd.Context(),
		http.MethodPost,
		fmt.Sprintf("/relays/%s/%d/toggle", target.Group, target.ID),
		nil,
		&payload,
	)
	return payload, result, err
}

func fetchModIORelayArray(cmd *cobra.Command, relayClient *client.Client) (relayArrayResponse, client.Result, error) {
	var payload relayArrayResponse
	result, err := relayClient.DoJSON(cmd.Context(), http.MethodGet, "/relays/modio", nil, &payload)
	return payload, result, err
}

func relayByID(relays []relayView, id int) (relayView, bool) {
	for _, relay := range relays {
		if relay.ID == id {
			return relay, true
		}
	}

	return relayView{}, false
}

func modioAssignmentsCoverAllRelays(assignments []relayAssignment) bool {
	if len(assignments) < modioRelayCount {
		return false
	}

	seen := [modioRelayCount]bool{}
	for _, assignment := range assignments {
		if assignment.Target.Group != relayGroupModIO {
			continue
		}
		if assignment.Target.ID < 1 || assignment.Target.ID > modioRelayCount {
			return false
		}
		seen[assignment.Target.ID-1] = true
	}

	for _, present := range seen {
		if !present {
			return false
		}
	}

	return true
}

func modioRelayStatesFromAssignments(assignments []relayAssignment) []bool {
	states := make([]bool, modioRelayCount)

	for _, assignment := range assignments {
		if assignment.Target.Group != relayGroupModIO {
			continue
		}
		states[assignment.Target.ID-1] = assignment.State
	}

	return states
}

func modioRelaySync(relays []relayView) string {
	for _, relay := range relays {
		if relay.Group != string(relayGroupModIO) {
			continue
		}
		if relay.Sync != nil {
			return *relay.Sync
		}
	}

	return ""
}

func modioStateUnknownError() error {
	return exitcodes.Wrap(
		exitcodes.StateError,
		&client.APIError{
			Code:    "MODIO_STATE_UNKNOWN",
			Message: "MOD-IO relay state is unknown; use a full modio relay set first",
			Status:  http.StatusConflict,
		},
	)
}

func runRelayBatchSet(
	cmd *cobra.Command,
	relayClient *client.Client,
	assignments []relayAssignment,
) (relaySetResult, *robot.DeviceContext, error) {
	results := make([]relaySetTargetResult, len(assignments))
	causes := make([]error, len(assignments))
	indexByTarget := make(map[string]int, len(assignments))
	modioAssignments := make([]relayAssignment, 0)
	deviceContext := (*robot.DeviceContext)(nil)

	for index, assignment := range assignments {
		targetKey := assignment.Target.String()
		indexByTarget[targetKey] = index
		results[index] = relaySetTargetResult{
			Target:    targetKey,
			Group:     string(assignment.Target.Group),
			ID:        assignment.Target.ID,
			Requested: assignment.State,
		}

		if assignment.Target.Group == relayGroupModIO {
			modioAssignments = append(modioAssignments, assignment)
		}
	}

	recordError := func(target relayTarget, err error) {
		index := indexByTarget[target.String()]
		item := &results[index]
		item.OK = false
		item.Error = relayBatchErrorFrom(err)
		causes[index] = err
	}

	recordSuccess := func(target relayTarget, relay relayView) {
		item := &results[indexByTarget[target.String()]]
		item.OK = true
		item.State = boolPtr(relay.State)
		item.Sync = relay.Sync
		item.Error = nil
	}

	for _, assignment := range assignments {
		if assignment.Target.Group != relayGroupOnboard {
			continue
		}

		payload, result, err := setRelayState(cmd, relayClient, assignment.Target, assignment.State)
		if deviceContext == nil {
			deviceContext = robot.FromClientDeviceContext(result.DeviceContext)
		}
		if err != nil {
			recordError(assignment.Target, err)
			continue
		}

		recordSuccess(assignment.Target, payload.Relay)
	}

	if len(modioAssignments) > 0 {
		states := make([]bool, modioRelayCount)
		haveFullState := false

		if modioAssignmentsCoverAllRelays(modioAssignments) {
			states = modioRelayStatesFromAssignments(modioAssignments)
			haveFullState = true
		} else {
			current, result, err := fetchModIORelayArray(cmd, relayClient)
			if deviceContext == nil {
				deviceContext = robot.FromClientDeviceContext(result.DeviceContext)
			}

			if err != nil {
				for _, assignment := range modioAssignments {
					recordError(assignment.Target, err)
				}
			} else if modioRelaySync(current.Relays) != "synchronized" {
				stateErr := modioStateUnknownError()
				for _, assignment := range modioAssignments {
					recordError(assignment.Target, stateErr)
				}
			} else {
				for _, relay := range current.Relays {
					if relay.ID >= 1 && relay.ID <= modioRelayCount {
						states[relay.ID-1] = relay.State
					}
				}
				for _, assignment := range modioAssignments {
					states[assignment.Target.ID-1] = assignment.State
				}
				haveFullState = true
			}
		}

		if haveFullState {
			var payload relayArrayResponse
			result, err := relayClient.DoJSON(
				cmd.Context(),
				http.MethodPut,
				"/relays/modio",
				map[string][]bool{"states": states},
				&payload,
			)
			if deviceContext == nil {
				deviceContext = robot.FromClientDeviceContext(result.DeviceContext)
			}
			if err != nil {
				for _, assignment := range modioAssignments {
					recordError(assignment.Target, err)
				}
			} else {
				for _, assignment := range modioAssignments {
					relay, ok := relayByID(payload.Relays, assignment.Target.ID)
					if !ok {
						recordError(
							assignment.Target,
							exitcodes.Wrap(
								exitcodes.GeneralError,
								&client.APIError{
									Code:    "GENERAL_ERROR",
									Message: fmt.Sprintf("device response omitted relay %s after MOD-IO batch update", assignment.Target),
								},
							),
						)
						continue
					}

					recordSuccess(assignment.Target, relay)
				}
			}
		}
	}

	failures := 0
	for _, item := range results {
		if !item.OK {
			failures++
		}
	}

	payload := relaySetResult{
		Results: results,
		AllOK:   failures == 0,
	}
	if failures == 0 {
		return payload, deviceContext, nil
	}

	if failures == len(results) {
		for _, cause := range causes {
			if cause != nil {
				return payload, deviceContext, cause
			}
		}
	}

	return payload, deviceContext, partialFailureError(failures, len(results))
}

func partialFailureError(failures int, total int) error {
	return exitcodes.Wrap(
		exitcodes.GeneralError,
		&client.APIError{
			Code:    "PARTIAL_FAILURE",
			Message: fmt.Sprintf("%d of %d relay targets failed", failures, total),
		},
	)
}

func relayBatchErrorFrom(err error) *relayBatchError {
	if err == nil {
		return nil
	}

	info := &relayBatchError{
		Code:    relayErrorCode(err),
		Message: strings.TrimSpace(err.Error()),
	}

	var apiErr *client.APIError
	if errors.As(err, &apiErr) {
		if message := strings.TrimSpace(apiErr.Message); message != "" {
			info.Message = message
		}
		if apiErr.Status != 0 {
			info.HTTPStatus = apiErr.Status
		}
	}

	return info
}

func relayErrorCode(err error) string {
	var apiErr *client.APIError
	if errors.As(err, &apiErr) {
		if code := canonicalRelayErrorCode(strings.ToUpper(strings.TrimSpace(apiErr.Code))); code != "" {
			return code
		}
		switch apiErr.Status {
		case http.StatusUnauthorized:
			return "AUTH_REQUIRED"
		case http.StatusForbidden:
			return "AUTH_FORBIDDEN"
		}
	}

	switch exitcodes.FromError(err) {
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

func canonicalRelayErrorCode(code string) string {
	switch code {
	case "AUTH_INVALID":
		return "AUTH_FORBIDDEN"
	default:
		return code
	}
}

func boolPtr(value bool) *bool {
	return &value
}

func relayStateLabel(value bool) string {
	if value {
		return "on"
	}

	return "off"
}

func relayStateValue(value *bool) string {
	if value == nil {
		return ""
	}

	return relayStateLabel(*value)
}

func relaySyncValue(value *string) string {
	if value == nil {
		return ""
	}

	return *value
}

func relayBatchErrorValue(err *relayBatchError) string {
	if err == nil {
		return ""
	}
	if err.Code != "" && err.Message != "" {
		return err.Code + ": " + err.Message
	}
	if err.Message != "" {
		return err.Message
	}
	return err.Code
}

func (r relayListResult) TableOutput() (outputformat.TableData, error) {
	rows := make([][]string, 0, len(r.Relays))
	for _, relay := range r.Relays {
		rows = append(rows, []string{
			fmt.Sprintf("%s:%d", relay.Group, relay.ID),
			relayStateLabel(relay.State),
			relaySyncValue(relay.Sync),
			strconv.FormatBool(r.ModIOPresent),
			r.ModIOSync,
		})
	}

	return outputformat.TableData{
		Headers: []string{"TARGET", "STATE", "SYNC", "MODIO PRESENT", "MODIO SYNC"},
		Rows:    rows,
	}, nil
}

func (r relayListResult) PlainOutput() ([]string, error) {
	lines := []string{
		fmt.Sprintf("modio.present=%t", r.ModIOPresent),
		fmt.Sprintf("modio.sync=%s", r.ModIOSync),
	}
	for _, relay := range r.Relays {
		target := fmt.Sprintf("%s:%d", relay.Group, relay.ID)
		lines = append(lines, fmt.Sprintf("relay.%s.state=%t", target, relay.State))
		lines = append(lines, fmt.Sprintf("relay.%s.sync=%s", target, relaySyncValue(relay.Sync)))
	}
	return lines, nil
}

func (r relaySingleResult) TableOutput() (outputformat.TableData, error) {
	return outputformat.TableData{
		Headers: []string{"TARGET", "STATE", "SYNC"},
		Rows: [][]string{{
			fmt.Sprintf("%s:%d", r.Relay.Group, r.Relay.ID),
			relayStateLabel(r.Relay.State),
			relaySyncValue(r.Relay.Sync),
		}},
	}, nil
}

func (r relaySingleResult) PlainOutput() ([]string, error) {
	return []string{
		fmt.Sprintf("relay.group=%s", r.Relay.Group),
		fmt.Sprintf("relay.id=%d", r.Relay.ID),
		fmt.Sprintf("relay.state=%t", r.Relay.State),
		fmt.Sprintf("relay.sync=%s", relaySyncValue(r.Relay.Sync)),
	}, nil
}

func (r relaySetResult) TableOutput() (outputformat.TableData, error) {
	rows := make([][]string, 0, len(r.Results))
	for _, result := range r.Results {
		rows = append(rows, []string{
			result.Target,
			relayStateLabel(result.Requested),
			relayStateValue(result.State),
			relaySyncValue(result.Sync),
			strconv.FormatBool(result.OK),
			relayBatchErrorValue(result.Error),
		})
	}

	return outputformat.TableData{
		Headers: []string{"TARGET", "REQUESTED", "STATE", "SYNC", "OK", "ERROR"},
		Rows:    rows,
	}, nil
}

func (r relaySetResult) PlainOutput() ([]string, error) {
	lines := []string{fmt.Sprintf("all_ok=%t", r.AllOK)}
	for _, result := range r.Results {
		prefix := "result." + result.Target
		lines = append(lines, fmt.Sprintf("%s.requested=%s", prefix, relayStateLabel(result.Requested)))
		lines = append(lines, fmt.Sprintf("%s.state=%s", prefix, relayStateValue(result.State)))
		lines = append(lines, fmt.Sprintf("%s.sync=%s", prefix, relaySyncValue(result.Sync)))
		lines = append(lines, fmt.Sprintf("%s.ok=%t", prefix, result.OK))
		if result.Error != nil {
			lines = append(lines, fmt.Sprintf("%s.error.code=%s", prefix, result.Error.Code))
			lines = append(lines, fmt.Sprintf("%s.error.message=%s", prefix, result.Error.Message))
		}
	}
	return lines, nil
}
