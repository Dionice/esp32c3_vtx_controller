#pragma once

#include <stdint.h>

// TRAMP helpers mirroring Betaflight framing
void trampSendFrequency(uint16_t freq);
void trampSendPower(uint16_t power);
void trampSendActiveState(bool active);
bool trampInit(void);
void trampResetState(void);

// TRAMP processing and parser
void trampProcess(void);

typedef struct {
	uint16_t frequency;
	uint16_t power;
	bool valid;
} tramp_device_state_t;

extern tramp_device_state_t tramp_state;

// TRAMP queue + retries
#define TRAMP_QUEUE_DEPTH 6
#define TRAMP_MAX_RETRIES 3

bool trampEnqueueGetSettings(void);
bool trampEnqueueInit(void);
bool trampEnqueueSetFrequency(uint16_t freq);
bool trampEnqueueSetPower(uint16_t power);
bool trampEnqueueSetActiveState(bool active);
bool trampIsBusy(void);

// half-duplex control for tramp can reuse same dir pin
void trampEnableDebug(bool enable);

