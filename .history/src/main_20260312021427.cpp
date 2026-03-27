#include <Arduino.h>
#include <LittleFS.h>
#include <QuickEspNow.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include "app_config.h"
#include "embedded_vtxtable.h"
#include "mavlink_bridge.h"
#include "smartaudio.h"
#include "storage.h"
#include "tramp.h"
#include "web_ui.h"

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
static const char* kApSsid = "ESP32C3-VTX";
static const char* kApPassword = "13579135";
static const char* kEmbeddedDefaultTable = DEFAULT_VTX_TABLE_PATH;
static HardwareSerial fcMavSerial(1);
static const uint8_t kMavlinkComponentId = 158;
static const uint8_t kMavResultAccepted = 0;
static const uint8_t kMavResultDenied = 2;
static const uint8_t kMavResultUnsupported = 3;
static const uint8_t kMavResultFailed = 4;

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
    uint8_t lastSentPowerValue;
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
    uint32_t forwardCount;
    uint32_t espNowRxCount;
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

static WebServer server(80);
static AppConfig config;
static DeviceRuntimeState deviceRuntime[MAX_VTX_DEVICES];
static MavlinkCommandParser mavlinkParser;
static MavlinkDebugState mavlinkDebug = {};
static bool transportReady = false;
static bool restartScheduled = false;
static unsigned long restartAtMs = 0;
static unsigned long lastEspNowBroadcastMs = 0;
static unsigned long lastMavlinkHeartbeatMs = 0;
static String uploadPath;
static String uploadBuffer;
static String uploadError;
static bool uploadOk = false;

static bool loadDeviceVtxTable(uint8_t deviceIndex, String* message = nullptr);
static size_t getBandCount(uint8_t deviceIndex);
static size_t getChannelCount();
static size_t getPowerCount(uint8_t deviceIndex);
static uint16_t getFrequencyForSelection(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex);
static uint8_t getPowerValueForIndex(uint8_t deviceIndex, size_t powerIndex);
static String getBandLabelForIndex(uint8_t deviceIndex, size_t bandIndex);
static String getPowerLabelForIndex(uint8_t deviceIndex, size_t powerIndex);
static bool isValidGpio(uint8_t pin);
static bool sendEspNowJson(const uint8_t* address, JsonDocument& doc);

static uint8_t mavlinkTxSequence = 0;

static uint16_t mavlinkX25Accumulate(uint8_t data, uint16_t crc) {
    uint8_t tmp = data ^ static_cast<uint8_t>(crc & 0xFF);
    tmp ^= static_cast<uint8_t>(tmp << 4);
    return static_cast<uint16_t>((crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8) ^
                                 (static_cast<uint16_t>(tmp) << 3) ^
                                 (static_cast<uint16_t>(tmp) >> 4));
}

static uint16_t mavlinkComputeFrameCrc(const uint8_t* data, size_t length, uint8_t crcExtra) {
    uint16_t crc = 0xFFFF;
    for (size_t index = 0; index < length; index++) {
        crc = mavlinkX25Accumulate(data[index], crc);
    }
    return mavlinkX25Accumulate(crcExtra, crc);
}

static void writeUint16Le(uint8_t* buffer, uint16_t value) {
    buffer[0] = static_cast<uint8_t>(value & 0xFF);
    buffer[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static void writeInt32Le(uint8_t* buffer, int32_t value) {
    buffer[0] = static_cast<uint8_t>(value & 0xFF);
    buffer[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buffer[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static bool mavlinkCommandTargetsBridge(const MavlinkVtxCommand& command) {
    const bool systemMatches = command.targetSystem == 0 || command.targetSystem == config.localNodeId;
    const bool componentMatches = command.targetComponent == 0 || command.targetComponent == kMavlinkComponentId;
    return systemMatches && componentMatches;
}

static void sendMavlinkCommandAck(const MavlinkVtxCommand& command, uint8_t result) {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    if (!fcMavSerial) {
        return;
    }

    uint8_t payload[10] = {};
    writeUint16Le(payload + 0, command.commandId);
    payload[2] = result;
    payload[3] = 0;
    writeInt32Le(payload + 4, 0);
    payload[8] = command.sourceSystem;
    payload[9] = command.sourceComponent;

    uint8_t frame[18] = {};
    frame[0] = 0xFE;
    frame[1] = sizeof(payload);
    frame[2] = mavlinkTxSequence++;
    frame[3] = config.localNodeId;
    frame[4] = kMavlinkComponentId;
    frame[5] = 77;
    memcpy(frame + 6, payload, sizeof(payload));

    const uint16_t crc = mavlinkComputeFrameCrc(frame + 1, 5 + sizeof(payload), 143);
    frame[16] = static_cast<uint8_t>(crc & 0xFF);
    frame[17] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    fcMavSerial.write(frame, sizeof(frame));
    mavlinkDebug.ackTxCount++;
    mavlinkDebug.lastTxAtMs = millis();
}

static void sendMavlinkHeartbeat() {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    if (!fcMavSerial) {
        return;
    }

    uint8_t payload[9] = {
        0, 0, 0, 0,
        18,
        8,
        0,
        4,
        3,
    };

    uint8_t frame[17];
    frame[0] = 0xFE;
    frame[1] = sizeof(payload);
    frame[2] = mavlinkTxSequence++;
    frame[3] = config.localNodeId;
    frame[4] = kMavlinkComponentId;
    frame[5] = 0;
    memcpy(frame + 6, payload, sizeof(payload));
    const uint16_t crc = mavlinkComputeFrameCrc(frame + 1, 5 + sizeof(payload), 50);
    frame[15] = static_cast<uint8_t>(crc & 0xFF);
    frame[16] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    fcMavSerial.write(frame, sizeof(frame));
    mavlinkDebug.heartbeatTxCount++;
    mavlinkDebug.lastTxAtMs = millis();
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
        static_cast<unsigned>(command.powerIndex),
        localApplied ? 1 : 0,
        forwarded ? 1 : 0);
}

static bool isValidOptionalGpio(int pin) {
    return pin < 0 || isValidGpio(static_cast<uint8_t>(pin));
}

static bool hasEnabledTrampDevice(const AppConfig& candidate) {
    const uint8_t deviceCount = appConfigClampDeviceCount(candidate.deviceCount);
    for (uint8_t index = 0; index < deviceCount; index++) {
        const VtxDeviceConfig& device = candidate.devices[index];
        if (device.enabled && device.protocol == VTX_PROTOCOL_TRAMP) {
            return true;
        }
    }
    return false;
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
        config.mavlinkBaud = 115200;
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
    }
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
        if (!isValidOptionalGpio(candidate.mavlinkTxPin)) {
            if (message) {
                *message = "MAVLink TX pin must be a valid GPIO or -1";
            }
            return false;
        }
        if (hasEnabledTrampDevice(candidate)) {
            if (message) {
                *message = "FC bridge role cannot be combined with enabled TRAMP devices on ESP32-C3; UART1 is needed for MAVLink";
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
        if ((device.controlMode == VTX_CONTROL_MODE_PWM && !isValidGpio(device.pwmInputPin)) ||
            !isValidGpio(device.vtxControlPin)) {
            if (message) {
                *message = "device pins must be valid GPIO numbers";
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
            if (device.pwmInputPin == other.pwmInputPin) {
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
        if (!device.enabled || device.controlMode != VTX_CONTROL_MODE_PWM) {
            continue;
        }
        pinMode(device.pwmInputPin, INPUT);
        attachInterruptArg(digitalPinToInterrupt(device.pwmInputPin), handlePwmEdge, reinterpret_cast<void*>(static_cast<uintptr_t>(index)), CHANGE);
    }
}

static void initializeVtxTransport() {
    Serial1.end();
    smartaudioSetOneWirePin(-1);
    smartaudioSetPrependZero(true);
    smartaudioEnableDebug(false);
    trampEnableDebug(false);
    transportReady = true;
}

static void selectTransportForDevice(const VtxDeviceConfig& device) {
    Serial1.end();
    if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        smartaudioSetPrependZero(true);
        smartaudioSetOneWirePin(device.vtxControlPin);
    } else {
        smartaudioSetOneWirePin(-1);
        Serial1.begin(9600, SERIAL_8N1, device.vtxControlPin, device.vtxControlPin);
    }
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
    const uint8_t powerValue = getPowerValueForIndex(deviceIndex, powerIndex);
    const uint16_t frequency = getFrequencyForSelection(deviceIndex, bandIndex, channelIndex);
    DeviceRuntimeState& state = deviceRuntime[deviceIndex];

    if (state.hasSentSelection &&
        state.lastSentProtocol == device.protocol &&
        state.lastSentBand == band &&
        state.lastSentChannel == channel &&
        state.lastSentPowerValue == powerValue &&
        state.lastSentFrequency == frequency) {
        rememberSelection(deviceIndex, bandIndex, channelIndex, powerIndex);
        return true;
    }

    if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        smartaudioSendBandChannel(band, channel);
        delay(80);
        selectTransportForDevice(device);
        smartaudioSendPower(powerValue);
    } else {
        if (frequency > 0) {
            trampSendFrequency(frequency);
            delay(20);
        }
        trampSendPower(powerValue);
        delay(20);
        trampSendActiveState(true);
    }

    state.hasSentSelection = true;
    state.lastSentProtocol = device.protocol;
    state.lastSentBand = band;
    state.lastSentChannel = channel;
    state.lastSentPowerValue = powerValue;
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

static void applyConfiguredSerialSelections() {
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        if (!device.enabled || device.controlMode != VTX_CONTROL_MODE_SERIAL) {
            continue;
        }
        applyVtxSelectionToDevice(
            index,
            static_cast<size_t>(clampManualBandValue(index, device.manualBand) - 1),
            static_cast<size_t>(clampManualChannelValue(device.manualChannel) - 1),
            static_cast<size_t>(clampManualPowerIndexValue(index, device.manualPowerIndex)));
    }
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
        const uint8_t powerIndex = command.powerIndex == MAVLINK_VTX_KEEP_VALUE
            ? device.manualPowerIndex
            : clampManualPowerIndexValue(index, command.powerIndex);

        device.manualBand = band;
        device.manualChannel = channel;
        device.manualPowerIndex = powerIndex;
        if (applyVtxSelectionToDevice(index, static_cast<size_t>(band - 1), static_cast<size_t>(channel - 1), static_cast<size_t>(powerIndex))) {
            applied = true;
        }
    }
    return applied;
}

static bool forwardMavlinkCommandOverEspNow(const MavlinkVtxCommand& command) {
    if (!config.espNowEnabled || !quickEspNow.readyToSendData()) {
        return false;
    }

    DynamicJsonDocument doc(256);
    doc["cmd"] = "mavlink_apply";
    doc["nodeId"] = command.nodeId;
    doc["deviceId"] = command.deviceId;
    doc["band"] = command.band;
    doc["channel"] = command.channel;
    doc["powerIndex"] = command.powerIndex;
    doc["flags"] = command.flags;
    return sendEspNowJson(ESPNOW_BROADCAST_ADDRESS, doc);
}

static void processMavlinkInput() {
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    while (fcMavSerial.available() > 0) {
        const uint8_t byte = static_cast<uint8_t>(fcMavSerial.read());
        mavlinkDebug.rxByteCount++;
        mavlinkDebug.lastByteAtMs = millis();
        MavlinkFrameInfo frameInfo = {};
        MavlinkVtxCommand command;
        const bool parsed = mavlinkParser.ingest(byte, &frameInfo, &command);
        if (frameInfo.complete) {
            mavlinkDebug.seenFrameCount++;
            mavlinkDebug.lastFrame = frameInfo;
            mavlinkDebug.hasLastFrame = true;
            if (frameInfo.messageId == 76) {
                mavlinkDebug.seenCommandLongCount++;
            }
        }
        if (!parsed) {
            continue;
        }

        mavlinkDebug.parsedCommandCount++;
        mavlinkDebug.lastCommandAtMs = millis();
        mavlinkDebug.lastCommand = command;
        mavlinkDebug.hasLastCommand = true;

        if (!mavlinkCommandTargetsBridge(command)) {
            sendMavlinkCommandAck(command, kMavResultDenied);
            continue;
        }
        const bool localApplied = applyMavlinkCommandLocally(command);
        const bool shouldForward = command.nodeId == MAVLINK_VTX_BROADCAST_NODE_ID || command.nodeId != config.localNodeId;
        bool forwarded = false;
        if (shouldForward) {
            forwarded = forwardMavlinkCommandOverEspNow(command);
            if (forwarded) {
                mavlinkDebug.forwardCount++;
            }
        }
        if (localApplied) {
            mavlinkDebug.localApplyCount++;
            storageSaveAppConfig(config);
        }
        const uint8_t ackResult = (localApplied || forwarded) ? kMavResultAccepted : kMavResultFailed;
        sendMavlinkCommandAck(command, ackResult);
        logMavlinkCommand("UART", command, localApplied, forwarded);
    }
}

static void appendStateJson(JsonObject root) {
    root["wifiChannel"] = config.wifiChannel;
    root["espNowEnabled"] = config.espNowEnabled;
    root["boardRole"] = appConfigBoardRoleToString(config.boardRole);
    root["localNodeId"] = config.localNodeId;
    root["mavlinkRxPin"] = config.mavlinkRxPin;
    root["mavlinkTxPin"] = config.mavlinkTxPin;
    root["mavlinkBaud"] = config.mavlinkBaud;
    JsonObject mavlinkObject = root.createNestedObject("mavlinkDebug");
    mavlinkObject["rxByteCount"] = mavlinkDebug.rxByteCount;
    mavlinkObject["seenFrameCount"] = mavlinkDebug.seenFrameCount;
    mavlinkObject["seenCommandLongCount"] = mavlinkDebug.seenCommandLongCount;
    mavlinkObject["parsedCommandCount"] = mavlinkDebug.parsedCommandCount;
    mavlinkObject["localApplyCount"] = mavlinkDebug.localApplyCount;
    mavlinkObject["forwardCount"] = mavlinkDebug.forwardCount;
    mavlinkObject["espNowRxCount"] = mavlinkDebug.espNowRxCount;
    mavlinkObject["heartbeatTxCount"] = mavlinkDebug.heartbeatTxCount;
    mavlinkObject["ackTxCount"] = mavlinkDebug.ackTxCount;
    mavlinkObject["lastByteAtMs"] = mavlinkDebug.lastByteAtMs;
    mavlinkObject["lastCommandAtMs"] = mavlinkDebug.lastCommandAtMs;
    mavlinkObject["lastTxAtMs"] = mavlinkDebug.lastTxAtMs;
    if (mavlinkDebug.hasLastFrame) {
        JsonObject lastFrame = mavlinkObject.createNestedObject("lastFrame");
        lastFrame["messageId"] = mavlinkDebug.lastFrame.messageId;
        lastFrame["sourceSystem"] = mavlinkDebug.lastFrame.sourceSystem;
        lastFrame["sourceComponent"] = mavlinkDebug.lastFrame.sourceComponent;
        lastFrame["targetSystem"] = mavlinkDebug.lastFrame.targetSystem;
        lastFrame["targetComponent"] = mavlinkDebug.lastFrame.targetComponent;
        lastFrame["commandId"] = mavlinkDebug.lastFrame.commandId;
        lastFrame["mavlink2"] = mavlinkDebug.lastFrame.mavlink2;
        lastFrame["payloadLength"] = mavlinkDebug.lastFrame.payloadLength;
    }
    if (mavlinkDebug.hasLastCommand) {
        JsonObject lastCommand = mavlinkObject.createNestedObject("lastCommand");
        lastCommand["commandId"] = mavlinkDebug.lastCommand.commandId;
        lastCommand["sourceSystem"] = mavlinkDebug.lastCommand.sourceSystem;
        lastCommand["sourceComponent"] = mavlinkDebug.lastCommand.sourceComponent;
        lastCommand["targetSystem"] = mavlinkDebug.lastCommand.targetSystem;
        lastCommand["targetComponent"] = mavlinkDebug.lastCommand.targetComponent;
        lastCommand["nodeId"] = mavlinkDebug.lastCommand.nodeId;
        lastCommand["deviceId"] = mavlinkDebug.lastCommand.deviceId;
        lastCommand["band"] = mavlinkDebug.lastCommand.band;
        lastCommand["channel"] = mavlinkDebug.lastCommand.channel;
        lastCommand["powerIndex"] = mavlinkDebug.lastCommand.powerIndex;
        lastCommand["flags"] = mavlinkDebug.lastCommand.flags;
    }
    root["transportReady"] = transportReady;
    root["restartPending"] = restartScheduled;
    root["ssid"] = WiFi.softAPSSID();
    root["ip"] = WiFi.softAPIP().toString();
    root["deviceCount"] = config.deviceCount;

    JsonArray devices = root.createNestedArray("devices");
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        const DeviceRuntimeState& state = deviceRuntime[index];
        JsonObject deviceObject = devices.createNestedObject();
        deviceObject["index"] = index;
        deviceObject["name"] = device.name;
        deviceObject["pwmInputPin"] = device.pwmInputPin;
        deviceObject["vtxControlPin"] = device.vtxControlPin;
        deviceObject["protocol"] = appConfigProtocolToString(device.protocol);
        deviceObject["controlMode"] = appConfigControlModeToString(device.controlMode);
        deviceObject["enabled"] = device.enabled;
        deviceObject["mavlinkNodeId"] = device.mavlinkNodeId;
        deviceObject["mavlinkDeviceId"] = device.mavlinkDeviceId;
        deviceObject["vtxTablePath"] = device.vtxTablePath;
        deviceObject["filteredPulse"] = state.filteredPulse;
        if (device.controlMode == VTX_CONTROL_MODE_PWM) {
            deviceObject["lastMappedIndex"] = state.lastMappedIndex;
        } else {
            deviceObject["lastMappedIndex"] = nullptr;
        }
        appendControlOptionsJson(index, deviceObject);
        appendSelectionStateJson(index, deviceObject);
    }

    JsonArray tables = root.createNestedArray("tables");
    storageListVtxFiles(tables);
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        if (!jsonArrayContainsString(tables, config.devices[index].vtxTablePath)) {
            tables.add(config.devices[index].vtxTablePath);
        }
    }
    if (storageFileExists(kEmbeddedDefaultTable) && !jsonArrayContainsString(tables, kEmbeddedDefaultTable)) {
        tables.add(kEmbeddedDefaultTable);
    }
}

static bool sendEspNowJson(const uint8_t* address, JsonDocument& doc) {
    char payload[250];
    const size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len == 0 || len >= sizeof(payload)) {
        return false;
    }
    return quickEspNow.send(address, reinterpret_cast<uint8_t*>(payload), len) == 0;
}

static void sendCompactEspNowStatus(const uint8_t* address) {
    DynamicJsonDocument doc(512);
    doc["type"] = "status";
    doc["role"] = appConfigBoardRoleToString(config.boardRole);
    doc["nodeId"] = config.localNodeId;
    doc["devices"] = config.deviceCount;
    JsonArray items = doc.createNestedArray("items");
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        JsonObject item = items.createNestedObject();
        item["name"] = config.devices[index].name;
        item["protocol"] = appConfigProtocolToString(config.devices[index].protocol);
        item["controlMode"] = appConfigControlModeToString(config.devices[index].controlMode);
        item["nodeId"] = config.devices[index].mavlinkNodeId;
        item["deviceId"] = config.devices[index].mavlinkDeviceId;
        item["table"] = baseNameFromPath(config.devices[index].vtxTablePath);
        item["pulse"] = deviceRuntime[index].filteredPulse;
        item["slot"] = deviceRuntime[index].lastMappedIndex;
        JsonObject selection = item.createNestedObject("selection");
        appendSelectionStateJson(index, selection);
    }
    sendEspNowJson(address, doc);
}

static bool updateConfigFromJson(JsonVariantConst input, bool* restartRequired, String* message) {
    AppConfig updated = config;

    if (!input["wifiChannel"].isNull()) {
        updated.wifiChannel = constrain(input["wifiChannel"].as<int>(), 1, 13);
    }
    if (!input["espNowEnabled"].isNull()) {
        updated.espNowEnabled = input["espNowEnabled"].as<bool>();
    }
    if (!input["boardRole"].isNull()) {
        String boardRoleName = input["boardRole"].as<String>();
        uint8_t boardRole = updated.boardRole;
        if (!appConfigBoardRoleFromString(boardRoleName, boardRole)) {
            if (message) {
                *message = "invalid board role";
            }
            return false;
        }
        updated.boardRole = boardRole;
    }
    if (!input["localNodeId"].isNull()) {
        updated.localNodeId = sanitizeMavlinkNodeId(static_cast<uint8_t>(input["localNodeId"].as<int>()), updated.localNodeId);
    }
    if (!input["mavlinkRxPin"].isNull()) {
        updated.mavlinkRxPin = static_cast<int8_t>(input["mavlinkRxPin"].as<int>());
    }
    if (!input["mavlinkTxPin"].isNull()) {
        updated.mavlinkTxPin = static_cast<int8_t>(input["mavlinkTxPin"].as<int>());
    }
    if (!input["mavlinkBaud"].isNull()) {
        updated.mavlinkBaud = static_cast<uint32_t>(max(1200, input["mavlinkBaud"].as<int>()));
    }
    JsonArrayConst devices = input["devices"].as<JsonArrayConst>();
    if (!devices.isNull()) {
        updated.deviceCount = appConfigClampDeviceCount(devices.size());
        for (uint8_t index = 0; index < MAX_VTX_DEVICES; index++) {
            appConfigSetDefaultDevice(updated.devices[index], index);
        }
        for (uint8_t index = 0; index < updated.deviceCount; index++) {
            JsonObjectConst deviceObject = devices[index].as<JsonObjectConst>();
            VtxDeviceConfig& device = updated.devices[index];
            const char* name = deviceObject["name"] | device.name;
            strncpy(device.name, name, sizeof(device.name) - 1);
            device.name[sizeof(device.name) - 1] = '\0';
            device.pwmInputPin = deviceObject["pwmInputPin"] | device.pwmInputPin;
            device.vtxControlPin = deviceObject["vtxControlPin"] | device.vtxControlPin;
            if (!deviceObject["protocol"].isNull()) {
                String protocolName = deviceObject["protocol"].as<String>();
                uint8_t protocol = device.protocol;
                if (!appConfigProtocolFromString(protocolName, protocol)) {
                    if (message) {
                        *message = "invalid device protocol";
                    }
                    return false;
                }
                device.protocol = protocol;
            }
            if (!deviceObject["controlMode"].isNull()) {
                String controlModeName = deviceObject["controlMode"].as<String>();
                uint8_t controlMode = device.controlMode;
                if (!appConfigControlModeFromString(controlModeName, controlMode)) {
                    if (message) {
                        *message = "invalid device control mode";
                    }
                    return false;
                }
                device.controlMode = controlMode;
            }
            device.enabled = deviceObject["enabled"].isNull() ? true : deviceObject["enabled"].as<bool>();
            device.manualBand = deviceObject["manualBand"] | device.manualBand;
            device.manualChannel = deviceObject["manualChannel"] | device.manualChannel;
            device.manualPowerIndex = deviceObject["manualPowerIndex"] | device.manualPowerIndex;
            device.mavlinkNodeId = deviceObject["mavlinkNodeId"] | updated.localNodeId;
            device.mavlinkDeviceId = deviceObject["mavlinkDeviceId"] | device.mavlinkDeviceId;
            if (!deviceObject["vtxTablePath"].isNull()) {
                String tablePath = normalizeTablePath(deviceObject["vtxTablePath"].as<String>());
                strncpy(device.vtxTablePath, tablePath.c_str(), sizeof(device.vtxTablePath) - 1);
                device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
            }
        }
    }

    updated.localNodeId = sanitizeMavlinkNodeId(updated.localNodeId, 1);
    for (uint8_t index = 0; index < updated.deviceCount; index++) {
        updated.devices[index].mavlinkNodeId = sanitizeMavlinkNodeId(updated.devices[index].mavlinkNodeId, updated.localNodeId);
        updated.devices[index].mavlinkDeviceId = sanitizeMavlinkDeviceId(updated.devices[index].mavlinkDeviceId, static_cast<uint8_t>(index + 1));
    }

    if (!validateDeviceConfigSet(updated, message)) {
        return false;
    }

    bool needsRestart =
        updated.wifiChannel != config.wifiChannel ||
        updated.espNowEnabled != config.espNowEnabled ||
        updated.boardRole != config.boardRole ||
        updated.localNodeId != config.localNodeId ||
        updated.mavlinkRxPin != config.mavlinkRxPin ||
        updated.mavlinkTxPin != config.mavlinkTxPin ||
        updated.mavlinkBaud != config.mavlinkBaud ||
        updated.deviceCount != config.deviceCount;

    if (!needsRestart) {
        for (uint8_t index = 0; index < updated.deviceCount; index++) {
            const VtxDeviceConfig& before = config.devices[index];
            const VtxDeviceConfig& after = updated.devices[index];
            if (before.pwmInputPin != after.pwmInputPin ||
                before.vtxControlPin != after.vtxControlPin ||
                before.protocol != after.protocol ||
                before.controlMode != after.controlMode ||
                before.mavlinkNodeId != after.mavlinkNodeId ||
                before.mavlinkDeviceId != after.mavlinkDeviceId ||
                before.enabled != after.enabled ||
                strcmp(before.name, after.name) != 0) {
                needsRestart = true;
                break;
            }
        }
    }

    config = updated;
    if (!storageSaveAppConfig(config)) {
        if (message) {
            *message = "failed to save config";
        }
        return false;
    }
    if (!loadAllDeviceVtxTables(message)) {
        if (message) {
            *message = message->length() > 0 ? *message : "failed to load device VTX tables";
        }
        return false;
    }
    sanitizeDeviceSelections();
    storageSaveAppConfig(config);
    if (!needsRestart) {
        applyConfiguredSerialSelections();
    }

    if (restartRequired) {
        *restartRequired = needsRestart;
    }
    if (message) {
        *message = needsRestart ? "configuration saved, restarting" : "configuration saved";
    }
    return true;
}

static void handleEspNowData(uint8_t* address, uint8_t* data, uint8_t len, signed int rssi, bool broadcast) {
    (void)rssi;
    (void)broadcast;

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, data, len);
    if (error) {
        return;
    }

    const String command = doc["cmd"] | "";
    if (command == "status") {
        sendCompactEspNowStatus(address);
        return;
    }
    if (command == "apply") {
        const uint8_t deviceIndex = doc["device"] | 0;
        const size_t band = doc["band"] | 0;
        const size_t channel = doc["channel"] | 0;
        const size_t power = doc["power"] | 0;
        if (band > 0 && channel > 0) {
            applyVtxSelectionToDevice(deviceIndex, band - 1, channel - 1, power);
        }
        sendCompactEspNowStatus(address);
        return;
    }
    if (command == "mavlink_apply") {
        MavlinkVtxCommand mavlinkCommand = {};
        mavlinkCommand.commandId = MAVLINK_VTX_COMMAND_ID;
        mavlinkCommand.nodeId = doc["nodeId"] | 0;
        mavlinkCommand.deviceId = doc["deviceId"] | 0;
        mavlinkCommand.band = doc["band"] | 0;
        mavlinkCommand.channel = doc["channel"] | 0;
        mavlinkCommand.powerIndex = doc["powerIndex"] | MAVLINK_VTX_KEEP_VALUE;
        mavlinkCommand.flags = doc["flags"] | 0;
        mavlinkDebug.espNowRxCount++;
        mavlinkDebug.lastCommandAtMs = millis();
        mavlinkDebug.lastCommand = mavlinkCommand;
        mavlinkDebug.hasLastCommand = true;
        const bool localApplied = applyMavlinkCommandLocally(mavlinkCommand);
        if (localApplied) {
            mavlinkDebug.localApplyCount++;
            storageSaveAppConfig(config);
        }
        logMavlinkCommand("ESPNOW", mavlinkCommand, localApplied, false);
        sendCompactEspNowStatus(address);
        return;
    }
    if (command == "set") {
        bool restartRequired = false;
        String message;
        DynamicJsonDocument response(256);
        response["type"] = "ack";
        response["ok"] = updateConfigFromJson(doc.as<JsonVariantConst>(), &restartRequired, &message);
        response["message"] = message;
        response["restart"] = restartRequired;
        sendEspNowJson(address, response);
        if (restartRequired) {
            scheduleRestart();
        }
    }
}

static void handleRoot() {
    server.send_P(200, "text/html", WEB_UI_HTML);
}

static void handleApiState() {
    DynamicJsonDocument doc(24576);
    appendStateJson(doc.to<JsonObject>());
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

static void handleApiControl() {
    const int deviceIndex = server.hasArg("device") ? server.arg("device").toInt() : -1;
    DynamicJsonDocument response(512);

    if (restartScheduled) {
        response["ok"] = false;
        response["message"] = "restart pending; wait for mode change to complete";
    } else if (deviceIndex < 0 || deviceIndex >= config.deviceCount) {
        response["ok"] = false;
        response["message"] = "invalid device index";
    } else {
        VtxDeviceConfig& device = config.devices[deviceIndex];
        if (!device.enabled) {
            response["ok"] = false;
            response["message"] = "device is disabled";
        } else if (device.controlMode != VTX_CONTROL_MODE_SERIAL) {
            response["ok"] = false;
            response["message"] = "device is not in serial control mode";
        } else {
            device.manualBand = clampManualBandValue(static_cast<uint8_t>(deviceIndex), static_cast<uint8_t>(server.arg("band").toInt()));
            device.manualChannel = clampManualChannelValue(static_cast<uint8_t>(server.arg("channel").toInt()));
            device.manualPowerIndex = clampManualPowerIndexValue(static_cast<uint8_t>(deviceIndex), static_cast<uint8_t>(server.arg("power").toInt()));
            const bool ok = applyVtxSelectionToDevice(
                static_cast<uint8_t>(deviceIndex),
                static_cast<size_t>(device.manualBand - 1),
                static_cast<size_t>(device.manualChannel - 1),
                static_cast<size_t>(device.manualPowerIndex));
            storageSaveAppConfig(config);
            response["ok"] = ok;
            response["message"] = ok ? "manual control applied" : "failed to apply manual control";
        }
    }

    String body;
    serializeJson(response, body);
    server.send(response["ok"].as<bool>() ? 200 : 400, "application/json", body);
}

static void handleApiConfig() {
    DynamicJsonDocument input(4096);
    input["wifiChannel"] = server.arg("wifiChannel").toInt();
    input["espNowEnabled"] = server.arg("espNowEnabled") == "1";
    input["boardRole"] = server.arg("boardRole");
    input["localNodeId"] = server.arg("localNodeId").toInt();
    input["mavlinkRxPin"] = server.arg("mavlinkRxPin").toInt();
    input["mavlinkTxPin"] = server.arg("mavlinkTxPin").toInt();
    input["mavlinkBaud"] = server.arg("mavlinkBaud").toInt();

    const String devicesJson = server.arg("devicesJson");
    if (devicesJson.length() > 0) {
        DynamicJsonDocument devicesDoc(3072);
        DeserializationError error = deserializeJson(devicesDoc, devicesJson);
        if (!error) {
            input["devices"] = devicesDoc.as<JsonArray>();
        }
    }

    bool restartRequired = false;
    String message;
    const bool ok = updateConfigFromJson(input.as<JsonVariantConst>(), &restartRequired, &message);
    DynamicJsonDocument response(256);
    response["ok"] = ok;
    response["message"] = message;
    response["restart"] = restartRequired;
    String body;
    serializeJson(response, body);
    server.send(ok ? 200 : 400, "application/json", body);
    if (ok && restartRequired) {
        scheduleRestart();
    }
}

static String sanitizeUploadPath(const String& inputName) {
    return normalizeTablePath(inputName);
}

static void handleApiVtxTableGet() {
    const int requestedDevice = server.hasArg("device") ? server.arg("device").toInt() : -1;
    String path = server.hasArg("path") ? normalizeTablePath(server.arg("path")) : String();
    String json;
    bool ok = false;
    String message;

    if (requestedDevice >= 0 && requestedDevice < config.deviceCount) {
        path = config.devices[requestedDevice].vtxTablePath;
        json = deviceRuntime[requestedDevice].vtxJson;
        if (json.length() == 0) {
            ok = loadDeviceVtxTable(static_cast<uint8_t>(requestedDevice), &message);
            if (ok) {
                json = deviceRuntime[requestedDevice].vtxJson;
            }
        } else {
            ok = true;
        }
    } else if (path.length() == 0 && config.deviceCount > 0) {
        path = config.devices[0].vtxTablePath;
        json = deviceRuntime[0].vtxJson;
        if (json.length() == 0) {
            ok = loadDeviceVtxTable(0, &message);
            if (ok) {
                json = deviceRuntime[0].vtxJson;
            }
        } else {
            ok = true;
        }
    } else if (storageFileExists(path.c_str())) {
        ok = storageLoadVtxTable(path.c_str());
        if (ok) {
            json = storageGetVtxJson();
        } else {
            message = "failed to load requested VTX table";
        }
    } else {
        message = "requested VTX table does not exist";
    }

    DynamicJsonDocument response(24576);
    response["ok"] = ok;
    response["path"] = path;
    response["json"] = json;
    response["device"] = requestedDevice;
    response["message"] = message;
    String body;
    serializeJson(response, body);
    server.send(200, "application/json", body);
}

static void handleApiVtxTablePost() {
    const String path = normalizeTablePath(server.arg("path"));
    const String json = server.arg("json");
    const bool selectAfterSave = server.arg("select") == "1";
    const int deviceIndex = server.hasArg("device") ? server.arg("device").toInt() : -1;
    String message;
    bool ok = false;

    if (json.length() == 0) {
        message = "missing JSON content";
    } else if (!storageSaveVtxTableJson(path.c_str(), json, &message)) {
        ok = false;
    } else {
        ok = true;
        if (selectAfterSave && deviceIndex >= 0 && deviceIndex < config.deviceCount) {
            VtxDeviceConfig& device = config.devices[deviceIndex];
            strncpy(device.vtxTablePath, path.c_str(), sizeof(device.vtxTablePath) - 1);
            device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
            storageSaveAppConfig(config);
            loadDeviceVtxTable(static_cast<uint8_t>(deviceIndex), &message);
            message = "table saved and assigned to device";
        } else {
            message = "table saved";
        }
    }

    DynamicJsonDocument response(512);
    response["ok"] = ok;
    response["message"] = message;
    response["path"] = path;
    String body;
    serializeJson(response, body);
    server.send(ok ? 200 : 400, "application/json", body);
}

static void handleUploadChunk() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadError = "";
        uploadOk = false;
        uploadPath = sanitizeUploadPath(upload.filename);
        uploadBuffer = "";
        if (upload.totalSize > 0) {
            uploadBuffer.reserve(upload.totalSize + 1);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadError.length() == 0) {
            for (size_t i = 0; i < upload.currentSize; i++) {
                uploadBuffer += static_cast<char>(upload.buf[i]);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadError.length() == 0) {
            if (uploadBuffer.length() == 0) {
                uploadError = "uploaded file is empty";
            } else if (!storageSaveVtxTableJson(uploadPath.c_str(), uploadBuffer, &uploadError)) {
                uploadOk = false;
            } else {
                uploadOk = true;
            }
        }
        uploadBuffer = "";
    }
}

static void handleUploadComplete() {
    DynamicJsonDocument response(256);
    response["ok"] = uploadOk;
    response["message"] = uploadOk ? "table uploaded" : uploadError;
    response["path"] = uploadPath;
    String body;
    serializeJson(response, body);
    server.send(uploadOk ? 200 : 400, "application/json", body);
}

static void configureWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/state", HTTP_GET, handleApiState);
    server.on("/api/config", HTTP_POST, handleApiConfig);
    server.on("/api/control", HTTP_POST, handleApiControl);
    server.on("/api/vtx-table", HTTP_GET, handleApiVtxTableGet);
    server.on("/api/vtx-table", HTTP_POST, handleApiVtxTablePost);
    server.on("/api/upload", HTTP_POST, handleUploadComplete, handleUploadChunk);
    server.begin();
}

static void initializeStorageAndConfig() {
    appConfigSetDefaults(config);
    if (!storageBegin()) {
        Serial.println("LittleFS failed to mount");
        return;
    }
    storageEnsureVtxFile(kEmbeddedDefaultTable);
    storageLoadAppConfig(&config);
    config.deviceCount = appConfigClampDeviceCount(config.deviceCount);
    String message;
    if (!loadAllDeviceVtxTables(&message)) {
        Serial.printf("Failed to load device VTX tables: %s\n", message.c_str());
    }
}

static void initializeWireless() {
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_MODE_AP);
    delay(100);

    bool apStarted = WiFi.softAP(kApSsid, kApPassword, config.wifiChannel);
    if (!apStarted) {
        delay(300);
        WiFi.mode(WIFI_OFF);
        delay(100);
        WiFi.mode(WIFI_MODE_AP);
        delay(100);
        apStarted = WiFi.softAP(kApSsid, kApPassword, config.wifiChannel);
    }

    Serial.printf("softAP start=%u ssid=%s channel=%u ip=%s\n",
                  apStarted ? 1 : 0,
                  kApSsid,
                  static_cast<unsigned>(config.wifiChannel),
                  WiFi.softAPIP().toString().c_str());

    if (!apStarted) {
        return;
    }

    if (config.espNowEnabled) {
        quickEspNow.setWiFiBandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        quickEspNow.onDataRcvd(handleEspNowData);
        quickEspNow.begin(config.wifiChannel, WIFI_IF_AP);
        Serial.println("ESP-NOW init requested on AP interface");
    }
}

static void initializeMavlinkSerial() {
    mavlinkParser.reset();
    fcMavSerial.end();
    lastMavlinkHeartbeatMs = 0;
    if (config.boardRole != BOARD_ROLE_FC_BRIDGE) {
        return;
    }
    if (hasEnabledTrampDevice(config)) {
        Serial.println("MAVLink UART disabled: FC bridge role conflicts with enabled TRAMP device on ESP32-C3");
        return;
    }
    if (!isValidGpio(static_cast<uint8_t>(config.mavlinkRxPin))) {
        Serial.println("MAVLink UART disabled: invalid RX pin");
        return;
    }
    fcMavSerial.begin(config.mavlinkBaud, SERIAL_8N1, config.mavlinkRxPin, config.mavlinkTxPin);
    Serial.printf("MAVLink UART started on UART1 rx=%d tx=%d baud=%lu sysid=%u compid=%u\n", config.mavlinkRxPin, config.mavlinkTxPin, static_cast<unsigned long>(config.mavlinkBaud), static_cast<unsigned>(config.localNodeId), static_cast<unsigned>(kMavlinkComponentId));
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("ESP32-C3 VTX controller starting");

    initializeStorageAndConfig();
    initializePwmCapture();
    initializeVtxTransport();
    sanitizeDeviceSelections();
    applyConfiguredSerialSelections();
    initializeMavlinkSerial();
    initializeWireless();
    configureWebServer();

    Serial.printf("Configured devices: %u\n", config.deviceCount);
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        Serial.printf("[%u] %s pwm=%u vtx=%u proto=%s control=%s node=%u device=%u table=%s enabled=%u\n", index, device.name, device.pwmInputPin, device.vtxControlPin, appConfigProtocolToString(device.protocol), appConfigControlModeToString(device.controlMode), device.mavlinkNodeId, device.mavlinkDeviceId, device.vtxTablePath, device.enabled ? 1 : 0);
    }
    Serial.printf("Role=%s node=%u mavlinkRx=%d mavlinkTx=%d mavlinkBaud=%lu\n", appConfigBoardRoleToString(config.boardRole), config.localNodeId, config.mavlinkRxPin, config.mavlinkTxPin, static_cast<unsigned long>(config.mavlinkBaud));
    Serial.printf("Web UI: http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
    server.handleClient();
    if (config.boardRole == BOARD_ROLE_FC_BRIDGE && millis() - lastMavlinkHeartbeatMs >= 1000) {
        lastMavlinkHeartbeatMs = millis();
        sendMavlinkHeartbeat();
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
    smartaudioProcess();
    trampProcess();
    if (config.espNowEnabled && millis() - lastEspNowBroadcastMs >= 5000 && quickEspNow.readyToSendData()) {
        lastEspNowBroadcastMs = millis();
        sendCompactEspNowStatus(ESPNOW_BROADCAST_ADDRESS);
    }
    if (restartScheduled && millis() >= restartAtMs) {
        ESP.restart();
    }
}
