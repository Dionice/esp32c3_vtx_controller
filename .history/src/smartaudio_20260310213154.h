#pragma once

#include <stdint.h>

// SmartAudio helpers mirroring Betaflight framing
void smartaudioSendBandChannel(uint8_t band, uint8_t channel);
void smartaudioSendPower(uint8_t power);
void smartaudioSendFrequency(uint16_t frequency, bool pitmodeFrequency);

// Simple probe result
typedef struct {
	uint8_t channel;
	uint8_t power;
	uint8_t operationMode;
	uint16_t frequency;
} smartaudio_probe_result_t;

// Send Get Settings and attempt to read a response within timeout_ms.
// Returns true if a valid settings response was parsed and fills `result`.
bool smartaudioProbe(smartaudio_probe_result_t *result, uint32_t timeout_ms);

