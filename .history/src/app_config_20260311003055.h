#pragma once

#include <Arduino.h>

enum VtxProtocol : uint8_t {
    VTX_PROTOCOL_SMARTAUDIO = 0,
    VTX_PROTOCOL_TRAMP = 1,
};

struct AppConfig {
    uint8_t pwmInputPin;
    uint8_t vtxUartPin;
    uint8_t protocol;
    uint8_t wifiChannel;
    bool espNowEnabled;
    char selectedVtxPath[64];
};

static inline void appConfigSetDefaults(AppConfig& config) {
    config.pwmInputPin = 2;
    config.vtxUartPin = 4;
    config.protocol = VTX_PROTOCOL_SMARTAUDIO;
    config.wifiChannel = 1;
    config.espNowEnabled = true;
    strncpy(config.selectedVtxPath, "/peak_thor_t35.json", sizeof(config.selectedVtxPath) - 1);
    config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
}

static inline const char* appConfigProtocolToString(uint8_t protocol) {
    switch (protocol) {
        case VTX_PROTOCOL_TRAMP:
            return "tramp";
        case VTX_PROTOCOL_SMARTAUDIO:
        default:
            return "smartaudio";
    }
}

static inline bool appConfigProtocolFromString(const String& input, uint8_t& protocol) {
    String normalized = input;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "smartaudio") {
        protocol = VTX_PROTOCOL_SMARTAUDIO;
        return true;
    }
    if (normalized == "tramp") {
        protocol = VTX_PROTOCOL_TRAMP;
        return true;
    }
    return false;
}
