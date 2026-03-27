#pragma once

#include <Arduino.h>

static const uint8_t MAX_VTX_DEVICES = 6;
static const char DEFAULT_VTX_TABLE_PATH[] = "/peak_thor_t35.json";

enum VtxProtocol : uint8_t {
    VTX_PROTOCOL_SMARTAUDIO = 0,
    VTX_PROTOCOL_TRAMP = 1,
};

struct VtxDeviceConfig {
    char name[24];
    uint8_t pwmInputPin;
    uint8_t vtxControlPin;
    uint8_t protocol;
    bool enabled;
    char vtxTablePath[64];
};

struct AppConfig {
    uint8_t wifiChannel;
    bool espNowEnabled;
    uint8_t deviceCount;
    VtxDeviceConfig devices[MAX_VTX_DEVICES];
};

static inline void appConfigSetDefaultDevice(VtxDeviceConfig& device, uint8_t index) {
    snprintf(device.name, sizeof(device.name), "VTX %u", static_cast<unsigned>(index + 1));
    device.pwmInputPin = static_cast<uint8_t>(2 + index);
    device.vtxControlPin = static_cast<uint8_t>(4 + index);
    device.protocol = VTX_PROTOCOL_SMARTAUDIO;
    device.enabled = (index == 0);
    strncpy(device.vtxTablePath, DEFAULT_VTX_TABLE_PATH, sizeof(device.vtxTablePath) - 1);
    device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
}

static inline void appConfigSetDefaults(AppConfig& config) {
    config.wifiChannel = 1;
    config.espNowEnabled = true;
    config.deviceCount = 1;
    for (uint8_t index = 0; index < MAX_VTX_DEVICES; index++) {
        appConfigSetDefaultDevice(config.devices[index], index);
    }
}

static inline uint8_t appConfigClampDeviceCount(uint8_t count) {
    if (count == 0) {
        return 1;
    }
    return count > MAX_VTX_DEVICES ? MAX_VTX_DEVICES : count;
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
