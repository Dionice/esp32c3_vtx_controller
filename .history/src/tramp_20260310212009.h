#pragma once

#include <stdint.h>

// TRAMP helpers mirroring Betaflight framing
void trampSendFrequency(uint16_t freq);
void trampSendPower(uint16_t power);
void trampSendActiveState(bool active);

