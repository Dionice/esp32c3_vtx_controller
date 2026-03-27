#include <Arduino.h>
#include <LittleFS.h>
#include <QuickEspNow.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include "app_config.h"
#include "embedded_vtxtable.h"
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

static WebServer server(80);
static AppConfig config;
static DeviceRuntimeState deviceRuntime[MAX_VTX_DEVICES];
static bool transportReady = false;
static bool restartScheduled = false;
static unsigned long restartAtMs = 0;
static unsigned long lastEspNowBroadcastMs = 0;
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
    return pin <= 21;
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
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        VtxDeviceConfig& device = config.devices[index];
        device.manualBand = clampManualBandValue(index, device.manualBand);
        device.manualChannel = clampManualChannelValue(device.manualChannel);
        device.manualPowerIndex = clampManualPowerIndexValue(index, device.manualPowerIndex);
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
    for (uint8_t left = 0; left < deviceCount; left++) {
        const VtxDeviceConfig& device = candidate.devices[left];
        if (!device.enabled) {
            continue;
        }
        anyEnabled = true;
        if (!isValidGpio(device.pwmInputPin) || !isValidGpio(device.vtxControlPin)) {
            if (message) {
                *message = "device pins must be valid GPIO numbers";
            }
            return false;
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

static void appendStateJson(JsonObject root) {
    root["wifiChannel"] = config.wifiChannel;
    root["espNowEnabled"] = config.espNowEnabled;
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
    doc["devices"] = config.deviceCount;
    JsonArray items = doc.createNestedArray("items");
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        JsonObject item = items.createNestedObject();
        item["name"] = config.devices[index].name;
        item["protocol"] = appConfigProtocolToString(config.devices[index].protocol);
        item["controlMode"] = appConfigControlModeToString(config.devices[index].controlMode);
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
            if (!deviceObject["vtxTablePath"].isNull()) {
                String tablePath = normalizeTablePath(deviceObject["vtxTablePath"].as<String>());
                strncpy(device.vtxTablePath, tablePath.c_str(), sizeof(device.vtxTablePath) - 1);
                device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
            }
        }
    }

    if (!validateDeviceConfigSet(updated, message)) {
        return false;
    }

    bool needsRestart =
        updated.wifiChannel != config.wifiChannel ||
        updated.espNowEnabled != config.espNowEnabled ||
        updated.deviceCount != config.deviceCount;

    if (!needsRestart) {
        for (uint8_t index = 0; index < updated.deviceCount; index++) {
            const VtxDeviceConfig& before = config.devices[index];
            const VtxDeviceConfig& after = updated.devices[index];
            if (before.pwmInputPin != after.pwmInputPin ||
                before.vtxControlPin != after.vtxControlPin ||
                before.protocol != after.protocol ||
                before.controlMode != after.controlMode ||
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
    if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        if (f) {
            server.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
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
    DynamicJsonDocument response(512);
    response["ok"] = ok;
    response["message"] = message;
    response["restartRequired"] = restartRequired;
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
    WiFi.mode(WIFI_MODE_AP);
    WiFi.softAP(kApSsid, kApPassword, config.wifiChannel);
    if (config.espNowEnabled) {
        quickEspNow.setWiFiBandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        quickEspNow.onDataRcvd(handleEspNowData);
        quickEspNow.begin(CURRENT_WIFI_CHANNEL, WIFI_IF_AP);
    }
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
    initializeWireless();
    configureWebServer();

    Serial.printf("Configured devices: %u\n", config.deviceCount);
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        Serial.printf("[%u] %s pwm=%u vtx=%u proto=%s control=%s table=%s enabled=%u\n", index, device.name, device.pwmInputPin, device.vtxControlPin, appConfigProtocolToString(device.protocol), appConfigControlModeToString(device.controlMode), device.vtxTablePath, device.enabled ? 1 : 0);
    }
    Serial.printf("Web UI: http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
    server.handleClient();
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
