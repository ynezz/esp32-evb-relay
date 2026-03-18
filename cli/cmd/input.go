package cmd

import (
	"context"
	"errors"
	"io"
	"time"

	"github.com/spf13/cobra"

	"example.com/esp32-evb-relay/cli/client"
	"example.com/esp32-evb-relay/cli/internal/exitcodes"
	"example.com/esp32-evb-relay/cli/internal/robot"
)

const streamReconnectDelay = 250 * time.Millisecond

func newInputCommand() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "input",
		Short: "Read device inputs",
	}

	cmd.AddCommand(newInputWatchCommand())
	return cmd
}

func newInputWatchCommand() *cobra.Command {
	return &cobra.Command{
		Use:   "watch",
		Short: "Watch the device event stream",
		RunE:  runInputWatch,
	}
}

func runInputWatch(cmd *cobra.Command, _ []string) error {
	runtime, ok := ConfigFromContext(cmd)
	if !ok {
		return exitcodes.Wrap(exitcodes.GeneralError, errors.New("runtime config is unavailable"))
	}

	streamClient, err := client.New(client.Config{
		Host:     runtime.Host,
		APIToken: runtime.APIToken,
		Timeout:  runtime.Timeout,
	})
	if err != nil {
		return err
	}

	streamWriter, err := robot.NewStreamWriter(cmd.OutOrStdout())
	if err != nil {
		return exitcodes.Wrap(exitcodes.GeneralError, err)
	}

	startedAt := time.Now().UTC()
	headerWritten := false

	for {
		err = streamClient.WatchEventsOnce(
			cmd.Context(),
			func(result client.Result) error {
				if headerWritten {
					return nil
				}
				headerWritten = true
				return streamWriter.WriteHeader(
					"events",
					streamClient.Host(),
					startedAt,
					robot.FromClientDeviceContext(result.DeviceContext),
				)
			},
			func(event client.StreamEvent) error {
				return streamWriter.WriteEvent(event.Event, event.Data, time.Now().UTC())
			},
		)

		if cmd.Context().Err() != nil {
			if headerWritten {
				return streamWriter.WriteStreamEnd("client_disconnect", time.Now().UTC())
			}
			return nil
		}

		if errors.Is(err, io.EOF) || exitcodes.FromError(err) == exitcodes.NetworkError {
			if !sleepContext(cmd.Context(), streamReconnectDelay) {
				if headerWritten {
					return streamWriter.WriteStreamEnd("client_disconnect", time.Now().UTC())
				}
				return nil
			}
			continue
		}

		return err
	}
}

func sleepContext(ctx context.Context, delay time.Duration) bool {
	timer := time.NewTimer(delay)
	defer timer.Stop()

	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
