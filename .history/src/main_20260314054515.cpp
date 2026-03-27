#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <driver/gpio.h>

extern "C" {
#include <common/mavlink.h>
}

#include "app_config.h"
#include "embedded_vtxtable.h"
#include "mavlink_bridge.h"
#include "smartaudio.h"
#include "storage.h"
#include "tramp.h"

static const uint8_t kDefaultBands[] = {1, 2, 3, 4, 5};
static const uint8_t kDefaultChannels[] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint16_t kDefaultFrequencies[5][8] = {
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
    {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
    {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},
};
static const uint8_t kDefaultPowerValues[] = {0, 1, 2, 3};
static const char* kEmbeddedDefaultTable = DEFAULT_VTX_TABLE_PATH;
static HardwareSerial fcMavSerial(0);
static const uint8_t kMavlinkComponentId = MAV_COMP_ID_TELEMETRY_RADIO;
static const uint8_t kMavResultAccepted = 0;
static const uint8_t kMavResultDenied = 2;
static const uint8_t kMavResultUnsupported = 3;
static const uint8_t kMavResultFailed = 4;
static const uint32_t kMavlinkHeartbeatIntervalMs = 2000;
static const uint32_t kMavlinkStatusTextIntervalMs = 5000;
static const uint32_t kMavlinkFirmwareVersion = 0x020200FFUL;
static const uint32_t kMavlinkMiddlewareVersion = 18;
static const uint16_t kMavlinkVendorId = 0x1205;
static const uint16_t kMavlinkProductId = 0x0001;

struct DeviceRuntimeState {
    volatile uint32_t riseMicros;
    volatile uint16_t capturedPulse;
    volatile bool pulseAvailable;
    uint16_t filteredPulse;
    bool filterInitialized;
    int32_t lastMappedIndex;
    int32_t candidateMappedIndex;
    uint8_t candidateCount;
    bool hasSelection;
    uint8_t currentBandIndex;
    uint8_t currentChannelIndex;
    uint8_t currentPowerIndex;
    bool hasSentSelection;
    uint8_t lastSentProtocol;
    uint8_t lastSentBand;
    uint8_t lastSentChannel;
    uint16_t lastSentPowerValue;
    uint16_t lastSentFrequency;
    DynamicJsonDocument* vtxDoc;
    String vtxJson;
};

struct MavlinkDebugState {
    uint32_t rxByteCount;
    uint32_t seenFrameCount;
    uint32_t seenCommandLongCount;
    uint32_t parsedCommandCount;
    uint32_t localApplyCount;
    uint32_t heartbeatTxCount;
    uint32_t ackTxCount;
    unsigned long lastByteAtMs;
    unsigned long lastCommandAtMs;
    unsigned long lastTxAtMs;
    MavlinkFrameInfo lastFrame;
    bool hasLastFrame;
    MavlinkVtxCommand lastCommand;
    bool hasLastCommand;
};

struct PendingMavlinkSelection {
    bool pending;
    uint8_t bandIndex;
    uint8_t channelIndex;
    uint8_t powerIndex;
};

static AppConfig config;
static DeviceRuntimeState deviceRuntime[MAX_VTX_DEVICES];
static PendingMavlinkSelection pendingMavlinkSelections[MAX_VTX_DEVICES];
static MavlinkDebugState mavlinkDebug = {};
static bool transportReady = false;
static bool restartScheduled = false;
static int8_t activeQueuedMavlinkDevice = -1;
static int8_t activeTransportPin = -1;
static uint8_t activeTransportProtocol = 0xFF;
static unsigned long restartAtMs = 0;
static unsigned long lastMavlinkHeartbeatMs = 0;
static unsigned long lastMavlinkStatusTextMs = 0;
static mavlink_message_t mavlinkRxMessage = {};
static mavlink_status_t mavlinkRxStatus = {};

static bool loadDeviceVtxTable(uint8_t deviceIndex, String* message = nullptr);
static size_t getBandCount(uint8_t deviceIndex);
static size_t getChannelCount();
static size_t getPowerCount(uint8_t deviceIndex);
static uint16_t getFrequencyForSelection(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex);
static uint8_t getPowerValueForIndex(uint8_t deviceIndex, size_t powerIndex);
static String getBandLabelForIndex(uint8_t deviceIndex, size_t bandIndex);
static String getPowerLabelForIndex(uint8_t deviceIndex, size_t powerIndex);
static uint16_t getTransmitPowerValueForDevice(uint8_t deviceIndex, uint8_t protocol, size_t powerIndex);
static uint8_t resolveMavlinkPowerIndex(uint8_t deviceIndex, const VtxDeviceConfig& device, const MavlinkVtxCommand& command);
static bool isValidGpio(uint8_t pin);
static bool queueMavlinkSelectionForDevice(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex, size_t powerIndex);
static void processQueuedMavlinkSelections(void);

static bool mavlinkCommandTargetsBridge(const MavlinkVtxCommand& command) {
    const bool systemMatches = command.targetSystem == 0 || command.targetSystem == config.localNodeId;
    const bool componentMatches = command.targetComponent == 0 || command.targetComponent == kMavlinkComponentId;
    return systemMatches && componentMatches;
}

static bool mavlinkTargetMatchesLocal(uint8_t targetSystem, uint8_t targetComponent) {
    const bool systemMatches = targetSystem == 0 || targetSystem == config.localNodeId;
    const bool componentMatches = targetComponent == 0 || targetComponent == kMavlinkComponentId;
    return systemMatches && componentMatches;
}

static void writeMavlinkMessage(const mavlink_message_t& message) {
    if (!fcMavSerial) {
        return;
    }

    uint8_t frame[MAVLINK_MAX_PACKET_LEN] = {};
    const uint16_t frameLength = mavlink_msg_to_send_buffer(frame, &message);
    if (frameLength == 0) {
        return;
    }

    fcMavSerial.write(frame, frameLength);
    mavlinkDebug.lastTxAtMs = millis();
}

static void populateVtxCommand(const mavlink_message_t& message,
                               const mavlink_command_long_t& payload,
                               MavlinkVtxCommand* command) {
    if (!command) {
        return;
    }

    memset(command, 0, sizeof(*command));
    command->commandId = payload.command;
    command->sourceSystem = message.sysid;
    command->sourceComponent = message.compid;
    command->targetSystem = payload.target_system;
    command->targetComponent = payload.target_component;
    const uint8_t requestedNodeId = static_cast<uint8_t>(constrain(static_cast<int>(lroundf(payload.param1)), 0, 255));
    command->nodeId = requestedNodeId == 0 ? config.localNodeId : requestedNodeId;
    command->deviceId = static_cast<uint8_t>(constrain(static_cast<int>(lroundf(payload.param2)), 0, 255));
    command->band = static_cast<uint8_t>(constrain(static_cast<int>(lroundf(payload.param3)), 0, 255));
    command->channel = static_cast<uint8_t>(constrain(static_cast<int>(lroundf(payload.param4)), 0, 255));
    command->powerValue = static_cast<uint16_t>(constrain(static_cast<int>(lroundf(payload.param5)), 0, 65535));
    command->flags = static_cast<uint8_t>(constrain(static_cast<int>(lroundf(payload.param6)), 0, 255));
}

static void sendMavlinkCommandAck(uint16_t commandId,
                                  uint8_t requesterSystem,
                                  uint8_t requesterComponent,
                                  uint8_t result) {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    if (!fcMavSerial) {
        return;
    }

    mavlink_message_t message = {};
    mavlink_msg_command_ack_pack_chan(config.localNodeId,
                                      kMavlinkComponentId,
                                      MAVLINK_COMM_0,
                                      &message,
                                      commandId,
                                      result,
                                      0,
                                      0,
                                      requesterSystem,
                                      requesterComponent);
    writeMavlinkMessage(message);
    mavlinkDebug.ackTxCount++;
}

static void sendMavlinkCommandAck(const MavlinkVtxCommand& command, uint8_t result) {
    sendMavlinkCommandAck(command.commandId, command.sourceSystem, command.sourceComponent, result);
}

static void sendMavlinkHeartbeat() {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    if (!fcMavSerial) {
        return;
    }

    mavlink_message_t message = {};
    mavlink_msg_heartbeat_pack_chan(config.localNodeId,
                                    kMavlinkComponentId,
                                    MAVLINK_COMM_0,
                                    &message,
                                    MAV_TYPE_ONBOARD_CONTROLLER,
                                    MAV_AUTOPILOT_INVALID,
                                    MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
                                    0,
                                    MAV_STATE_ACTIVE);
    writeMavlinkMessage(message);
    mavlinkDebug.heartbeatTxCount++;
}

static void sendMavlinkStatusText() {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE || !fcMavSerial) {
        return;
    }

    char text[50] = {};
    snprintf(text,
             sizeof(text),
             "ESP32 VTX bridge alive %lus",
             static_cast<unsigned long>(millis() / 1000UL));

    mavlink_message_t message = {};
    mavlink_msg_statustext_pack_chan(config.localNodeId,
                                     kMavlinkComponentId,
                                     MAVLINK_COMM_0,
                                     &message,
                                     MAV_SEVERITY_INFO,
                                     text,
                                     0,
                                     0);
    writeMavlinkMessage(message);
}

static void sendMavlinkAutopilotVersion(const mavlink_message_t& sourceMessage) {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE || !fcMavSerial) {
        return;
    }

    mavlink_autopilot_version_t version = {};
    version.capabilities = MAV_PROTOCOL_CAPABILITY_MAVLINK2 | MAV_PROTOCOL_CAPABILITY_PARAM_ENCODE_BYTEWISE;
    version.flight_sw_version = kMavlinkFirmwareVersion;
    version.middleware_sw_version = kMavlinkMiddlewareVersion;
    version.vendor_id = kMavlinkVendorId;
    version.product_id = kMavlinkProductId;

    mavlink_message_t message = {};
    mavlink_msg_autopilot_version_encode_chan(config.localNodeId,
                                              kMavlinkComponentId,
                                              MAVLINK_COMM_0,
                                              &message,
                                              &version);
    writeMavlinkMessage(message);
    Serial.printf("TX AUTOPILOT_VERSION to=%u/%u caps=0x%08lX\n",
                  static_cast<unsigned>(sourceMessage.sysid),
                  static_cast<unsigned>(sourceMessage.compid),
                  static_cast<unsigned long>(version.capabilities));
}

static void sendMavlinkPingResponse(const mavlink_message_t& sourceMessage, const mavlink_ping_t& ping) {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE || !fcMavSerial) {
        return;
    }

    mavlink_message_t message = {};
    mavlink_msg_ping_pack_chan(config.localNodeId,
                               kMavlinkComponentId,
                               MAVLINK_COMM_0,
                               &message,
                               ping.time_usec,
                               ping.seq,
                               sourceMessage.sysid,
                               sourceMessage.compid);
    writeMavlinkMessage(message);
    Serial.printf("TX PING seq=%lu to=%u/%u\n",
                  static_cast<unsigned long>(ping.seq),
                  static_cast<unsigned>(sourceMessage.sysid),
                  static_cast<unsigned>(sourceMessage.compid));
}

static void logMavlinkCommand(const char* source, const MavlinkVtxCommand& command, bool localApplied, bool forwarded) {
    Serial.printf(
        "MAVLink %s src=%u/%u tgt=%u/%u cmd=%u node=%u device=%u band=%u channel=%u power=%u applied=%u forwarded=%u\n",
        source,
        static_cast<unsigned>(command.sourceSystem),
        static_cast<unsigned>(command.sourceComponent),
        static_cast<unsigned>(command.targetSystem),
        static_cast<unsigned>(command.targetComponent),
        static_cast<unsigned>(command.commandId),
        static_cast<unsigned>(command.nodeId),
        static_cast<unsigned>(command.deviceId),
        static_cast<unsigned>(command.band),
        static_cast<unsigned>(command.channel),
        static_cast<unsigned>(command.powerValue),
        localApplied ? 1 : 0,
        forwarded ? 1 : 0);
}

static bool isValidOptionalGpio(int pin) {
    return pin < 0 || isValidGpio(static_cast<uint8_t>(pin));
}

static bool isValidPwmInputPin(int pin) {
    return pin == -1 || isValidGpio(static_cast<uint8_t>(pin));
}

static uint8_t sanitizeMavlinkNodeId(uint8_t value, uint8_t fallback) {
    if (value == 0 || value >= MAVLINK_VTX_BROADCAST_NODE_ID) {
        return fallback;
    }
    return value;
}

static uint8_t sanitizeMavlinkDeviceId(uint8_t value, uint8_t fallback) {
    if (value == 0 || value == MAVLINK_VTX_ALL_DEVICES_ID) {
        return fallback;
    }
    return value;
}

static void freeDeviceTable(uint8_t index) {
    if (index >= MAX_VTX_DEVICES) {
        return;
    }
    if (deviceRuntime[index].vtxDoc) {
        delete deviceRuntime[index].vtxDoc;
        deviceRuntime[index].vtxDoc = nullptr;
    }
    deviceRuntime[index].vtxJson = "";
}

static void freeAllDeviceTables() {
    for (uint8_t index = 0; index < MAX_VTX_DEVICES; index++) {
        freeDeviceTable(index);
    }
}

static String normalizeTablePath(const String& input) {
    String path = input;
    path.trim();
    if (path.length() == 0) {
        path = "custom_vtx.json";
    }
    path.replace("\\", "/");
    const int slash = path.lastIndexOf('/');
    if (slash >= 0) {
        path = path.substring(slash + 1);
    }
    String clean;
    for (size_t i = 0; i < path.length(); i++) {
        const char ch = path[i];
        if (isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-') {
            clean += ch;
        }
    }
    if (!clean.endsWith(".json")) {
        clean += ".json";
    }
    if (clean.length() == 0) {
        clean = "custom_vtx.json";
    }
    return "/" + clean;
}

static String baseNameFromPath(const char* path) {
    if (!path) {
        return String();
    }
    String fullPath(path);
    const int index = fullPath.lastIndexOf('/');
    return index >= 0 ? fullPath.substring(index + 1) : fullPath;
}

static bool jsonArrayContainsString(JsonArray array, const char* value) {
    if (!value || value[0] == '\0') {
        return false;
    }
    for (JsonVariant item : array) {
        const char* current = item.as<const char*>();
        if (current && strcmp(current, value) == 0) {
            return true;
        }
    }
    return false;
}

static bool isValidGpio(uint8_t pin) {
    return pin <= 21 && pin != 2;
}

static uint8_t clampManualBandValue(uint8_t deviceIndex, uint8_t value) {
    const size_t bandCount = getBandCount(deviceIndex);
    if (bandCount == 0) {
        return 1;
    }
    return static_cast<uint8_t>(constrain(static_cast<int>(value), 1, static_cast<int>(bandCount)));
}

static uint8_t clampManualChannelValue(uint8_t value) {
    const size_t channelCount = getChannelCount();
    if (channelCount == 0) {
        return 1;
    }
    return static_cast<uint8_t>(constrain(static_cast<int>(value), 1, static_cast<int>(channelCount)));
}

static uint8_t clampManualPowerIndexValue(uint8_t deviceIndex, uint8_t value) {
    const size_t powerCount = getPowerCount(deviceIndex);
    if (powerCount == 0) {
        return 0;
    }
    return static_cast<uint8_t>(constrain(static_cast<int>(value), 0, static_cast<int>(powerCount - 1)));
}

static void sanitizeDeviceSelections() {
    config.localNodeId = sanitizeMavlinkNodeId(config.localNodeId, 1);
    if (config.mavlinkBaud < 1200) {
        config.mavlinkBaud = 57600;
    }
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        VtxDeviceConfig& device = config.devices[index];
        device.manualBand = clampManualBandValue(index, device.manualBand);
        device.manualChannel = clampManualChannelValue(device.manualChannel);
        device.manualPowerIndex = clampManualPowerIndexValue(index, device.manualPowerIndex);
        device.mavlinkNodeId = sanitizeMavlinkNodeId(device.mavlinkNodeId, config.localNodeId);
        device.mavlinkDeviceId = sanitizeMavlinkDeviceId(device.mavlinkDeviceId, static_cast<uint8_t>(index + 1));
    }
}

static void rememberSelection(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex, size_t powerIndex) {
    if (deviceIndex >= MAX_VTX_DEVICES) {
        return;
    }
    DeviceRuntimeState& state = deviceRuntime[deviceIndex];
    state.hasSelection = true;
    state.currentBandIndex = static_cast<uint8_t>(bandIndex);
    state.currentChannelIndex = static_cast<uint8_t>(channelIndex);
    state.currentPowerIndex = static_cast<uint8_t>(powerIndex);
}

static void appendControlOptionsJson(uint8_t deviceIndex, JsonObject deviceObject) {
    const VtxDeviceConfig& device = config.devices[deviceIndex];
    deviceObject["controlMode"] = appConfigControlModeToString(device.controlMode);
    deviceObject["manualBand"] = device.manualBand;
    deviceObject["manualChannel"] = device.manualChannel;
    deviceObject["manualPowerIndex"] = device.manualPowerIndex;
    deviceObject["mavlinkNodeId"] = device.mavlinkNodeId;
    deviceObject["mavlinkDeviceId"] = device.mavlinkDeviceId;
    deviceObject["channelCount"] = getChannelCount();

    JsonArray bandOptions = deviceObject.createNestedArray("bandOptions");
    const size_t bandCount = getBandCount(deviceIndex);
    for (size_t bandIndex = 0; bandIndex < bandCount; bandIndex++) {
        JsonObject option = bandOptions.createNestedObject();
        option["value"] = bandIndex + 1;
        option["label"] = getBandLabelForIndex(deviceIndex, bandIndex);
        option["frequency"] = getFrequencyForSelection(deviceIndex, bandIndex, 0);
        JsonArray frequencies = option.createNestedArray("frequencies");
        const size_t channelCount = getChannelCount();
        for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
            frequencies.add(getFrequencyForSelection(deviceIndex, bandIndex, channelIndex));
        }
    }

    JsonArray powerOptions = deviceObject.createNestedArray("powerOptions");
    const size_t powerCount = getPowerCount(deviceIndex);
    for (size_t powerIndex = 0; powerIndex < powerCount; powerIndex++) {
        JsonObject option = powerOptions.createNestedObject();
        option["value"] = powerIndex;
        option["label"] = getPowerLabelForIndex(deviceIndex, powerIndex);
        option["powerValue"] = getPowerValueForIndex(deviceIndex, powerIndex);
    }
}

static void resetRuntimeState() {
    for (uint8_t index = 0; index < MAX_VTX_DEVICES; index++) {
        deviceRuntime[index].riseMicros = 0;
        deviceRuntime[index].capturedPulse = 0;
        deviceRuntime[index].pulseAvailable = false;
        deviceRuntime[index].filteredPulse = 1500;
        deviceRuntime[index].filterInitialized = false;
        deviceRuntime[index].lastMappedIndex = -1;
        deviceRuntime[index].candidateMappedIndex = -1;
        deviceRuntime[index].candidateCount = 0;
        deviceRuntime[index].hasSelection = false;
        deviceRuntime[index].currentBandIndex = 0;
        deviceRuntime[index].currentChannelIndex = 0;
        deviceRuntime[index].currentPowerIndex = 0;
        deviceRuntime[index].hasSentSelection = false;
        deviceRuntime[index].lastSentProtocol = 0;
        deviceRuntime[index].lastSentBand = 0;
        deviceRuntime[index].lastSentChannel = 0;
        deviceRuntime[index].lastSentPowerValue = 0;
        deviceRuntime[index].lastSentFrequency = 0;
        pendingMavlinkSelections[index].pending = false;
        pendingMavlinkSelections[index].bandIndex = 0;
        pendingMavlinkSelections[index].channelIndex = 0;
        pendingMavlinkSelections[index].powerIndex = 0;
    }
    activeQueuedMavlinkDevice = -1;
}

static bool ensureDeviceVtxTableLoaded(uint8_t deviceIndex) {
    if (deviceIndex >= config.deviceCount) {
        return false;
    }
    if (deviceRuntime[deviceIndex].vtxDoc) {
        return true;
    }
    return loadDeviceVtxTable(deviceIndex, nullptr);
}

static DynamicJsonDocument* getDeviceVtxDoc(uint8_t deviceIndex) {
    if (deviceIndex >= MAX_VTX_DEVICES) {
        return nullptr;
    }
    if (!ensureDeviceVtxTableLoaded(deviceIndex)) {
        return nullptr;
    }
    return deviceRuntime[deviceIndex].vtxDoc;
}

static size_t getBandCount(uint8_t deviceIndex) {
    DynamicJsonDocument* doc = getDeviceVtxDoc(deviceIndex);
    if (doc) {
        JsonArray bands = (*doc)["vtx_table"]["bands_list"].as<JsonArray>();
        if (!bands.isNull() && bands.size() > 0) {
            return bands.size();
        }
    }
    return sizeof(kDefaultBands) / sizeof(kDefaultBands[0]);
}

static size_t getChannelCount() {
    return sizeof(kDefaultChannels) / sizeof(kDefaultChannels[0]);
}

static size_t getPowerCount(uint8_t deviceIndex) {
    DynamicJsonDocument* doc = getDeviceVtxDoc(deviceIndex);
    if (doc) {
        JsonArray levels = (*doc)["vtx_table"]["powerlevels_list"].as<JsonArray>();
        if (!levels.isNull() && levels.size() > 0) {
            return levels.size();
        }
    }
    return sizeof(kDefaultPowerValues) / sizeof(kDefaultPowerValues[0]);
}

static uint16_t getFrequencyForSelection(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex) {
    DynamicJsonDocument* doc = getDeviceVtxDoc(deviceIndex);
    if (doc) {
        JsonArray bands = (*doc)["vtx_table"]["bands_list"].as<JsonArray>();
        if (!bands.isNull() && bandIndex < bands.size()) {
            JsonArray freqs = bands[bandIndex]["frequencies"].as<JsonArray>();
            if (!freqs.isNull() && channelIndex < freqs.size()) {
                return freqs[channelIndex] | 0;
            }
        }
    }
    if (bandIndex < (sizeof(kDefaultFrequencies) / sizeof(kDefaultFrequencies[0])) && channelIndex < 8) {
        return kDefaultFrequencies[bandIndex][channelIndex];
    }
    return 0;
}

static uint8_t getPowerValueForIndex(uint8_t deviceIndex, size_t powerIndex) {
    DynamicJsonDocument* doc = getDeviceVtxDoc(deviceIndex);
    if (doc) {
        JsonArray levels = (*doc)["vtx_table"]["powerlevels_list"].as<JsonArray>();
        if (!levels.isNull() && powerIndex < levels.size()) {
            return levels[powerIndex]["value"] | 0;
        }
    }
    if (powerIndex < (sizeof(kDefaultPowerValues) / sizeof(kDefaultPowerValues[0]))) {
        return kDefaultPowerValues[powerIndex];
    }
    return 0;
}

static uint16_t parsePowerLabelMilliwatts(const String& label) {
    String normalized = label;
    normalized.trim();
    normalized.toLowerCase();
    normalized.replace(" ", "");
    if (normalized.length() == 0) {
        return 0;
    }

    bool hasMilliwattsSuffix = normalized.endsWith("mw");
    bool hasWattsSuffix = !hasMilliwattsSuffix && normalized.endsWith("w");
    if (hasMilliwattsSuffix) {
        normalized.remove(normalized.length() - 2);
    } else if (hasWattsSuffix) {
        normalized.remove(normalized.length() - 1);
    }

    char* parseEnd = nullptr;
    const float parsed = strtof(normalized.c_str(), &parseEnd);
    if (parseEnd == normalized.c_str() || *parseEnd != '\0' || !isfinite(parsed) || parsed <= 0.0f) {
        return 0;
    }

    if (hasMilliwattsSuffix) {
        return static_cast<uint16_t>(lroundf(parsed));
    }
    if (hasWattsSuffix || parsed < 10.0f) {
        return static_cast<uint16_t>(lroundf(parsed * 1000.0f));
    }
    return static_cast<uint16_t>(lroundf(parsed));
}

static String getBandLabelForIndex(uint8_t deviceIndex, size_t bandIndex) {
    DynamicJsonDocument* doc = getDeviceVtxDoc(deviceIndex);
    if (doc) {
        JsonArray bands = (*doc)["vtx_table"]["bands_list"].as<JsonArray>();
        if (!bands.isNull() && bandIndex < bands.size()) {
            const char* letter = bands[bandIndex]["letter"] | "";
            const char* name = bands[bandIndex]["name"] | "";
            if (letter[0] != '\0') {
                return String(letter);
            }
            if (name[0] != '\0') {
                return String(name);
            }
        }
    }
    if (bandIndex < (sizeof(kDefaultBands) / sizeof(kDefaultBands[0]))) {
        return String(kDefaultBands[bandIndex]);
    }
    return String();
}

static String getPowerLabelForIndex(uint8_t deviceIndex, size_t powerIndex) {
    DynamicJsonDocument* doc = getDeviceVtxDoc(deviceIndex);
    if (doc) {
        JsonArray levels = (*doc)["vtx_table"]["powerlevels_list"].as<JsonArray>();
        if (!levels.isNull() && powerIndex < levels.size()) {
            const char* label = levels[powerIndex]["label"] | "";
            if (label[0] != '\0') {
                String trimmed(label);
                trimmed.trim();
                return trimmed;
            }
        }
    }
    return String(getPowerValueForIndex(deviceIndex, powerIndex));
}

static uint16_t getTransmitPowerValueForDevice(uint8_t deviceIndex, uint8_t protocol, size_t powerIndex) {
    const uint16_t rawValue = getPowerValueForIndex(deviceIndex, powerIndex);
    if (protocol != VTX_PROTOCOL_TRAMP) {
        return rawValue;
    }

    const uint16_t labelValue = parsePowerLabelMilliwatts(getPowerLabelForIndex(deviceIndex, powerIndex));
    return labelValue > 0 ? labelValue : rawValue;
}

static uint8_t resolveMavlinkPowerIndex(uint8_t deviceIndex, const VtxDeviceConfig& device, const MavlinkVtxCommand& command) {
    if (command.powerValue == MAVLINK_VTX_KEEP_VALUE) {
        return device.manualPowerIndex;
    }

    const size_t powerCount = getPowerCount(deviceIndex);
    if (powerCount == 0) {
        return 0;
    }

    if (command.powerValue < powerCount) {
        return clampManualPowerIndexValue(deviceIndex, static_cast<uint8_t>(command.powerValue));
    }

    for (size_t powerIndex = 0; powerIndex < powerCount; powerIndex++) {
        if (command.powerValue == getTransmitPowerValueForDevice(deviceIndex, device.protocol, powerIndex) ||
            command.powerValue == getPowerValueForIndex(deviceIndex, powerIndex) ||
            command.powerValue == parsePowerLabelMilliwatts(getPowerLabelForIndex(deviceIndex, powerIndex))) {
            return static_cast<uint8_t>(powerIndex);
        }
    }

    return device.manualPowerIndex;
}

static void appendSelectionStateJson(uint8_t deviceIndex, JsonObject deviceObject) {
    const DeviceRuntimeState& state = deviceRuntime[deviceIndex];
    deviceObject["currentPwmUs"] = state.filteredPulse;

    if (!state.hasSelection) {
        deviceObject["band"] = nullptr;
        deviceObject["bandLabel"] = nullptr;
        deviceObject["channel"] = nullptr;
        deviceObject["powerIndex"] = nullptr;
        deviceObject["powerValue"] = nullptr;
        deviceObject["powerLabel"] = nullptr;
        deviceObject["frequency"] = nullptr;
        deviceObject["pitMode"] = nullptr;
        return;
    }

    const uint32_t bandCount = getBandCount(deviceIndex);
    const uint32_t channelCount = getChannelCount();
    const uint32_t powerCount = getPowerCount(deviceIndex);
    if (bandCount == 0 || channelCount == 0 || powerCount == 0 ||
        state.currentBandIndex >= bandCount ||
        state.currentChannelIndex >= channelCount ||
        state.currentPowerIndex >= powerCount) {
        return;
    }

    const uint32_t bandIndex = state.currentBandIndex;
    const uint32_t channelIndex = state.currentChannelIndex;
    const uint32_t powerIndex = state.currentPowerIndex;

    deviceObject["band"] = bandIndex + 1;
    deviceObject["bandLabel"] = getBandLabelForIndex(deviceIndex, bandIndex);
    deviceObject["channel"] = channelIndex + 1;
    deviceObject["powerIndex"] = powerIndex;
    deviceObject["powerValue"] = getPowerValueForIndex(deviceIndex, powerIndex);
    deviceObject["powerLabel"] = getPowerLabelForIndex(deviceIndex, powerIndex);
    deviceObject["frequency"] = getFrequencyForSelection(deviceIndex, bandIndex, channelIndex);
    deviceObject["pitMode"] = false;
}

static bool loadDeviceVtxTable(uint8_t deviceIndex, String* message) {
    if (deviceIndex >= config.deviceCount) {
        if (message) {
            *message = "invalid device index";
        }
        return false;
    }

    VtxDeviceConfig& device = config.devices[deviceIndex];
    if (strcmp(device.vtxTablePath, kEmbeddedDefaultTable) == 0) {
        DynamicJsonDocument* doc = nullptr;
        const String rawJson = String(EMBEDDED_VTXTABLE);
        if (!storageParseVtxTableJsonTo(rawJson, &doc, message)) {
            return false;
        }

        freeDeviceTable(deviceIndex);
        deviceRuntime[deviceIndex].vtxDoc = doc;
        deviceRuntime[deviceIndex].vtxJson = rawJson;
        return true;
    }

    if (!storageFileExists(device.vtxTablePath)) {
        strncpy(device.vtxTablePath, kEmbeddedDefaultTable, sizeof(device.vtxTablePath) - 1);
        device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
        return loadDeviceVtxTable(deviceIndex, message);
    }

    DynamicJsonDocument* doc = nullptr;
    String rawJson;
    if (!storageLoadVtxTableFileTo(device.vtxTablePath, &doc, &rawJson, message)) {
        return false;
    }

    freeDeviceTable(deviceIndex);
    deviceRuntime[deviceIndex].vtxDoc = doc;
    deviceRuntime[deviceIndex].vtxJson = rawJson;
    return true;
}

static bool loadAllDeviceVtxTables(String* message = nullptr) {
    bool changed = false;
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        if (!storageFileExists(config.devices[index].vtxTablePath)) {
            strncpy(config.devices[index].vtxTablePath, kEmbeddedDefaultTable, sizeof(config.devices[index].vtxTablePath) - 1);
            config.devices[index].vtxTablePath[sizeof(config.devices[index].vtxTablePath) - 1] = '\0';
            changed = true;
        }
        if (!loadDeviceVtxTable(index, message)) {
            return false;
        }
    }
    if (changed) {
        storageSaveAppConfig(config);
    }
    return true;
}

static bool validateDeviceConfigSet(const AppConfig& candidate, String* message) {
    bool anyEnabled = false;
    const uint8_t deviceCount = appConfigClampDeviceCount(candidate.deviceCount);
    const uint8_t localNodeId = sanitizeMavlinkNodeId(candidate.localNodeId, 1);
    if (candidate.boardRole == BOARD_ROLE_FC_BRIDGE) {
        if (!isValidGpio(static_cast<uint8_t>(candidate.mavlinkRxPin))) {
            if (message) {
                *message = "FC bridge role requires a valid MAVLink RX pin";
            }
            return false;
        }
        if (candidate.mavlinkTxPin < 0 || !isValidGpio(static_cast<uint8_t>(candidate.mavlinkTxPin))) {
            if (message) {
                *message = "FC bridge role requires a valid MAVLink TX pin";
            }
            return false;
        }
    } else if (!isValidOptionalGpio(candidate.mavlinkRxPin) || !isValidOptionalGpio(candidate.mavlinkTxPin)) {
        if (message) {
            *message = "MAVLink pins must be valid GPIO numbers or -1";
        }
        return false;
    }
    for (uint8_t left = 0; left < deviceCount; left++) {
        const VtxDeviceConfig& device = candidate.devices[left];
        if (!device.enabled) {
            continue;
        }
        anyEnabled = true;
        if ((device.controlMode == VTX_CONTROL_MODE_PWM && !isValidPwmInputPin(device.pwmInputPin)) ||
            !isValidGpio(device.vtxControlPin)) {
            if (message) {
                *message = "PWM pins must be valid GPIO numbers or -1; VTX control pins must be valid GPIO numbers";
            }
            return false;
        }
        if (device.controlMode == VTX_CONTROL_MODE_MAVLINK) {
            const uint8_t nodeId = sanitizeMavlinkNodeId(device.mavlinkNodeId, localNodeId);
            const uint8_t deviceId = sanitizeMavlinkDeviceId(device.mavlinkDeviceId, static_cast<uint8_t>(left + 1));
            if (nodeId == 0 || nodeId >= MAVLINK_VTX_BROADCAST_NODE_ID ||
                deviceId == 0 || deviceId == MAVLINK_VTX_ALL_DEVICES_ID) {
                if (message) {
                    *message = "MAVLink devices need nodeId 1-254 and deviceId 1-254";
                }
                return false;
            }
        }
        if (!storageFileExists(device.vtxTablePath)) {
            if (message) {
                *message = "device VTX table does not exist";
            }
            return false;
        }
        for (uint8_t right = left + 1; right < deviceCount; right++) {
            const VtxDeviceConfig& other = candidate.devices[right];
            if (!other.enabled) {
                continue;
            }
            if (device.pwmInputPin >= 0 && other.pwmInputPin >= 0 && device.pwmInputPin == other.pwmInputPin) {
                if (device.controlMode != VTX_CONTROL_MODE_PWM || other.controlMode != VTX_CONTROL_MODE_PWM) {
                    continue;
                }
                if (message) {
                    *message = "enabled devices must use unique PWM input pins";
                }
                return false;
            }
            if (device.vtxControlPin == other.vtxControlPin) {
                if (message) {
                    *message = "enabled devices must use unique VTX control pins";
                }
                return false;
            }
            if (device.controlMode == VTX_CONTROL_MODE_MAVLINK &&
                other.controlMode == VTX_CONTROL_MODE_MAVLINK &&
                sanitizeMavlinkNodeId(device.mavlinkNodeId, localNodeId) == sanitizeMavlinkNodeId(other.mavlinkNodeId, localNodeId) &&
                sanitizeMavlinkDeviceId(device.mavlinkDeviceId, static_cast<uint8_t>(left + 1)) == sanitizeMavlinkDeviceId(other.mavlinkDeviceId, static_cast<uint8_t>(right + 1))) {
                if (message) {
                    *message = "enabled MAVLink devices must use unique nodeId/deviceId pairs";
                }
                return false;
            }
        }
    }
    if (!anyEnabled) {
        if (message) {
            *message = "at least one VTX device must be enabled";
        }
        return false;
    }
    return true;
}

static void IRAM_ATTR handlePwmEdge(void* arg) {
    const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg));
    if (index >= MAX_VTX_DEVICES || index >= config.deviceCount) {
        return;
    }
    const VtxDeviceConfig& device = config.devices[index];
    DeviceRuntimeState& state = deviceRuntime[index];
    const uint32_t now = micros();
    const int level = gpio_get_level(static_cast<gpio_num_t>(device.pwmInputPin));
    if (level) {
        state.riseMicros = now;
    } else if (state.riseMicros != 0) {
        const uint32_t pulse = now - state.riseMicros;
        if (pulse >= 400 && pulse <= 3000) {
            state.capturedPulse = static_cast<uint16_t>(pulse);
            state.pulseAvailable = true;
        }
    }
}

static void initializePwmCapture() {
    resetRuntimeState();
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        if (!device.enabled || device.controlMode != VTX_CONTROL_MODE_PWM || device.pwmInputPin < 0) {
            continue;
        }
        pinMode(device.pwmInputPin, INPUT);
        attachInterruptArg(digitalPinToInterrupt(device.pwmInputPin), handlePwmEdge, reinterpret_cast<void*>(static_cast<uintptr_t>(index)), CHANGE);
    }
}

static void initializeVtxTransport() {
    Serial1.end();
    trampResetState();
    trampSetOneWirePin(-1);
    smartaudioSetOneWirePin(-1);
    smartaudioSetPrependZero(true);
    smartaudioEnableDebug(false);
    trampEnableDebug(false);
    activeTransportPin = -1;
    activeTransportProtocol = 0xFF;
    transportReady = true;
}

static void selectTransportForDevice(const VtxDeviceConfig& device) {
    if (activeTransportProtocol == device.protocol && activeTransportPin == device.vtxControlPin) {
        return;
    }

    Serial1.end();
    trampResetState();
    if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        trampSetOneWirePin(-1);
        smartaudioSetPrependZero(true);
        smartaudioSetOneWirePin(device.vtxControlPin);
    } else {
        smartaudioSetOneWirePin(-1);
        trampSetOneWirePin(device.vtxControlPin);
    }

    activeTransportProtocol = device.protocol;
    activeTransportPin = device.vtxControlPin;
}

static bool flushQueuedTransportForDevice(const VtxDeviceConfig& device, uint32_t timeoutMs) {
    const uint32_t startMs = millis();
    while (millis() - startMs < timeoutMs) {
        selectTransportForDevice(device);
        if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
            smartaudioProcess();
            if (!smartaudioIsBusy()) {
                return true;
            }
        } else {
            trampProcess();
            if (!trampIsBusy()) {
                return true;
            }
        }
        delay(5);
    }

    return false;
}

static bool applyVtxSelectionToDevice(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex, size_t powerIndex) {
    if (deviceIndex >= config.deviceCount) {
        return false;
    }
    const VtxDeviceConfig& device = config.devices[deviceIndex];
    if (!device.enabled) {
        return false;
    }

    selectTransportForDevice(device);
    const uint8_t band = static_cast<uint8_t>(bandIndex + 1);
    const uint8_t channel = static_cast<uint8_t>(channelIndex + 1);
    const uint16_t transmitPowerValue = getTransmitPowerValueForDevice(deviceIndex, device.protocol, powerIndex);
    const uint16_t frequency = getFrequencyForSelection(deviceIndex, bandIndex, channelIndex);
    DeviceRuntimeState& state = deviceRuntime[deviceIndex];

    if (state.hasSentSelection &&
        state.lastSentProtocol == device.protocol &&
        state.lastSentBand == band &&
        state.lastSentChannel == channel &&
        state.lastSentPowerValue == transmitPowerValue &&
        state.lastSentFrequency == frequency) {
        rememberSelection(deviceIndex, bandIndex, channelIndex, powerIndex);
        return true;
    }

    if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        if (!flushQueuedTransportForDevice(device, 500)) {
            return false;
        }
        if (!smartaudioEnqueueSetChannel(band, channel) ||
            !smartaudioEnqueueSetPower(static_cast<uint8_t>(transmitPowerValue))) {
            return false;
        }
        if (!flushQueuedTransportForDevice(device, 1000)) {
            return false;
        }
    } else {
        if (!trampInit()) {
            return false;
        }
        if (frequency > 0) {
            trampSendFrequency(frequency);
            delay(200);
        }
        trampSendPower(transmitPowerValue);
        delay(200);
        trampSendActiveState(true);
    }

    state.hasSentSelection = true;
    state.lastSentProtocol = device.protocol;
    state.lastSentBand = band;
    state.lastSentChannel = channel;
    state.lastSentPowerValue = transmitPowerValue;
    state.lastSentFrequency = frequency;
    rememberSelection(deviceIndex, bandIndex, channelIndex, powerIndex);
    return true;
}

static void processDevicePulse(uint8_t deviceIndex, uint16_t pulse) {
    if (deviceIndex >= config.deviceCount || config.devices[deviceIndex].controlMode != VTX_CONTROL_MODE_PWM) {
        return;
    }
    DeviceRuntimeState& state = deviceRuntime[deviceIndex];
    pulse = constrain(pulse, 500U, 2500U);
    if (!state.filterInitialized) {
        state.filteredPulse = pulse;
        state.filterInitialized = true;
    } else {
        state.filteredPulse = static_cast<uint16_t>((state.filteredPulse * 3U + pulse) / 4U);
    }

    const uint32_t bandCount = getBandCount(deviceIndex);
    const uint32_t channelCount = getChannelCount();
    const uint32_t powerCount = getPowerCount(deviceIndex);
    const uint32_t total = bandCount * channelCount * powerCount;
    if (total == 0) {
        return;
    }

    const int32_t idx = map(state.filteredPulse, 500, 2500, 0, total - 1);
    if (idx == state.lastMappedIndex) {
        state.candidateMappedIndex = -1;
        state.candidateCount = 0;
        return;
    }
    if (idx != state.candidateMappedIndex) {
        state.candidateMappedIndex = idx;
        state.candidateCount = 1;
    } else if (state.candidateCount < 255) {
        state.candidateCount++;
    }
    if (state.candidateCount < 3) {
        return;
    }

    state.lastMappedIndex = idx;
    state.candidateMappedIndex = -1;
    state.candidateCount = 0;

    uint32_t rem = static_cast<uint32_t>(idx);
    const uint32_t powIdx = rem % powerCount; rem /= powerCount;
    const uint32_t chanIdx = rem % channelCount; rem /= channelCount;
    const uint32_t bandIdx = rem % bandCount;
    applyVtxSelectionToDevice(deviceIndex, bandIdx, chanIdx, powIdx);
}

static void scheduleRestart() {
    restartScheduled = true;
    restartAtMs = millis() + 1200;
}

static bool deviceMatchesMavlinkTarget(const VtxDeviceConfig& device, const MavlinkVtxCommand& command) {
    if (!device.enabled || device.controlMode != VTX_CONTROL_MODE_MAVLINK) {
        return false;
    }
    const bool nodeMatches = command.nodeId == MAVLINK_VTX_BROADCAST_NODE_ID || device.mavlinkNodeId == command.nodeId;
    const bool deviceMatches = command.deviceId == MAVLINK_VTX_ALL_DEVICES_ID || device.mavlinkDeviceId == command.deviceId;
    return nodeMatches && deviceMatches;
}

static bool applyMavlinkCommandLocally(const MavlinkVtxCommand& command) {
    bool applied = false;
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        VtxDeviceConfig& device = config.devices[index];
        if (!deviceMatchesMavlinkTarget(device, command)) {
            continue;
        }

        const uint8_t band = command.band == 0 ? device.manualBand : clampManualBandValue(index, command.band);
        const uint8_t channel = command.channel == 0 ? device.manualChannel : clampManualChannelValue(command.channel);
        const uint8_t powerIndex = resolveMavlinkPowerIndex(index, device, command);

        device.manualBand = band;
        device.manualChannel = channel;
        device.manualPowerIndex = powerIndex;
        if (queueMavlinkSelectionForDevice(index, static_cast<size_t>(band - 1), static_cast<size_t>(channel - 1), static_cast<size_t>(powerIndex))) {
            applied = true;
        }
    }
    return applied;
}

static bool enqueueQueuedVtxSelection(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex, size_t powerIndex) {
    if (deviceIndex >= config.deviceCount) {
        return false;
    }

    const VtxDeviceConfig& device = config.devices[deviceIndex];
    if (!device.enabled) {
        return false;
    }

    selectTransportForDevice(device);

    if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        if (smartaudioIsBusy()) {
            return false;
        }

        return smartaudioEnqueueSetChannel(static_cast<uint8_t>(bandIndex + 1), static_cast<uint8_t>(channelIndex + 1)) &&
               smartaudioEnqueueSetPower(static_cast<uint8_t>(getTransmitPowerValueForDevice(deviceIndex, device.protocol, powerIndex)));
    }

    if (trampIsBusy()) {
        return false;
    }

    const uint16_t frequency = getFrequencyForSelection(deviceIndex, bandIndex, channelIndex);
    return trampEnqueueInit() &&
        (frequency == 0 || trampEnqueueSetFrequency(frequency)) &&
           trampEnqueueSetPower(getTransmitPowerValueForDevice(deviceIndex, device.protocol, powerIndex)) &&
           trampEnqueueSetActiveState(true);
}

static bool queueMavlinkSelectionForDevice(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex, size_t powerIndex) {
    if (deviceIndex >= MAX_VTX_DEVICES) {
        return false;
    }

    pendingMavlinkSelections[deviceIndex].pending = true;
    pendingMavlinkSelections[deviceIndex].bandIndex = static_cast<uint8_t>(bandIndex);
    pendingMavlinkSelections[deviceIndex].channelIndex = static_cast<uint8_t>(channelIndex);
    pendingMavlinkSelections[deviceIndex].powerIndex = static_cast<uint8_t>(powerIndex);
    rememberSelection(deviceIndex, bandIndex, channelIndex, powerIndex);
    return true;
}

static void processQueuedMavlinkSelections(void) {
    if (activeQueuedMavlinkDevice >= 0) {
        const uint8_t deviceIndex = static_cast<uint8_t>(activeQueuedMavlinkDevice);
        if (deviceIndex >= config.deviceCount) {
            activeQueuedMavlinkDevice = -1;
        } else {
            const VtxDeviceConfig& device = config.devices[deviceIndex];
            if (!device.enabled || device.controlMode != VTX_CONTROL_MODE_MAVLINK) {
                activeQueuedMavlinkDevice = -1;
            } else {
                selectTransportForDevice(device);
                if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
                    smartaudioProcess();
                    if (smartaudioIsBusy()) {
                        return;
                    }
                } else {
                    trampProcess();
                    if (trampIsBusy()) {
                        return;
                    }
                }
                activeQueuedMavlinkDevice = -1;
            }
        }
    }

    for (uint8_t index = 0; index < config.deviceCount; index++) {
        if (!pendingMavlinkSelections[index].pending) {
            continue;
        }

        const VtxDeviceConfig& device = config.devices[index];
        if (!device.enabled || device.controlMode != VTX_CONTROL_MODE_MAVLINK) {
            pendingMavlinkSelections[index].pending = false;
            continue;
        }

        const size_t bandIndex = pendingMavlinkSelections[index].bandIndex;
        const size_t channelIndex = pendingMavlinkSelections[index].channelIndex;
        const size_t powerIndex = pendingMavlinkSelections[index].powerIndex;

        if (!enqueueQueuedVtxSelection(index, bandIndex, channelIndex, powerIndex)) {
            return;
        }

        pendingMavlinkSelections[index].pending = false;
        activeQueuedMavlinkDevice = static_cast<int8_t>(index);
        selectTransportForDevice(device);
        if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
            smartaudioProcess();
            if (!smartaudioIsBusy()) {
                activeQueuedMavlinkDevice = -1;
            }
        } else {
            trampProcess();
            if (!trampIsBusy()) {
                activeQueuedMavlinkDevice = -1;
            }
        }
        return;
    }
}

static void processMavlinkInput() {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    while (fcMavSerial.available() > 0) {
        const uint8_t byte = static_cast<uint8_t>(fcMavSerial.read());
        mavlinkDebug.rxByteCount++;
        mavlinkDebug.lastByteAtMs = millis();
        if (!mavlink_parse_char(MAVLINK_COMM_0, byte, &mavlinkRxMessage, &mavlinkRxStatus)) {
            continue;
        }

        MavlinkFrameInfo frameInfo = {};
        frameInfo.complete = true;
        frameInfo.mavlink2 = (mavlinkRxMessage.magic == MAVLINK_STX);
        frameInfo.payloadLength = mavlinkRxMessage.len;
        frameInfo.messageId = mavlinkRxMessage.msgid;
        frameInfo.sourceSystem = mavlinkRxMessage.sysid;
        frameInfo.sourceComponent = mavlinkRxMessage.compid;
        mavlinkDebug.seenFrameCount++;

        if (mavlinkRxMessage.msgid == MAVLINK_MSG_ID_PING) {
            mavlink_ping_t ping = {};
            mavlink_msg_ping_decode(&mavlinkRxMessage, &ping);
            mavlinkDebug.lastFrame = frameInfo;
            mavlinkDebug.hasLastFrame = true;
            Serial.printf("RX PING from=%u/%u target=%u/%u seq=%lu\n",
                          static_cast<unsigned>(mavlinkRxMessage.sysid),
                          static_cast<unsigned>(mavlinkRxMessage.compid),
                          static_cast<unsigned>(ping.target_system),
                          static_cast<unsigned>(ping.target_component),
                          static_cast<unsigned long>(ping.seq));
            if (mavlinkTargetMatchesLocal(ping.target_system, ping.target_component)) {
                sendMavlinkPingResponse(mavlinkRxMessage, ping);
            }
            continue;
        }

        if (mavlinkRxMessage.msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
            mavlinkDebug.lastFrame = frameInfo;
            mavlinkDebug.hasLastFrame = true;
            continue;
        }

        mavlinkDebug.seenCommandLongCount++;
        mavlink_command_long_t commandLong = {};
        mavlink_msg_command_long_decode(&mavlinkRxMessage, &commandLong);
        frameInfo.targetSystem = commandLong.target_system;
        frameInfo.targetComponent = commandLong.target_component;
        frameInfo.commandId = commandLong.command;
        mavlinkDebug.lastFrame = frameInfo;
        mavlinkDebug.hasLastFrame = true;

        if (!mavlinkTargetMatchesLocal(commandLong.target_system, commandLong.target_component)) {
            continue;
        }

        mavlinkDebug.parsedCommandCount++;
        mavlinkDebug.lastCommandAtMs = millis();

        if (commandLong.command == MAV_CMD_REQUEST_MESSAGE) {
            const uint32_t requestedMessageId = static_cast<uint32_t>(lroundf(commandLong.param1));
            if (requestedMessageId == MAVLINK_MSG_ID_AUTOPILOT_VERSION) {
                sendMavlinkCommandAck(commandLong.command,
                                      mavlinkRxMessage.sysid,
                                      mavlinkRxMessage.compid,
                                      kMavResultAccepted);
                sendMavlinkAutopilotVersion(mavlinkRxMessage);
            } else {
                sendMavlinkCommandAck(commandLong.command,
                                      mavlinkRxMessage.sysid,
                                      mavlinkRxMessage.compid,
                                      kMavResultUnsupported);
                Serial.printf("UNSUPPORTED REQUEST_MESSAGE msgid=%lu from=%u/%u\n",
                              static_cast<unsigned long>(requestedMessageId),
                              static_cast<unsigned>(mavlinkRxMessage.sysid),
                              static_cast<unsigned>(mavlinkRxMessage.compid));
            }
            continue;
        }

        if (commandLong.command != MAVLINK_VTX_COMMAND_ID) {
            sendMavlinkCommandAck(commandLong.command,
                                  mavlinkRxMessage.sysid,
                                  mavlinkRxMessage.compid,
                                  kMavResultUnsupported);
            Serial.printf("IGNORED COMMAND_LONG command=%u from=%u/%u target=%u/%u\n",
                          static_cast<unsigned>(commandLong.command),
                          static_cast<unsigned>(mavlinkRxMessage.sysid),
                          static_cast<unsigned>(mavlinkRxMessage.compid),
                          static_cast<unsigned>(commandLong.target_system),
                          static_cast<unsigned>(commandLong.target_component));
            continue;
        }

        MavlinkVtxCommand command = {};
        populateVtxCommand(mavlinkRxMessage, commandLong, &command);
        mavlinkDebug.lastCommand = command;
        mavlinkDebug.hasLastCommand = true;

        const bool localApplied = applyMavlinkCommandLocally(command);
        if (localApplied) {
            mavlinkDebug.localApplyCount++;
        }
        const uint8_t ackResult = localApplied ? kMavResultAccepted : kMavResultFailed;
        sendMavlinkCommandAck(command, ackResult);
        logMavlinkCommand("UART", command, localApplied, false);
    }
}

static void initializeStorageAndConfig() {
    appConfigSetDefaults(config);
    if (!storageBegin()) {
        Serial.println("LittleFS failed to mount");
        return;
    }
    storageEnsureVtxFile(kEmbeddedDefaultTable);
    if (!storageLoadAppConfig(&config)) {
        Serial.println("Failed to load app config, saving defaults");
        storageSaveAppConfig(config);
    }
    config.deviceCount = appConfigClampDeviceCount(config.deviceCount);
    String message;
    if (!loadAllDeviceVtxTables(&message)) {
        Serial.printf("Failed to load device VTX tables: %s\n", message.c_str());
    }
}

static void disableWireless() {
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}

static void initializeMavlinkSerial() {
    memset(&mavlinkRxMessage, 0, sizeof(mavlinkRxMessage));
    memset(&mavlinkRxStatus, 0, sizeof(mavlinkRxStatus));
    fcMavSerial.end();
    lastMavlinkHeartbeatMs = 0;
    lastMavlinkStatusTextMs = 0;
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    if (!isValidGpio(static_cast<uint8_t>(config.mavlinkRxPin))) {
        Serial.println("MAVLink UART disabled: invalid RX pin");
        return;
    }
    if (config.mavlinkTxPin < 0 || !isValidGpio(static_cast<uint8_t>(config.mavlinkTxPin))) {
        Serial.println("MAVLink UART disabled: invalid TX pin");
        return;
    }
    fcMavSerial.begin(config.mavlinkBaud, SERIAL_8N1, config.mavlinkRxPin, config.mavlinkTxPin);
    Serial.printf("MAVLink UART started on UART0 rx=%d tx=%d baud=%lu sysid=%u compid=%u\n", config.mavlinkRxPin, config.mavlinkTxPin, static_cast<unsigned long>(config.mavlinkBaud), static_cast<unsigned>(config.localNodeId), static_cast<unsigned>(kMavlinkComponentId));
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("ESP32-C3 VTX controller starting");

    initializeStorageAndConfig();
    initializePwmCapture();
    initializeVtxTransport();
    sanitizeDeviceSelections();
    initializeMavlinkSerial();
    initializeWireless();
    configureWebServer();

    Serial.printf("Configured devices: %u\n", config.deviceCount);
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        Serial.printf("[%u] %s pwm=%d vtx=%u proto=%s control=%s node=%u device=%u table=%s enabled=%u\n", index, device.name, device.pwmInputPin, device.vtxControlPin, appConfigProtocolToString(device.protocol), appConfigControlModeToString(device.controlMode), device.mavlinkNodeId, device.mavlinkDeviceId, device.vtxTablePath, device.enabled ? 1 : 0);
    }
    Serial.printf("Role=%s node=%u mavlinkRx=%d mavlinkTx=%d mavlinkBaud=%lu\n", appConfigBoardRoleToString(config.boardRole), config.localNodeId, config.mavlinkRxPin, config.mavlinkTxPin, static_cast<unsigned long>(config.mavlinkBaud));
    Serial.printf("Web UI: http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
    server.handleClient();
    if (config.boardRole == BOARD_ROLE_FC_BRIDGE && millis() - lastMavlinkHeartbeatMs >= kMavlinkHeartbeatIntervalMs) {
        lastMavlinkHeartbeatMs = millis();
        sendMavlinkHeartbeat();
    }
    if (config.boardRole == BOARD_ROLE_FC_BRIDGE && millis() - lastMavlinkStatusTextMs >= kMavlinkStatusTextIntervalMs) {
        lastMavlinkStatusTextMs = millis();
        sendMavlinkStatusText();
    }
    processMavlinkInput();
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        if (!config.devices[index].enabled) {
            continue;
        }
        if (deviceRuntime[index].pulseAvailable) {
            noInterrupts();
            const uint16_t pulse = deviceRuntime[index].capturedPulse;
            deviceRuntime[index].pulseAvailable = false;
            interrupts();
            processDevicePulse(index, pulse);
        }
    }
    processQueuedMavlinkSelections();
    if (millis() - lastWirelessHealthCheckMs >= kWirelessHealthCheckIntervalMs) {
        lastWirelessHealthCheckMs = millis();
        if (!isWirelessApHealthy()) {
            Serial.println("AP health check failed, reinitializing wireless stack");
            initializeWireless();
        }
    }
    if (espNowActive && millis() - lastEspNowBroadcastMs >= 5000 && quickEspNow.readyToSendData()) {
        lastEspNowBroadcastMs = millis();
        sendCompactEspNowStatus(ESPNOW_BROADCAST_ADDRESS);
    }
    if (restartScheduled && millis() >= restartAtMs) {
        ESP.restart();
    }
}
