package robot

import "example.com/esp32-evb-relay/cli/client"

type DeviceContext struct {
	ModIOPresent    *bool  `json:"modio_present,omitempty"`
	ModIOSync       string `json:"modio_sync,omitempty"`
	FirmwareVersion string `json:"firmware_version,omitempty"`
}

func FromClientDeviceContext(source *client.DeviceContext) *DeviceContext {
	if source == nil {
		return nil
	}

	deviceContext := &DeviceContext{
		ModIOSync:       source.ModIOSync,
		FirmwareVersion: source.FirmwareVersion,
	}
	if source.ModIOPresent != nil {
		present := *source.ModIOPresent
		deviceContext.ModIOPresent = &present
	}

	return deviceContext
}
