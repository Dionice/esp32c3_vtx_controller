#pragma once

#include <stdint.h>

// SmartAudio helpers mirroring Betaflight framing
void smartaudioSendBandChannel(uint8_t band, uint8_t channel);
void smartaudioSendPower(uint8_t power);
void smartaudioSendFrequency(uint16_t frequency, bool pitmodeFrequency);

