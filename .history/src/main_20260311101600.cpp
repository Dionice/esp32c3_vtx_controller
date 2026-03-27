#include <Arduino.h>
#include <LittleFS.h>
#include <QuickEspNow.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include "app_config.h"
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
static const char* kEmbeddedDefaultTable = "/peak_thor_t35.json";

struct DeviceRuntimeState {
    volatile uint32_t riseMicros;
    volatile uint16_t capturedPulse;
    volatile bool pulseAvailable;
    uint16_t filteredPulse;
    bool filterInitialized;
    int32_t lastMappedIndex;
    int32_t candidateMappedIndex;
    uint8_t candidateCount;
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
    }
}

static size_t getBandCount() {
    DynamicJsonDocument* doc = storageGetVtxDoc();
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

static size_t getPowerCount() {
    DynamicJsonDocument* doc = storageGetVtxDoc();
    if (doc) {
        JsonArray levels = (*doc)["vtx_table"]["powerlevels_list"].as<JsonArray>();
        if (!levels.isNull() && levels.size() > 0) {
            return levels.size();
        }
    }
    return sizeof(kDefaultPowerValues) / sizeof(kDefaultPowerValues[0]);
}

static uint16_t getFrequencyForSelection(size_t bandIndex, size_t channelIndex) {
    DynamicJsonDocument* doc = storageGetVtxDoc();
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

static uint8_t getPowerValueForIndex(size_t powerIndex) {
    DynamicJsonDocument* doc = storageGetVtxDoc();
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

static bool loadSelectedVtxTable() {
    if (!storageFileExists(config.selectedVtxPath)) {
        strncpy(config.selectedVtxPath, kEmbeddedDefaultTable, sizeof(config.selectedVtxPath) - 1);
        config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
    }
    return storageLoadVtxTable(config.selectedVtxPath);
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
        for (uint8_t right = left + 1; right < deviceCount; right++) {
            const VtxDeviceConfig& other = candidate.devices[right];
            if (!other.enabled) {
                continue;
            }
            if (device.pwmInputPin == other.pwmInputPin) {
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
        if (!device.enabled) {
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

static bool applyVtxSelectionToDevice(uint8_t deviceIndex, size_t bandIndex, size_t channelIndex, size_t powerIndex, bool pitMode) {
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
    const uint8_t powerValue = getPowerValueForIndex(powerIndex);
    const uint16_t frequency = getFrequencyForSelection(bandIndex, channelIndex);

    if (device.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        smartaudioSendBandChannel(band, channel);
        delay(20);
        smartaudioSendPower(powerValue);
        if (pitMode && frequency > 0) {
            delay(20);
            smartaudioSendFrequency(frequency, true);
        }
    } else {
        if (frequency > 0) {
            trampSendFrequency(frequency);
            delay(20);
        }
        trampSendPower(powerValue);
        delay(20);
        trampSendActiveState(!pitMode);
    }
    return true;
}

static void processDevicePulse(uint8_t deviceIndex, uint16_t pulse) {
    DeviceRuntimeState& state = deviceRuntime[deviceIndex];
    pulse = constrain(pulse, 500U, 2500U);
    if (!state.filterInitialized) {
        state.filteredPulse = pulse;
        state.filterInitialized = true;
    } else {
        state.filteredPulse = static_cast<uint16_t>((state.filteredPulse * 3U + pulse) / 4U);
    }

    const uint32_t bandCount = getBandCount();
    const uint32_t channelCount = getChannelCount();
    const uint32_t powerCount = getPowerCount();
    const uint32_t total = bandCount * channelCount * powerCount * 2U;
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
    const uint32_t pitIdx = rem % 2U; rem /= 2U;
    const uint32_t powIdx = rem % powerCount; rem /= powerCount;
    const uint32_t chanIdx = rem % channelCount; rem /= channelCount;
    const uint32_t bandIdx = rem % bandCount;
    applyVtxSelectionToDevice(deviceIndex, bandIdx, chanIdx, powIdx, pitIdx != 0);
}

static void scheduleRestart() {
    restartScheduled = true;
    restartAtMs = millis() + 1200;
}

static void appendStateJson(JsonObject root) {
    root["wifiChannel"] = config.wifiChannel;
    root["espNowEnabled"] = config.espNowEnabled;
    root["selectedVtxPath"] = config.selectedVtxPath;
    root["transportReady"] = transportReady;
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
        deviceObject["enabled"] = device.enabled;
        deviceObject["filteredPulse"] = state.filteredPulse;
        deviceObject["lastMappedIndex"] = state.lastMappedIndex;
    }

    JsonArray tables = root.createNestedArray("tables");
    storageListVtxFiles(tables);
    if (!jsonArrayContainsString(tables, config.selectedVtxPath)) {
        tables.add(config.selectedVtxPath);
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
    doc["table"] = baseNameFromPath(config.selectedVtxPath);
    JsonArray items = doc.createNestedArray("items");
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        JsonObject item = items.createNestedObject();
        item["name"] = config.devices[index].name;
        item["protocol"] = appConfigProtocolToString(config.devices[index].protocol);
        item["pulse"] = deviceRuntime[index].filteredPulse;
        item["slot"] = deviceRuntime[index].lastMappedIndex;
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
    if (!input["selectedVtxPath"].isNull()) {
        String path = input["selectedVtxPath"].as<String>();
        if (!path.startsWith("/")) {
            path = "/" + path;
        }
        if (!storageFileExists(path.c_str())) {
            if (message) {
                *message = "selected VTX table does not exist";
            }
            return false;
        }
        strncpy(updated.selectedVtxPath, path.c_str(), sizeof(updated.selectedVtxPath) - 1);
        updated.selectedVtxPath[sizeof(updated.selectedVtxPath) - 1] = '\0';
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
            device.enabled = deviceObject["enabled"].isNull() ? true : deviceObject["enabled"].as<bool>();
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
    if (!loadSelectedVtxTable()) {
        if (message) {
            *message = "failed to load VTX table";
        }
        return false;
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
        const bool pit = doc["pit"] | false;
        if (band > 0 && channel > 0) {
            applyVtxSelectionToDevice(deviceIndex, band - 1, channel - 1, power, pit);
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
    server.send_P(200, "text/html", WEB_UI_HTML);
}

static void handleApiState() {
    DynamicJsonDocument doc(4096);
    appendStateJson(doc.to<JsonObject>());
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

static void handleApiConfig() {
    DynamicJsonDocument input(4096);
    input["wifiChannel"] = server.arg("wifiChannel").toInt();
    input["espNowEnabled"] = server.arg("espNowEnabled") == "1";
    input["selectedVtxPath"] = server.arg("selectedVtxPath");

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
    String path = server.hasArg("path") ? normalizeTablePath(server.arg("path")) : String(config.selectedVtxPath);
    String json;
    bool ok = false;
    String message;

    if (path == config.selectedVtxPath || server.arg("path").length() == 0) {
        json = storageGetVtxJson();
        path = config.selectedVtxPath;
        if (json.length() == 0) {
            ok = loadSelectedVtxTable();
            if (ok) {
                json = storageGetVtxJson();
            } else {
                message = "failed to load selected VTX table";
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
        loadSelectedVtxTable();
    } else {
        message = "requested VTX table does not exist";
    }

    DynamicJsonDocument response(24576);
    response["ok"] = ok;
    response["path"] = path;
    response["json"] = json;
    response["selected"] = config.selectedVtxPath;
    response["message"] = message;
    String body;
    serializeJson(response, body);
    server.send(200, "application/json", body);
}

static void handleApiVtxTablePost() {
    const String path = normalizeTablePath(server.arg("path"));
    const String json = server.arg("json");
    const bool selectAfterSave = server.arg("select") == "1";
    String message;
    bool ok = false;

    if (json.length() == 0) {
        message = "missing JSON content";
    } else if (!storageSaveVtxTableJson(path.c_str(), json, &message)) {
        ok = false;
    } else {
        ok = true;
        if (selectAfterSave) {
            strncpy(config.selectedVtxPath, path.c_str(), sizeof(config.selectedVtxPath) - 1);
            config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
            storageSaveAppConfig(config);
            loadSelectedVtxTable();
            message = "table saved and selected";
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
                strncpy(config.selectedVtxPath, uploadPath.c_str(), sizeof(config.selectedVtxPath) - 1);
                config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
                storageSaveAppConfig(config);
                loadSelectedVtxTable();
                uploadOk = true;
            }
        }
        uploadBuffer = "";
    }
}

static void handleUploadComplete() {
    DynamicJsonDocument response(256);
    response["ok"] = uploadOk;
    response["message"] = uploadOk ? "table uploaded and selected" : uploadError;
    response["path"] = uploadPath;
    String body;
    serializeJson(response, body);
    server.send(uploadOk ? 200 : 400, "application/json", body);
}

static void configureWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/state", HTTP_GET, handleApiState);
    server.on("/api/config", HTTP_POST, handleApiConfig);
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
    if (!storageFileExists(config.selectedVtxPath)) {
        strncpy(config.selectedVtxPath, kEmbeddedDefaultTable, sizeof(config.selectedVtxPath) - 1);
        config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
        storageSaveAppConfig(config);
    }
    loadSelectedVtxTable();
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
    initializeWireless();
    configureWebServer();

    Serial.printf("Configured devices: %u\n", config.deviceCount);
    for (uint8_t index = 0; index < config.deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        Serial.printf("[%u] %s pwm=%u vtx=%u proto=%s enabled=%u\n", index, device.name, device.pwmInputPin, device.vtxControlPin, appConfigProtocolToString(device.protocol), device.enabled ? 1 : 0);
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
