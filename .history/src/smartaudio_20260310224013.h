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

// State machine and processing
void smartaudioProcess(void);
void smartaudioRequestGetSettings(void);
void smartaudioRequestSetChannel(uint8_t band, uint8_t channel);
void smartaudioRequestSetPower(uint8_t power);
void smartaudioRequestSetFrequency(uint16_t frequency, bool pitmodeFrequency);

// Last known settings (updated when responses arrive)
typedef struct {
	uint8_t channel;
	uint8_t power;
	uint8_t opMode;
	uint16_t frequency;
	bool valid;
} smartaudio_device_state_t;

extern smartaudio_device_state_t sa_state;

// Queue and retransmit configuration
#define SMARTAUDIO_QUEUE_DEPTH 8
#define SMARTAUDIO_MAX_RETRIES 3

// Enqueue requests (these return true if queued)
bool smartaudioEnqueueGetSettings(void);
bool smartaudioEnqueueSetChannel(uint8_t band, uint8_t channel);
bool smartaudioEnqueueSetPower(uint8_t power);
bool smartaudioEnqueueSetFrequency(uint16_t frequency, bool pitmodeFrequency);

// Half-duplex / inversion support
void smartaudioSetDirPin(int pin); // pass -1 to disable
void smartaudioSetInvertUART(bool invert); // attempt inversion via ESP driver
void smartaudioSetOneWirePin(int pin);
void smartaudioSetPrependZero(bool enable);

// Debug
void smartaudioEnableDebug(bool enable);

