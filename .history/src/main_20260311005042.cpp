#include <Arduino.h>
#include <LittleFS.h>
#include <QuickEspNow.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp32-hal-rmt.h"
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
static const char* kApPassword = "configureme";
static const char* kEmbeddedDefaultTable = "/peak_thor_t35.json";

static WebServer server(80);
static AppConfig config;
static volatile uint16_t capturedPulse = 0;
static volatile bool pulseAvailable = false;
static int32_t lastMappedIndex = -1;
static int32_t candidateMappedIndex = -1;
static uint8_t candidateCount = 0;
static uint16_t filteredPulse = 1500;
static bool filterInitialized = false;
static bool transportReady = false;
static bool restartScheduled = false;
static unsigned long restartAtMs = 0;
static unsigned long lastEspNowBroadcastMs = 0;
static rmt_obj_t* pwmRmt = nullptr;
static File uploadFile;
static String uploadPath;
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

static void pwmRmtCallback(uint32_t* data, size_t len, void* arg) {
    (void)arg;
    if (!data || len == 0) {
        return;
    }

    rmt_data_t* items = reinterpret_cast<rmt_data_t*>(data);
    for (size_t i = 0; i < len; i++) {
        const rmt_data_t item = items[i];
        if (item.level0 == 1 && item.duration0 > 400 && item.duration0 < 3000) {
            capturedPulse = static_cast<uint16_t>(item.duration0);
            pulseAvailable = true;
        }
        if (item.level1 == 1 && item.duration1 > 400 && item.duration1 < 3000) {
            capturedPulse = static_cast<uint16_t>(item.duration1);
            pulseAvailable = true;
        }
    }
}

static bool loadSelectedVtxTable() {
    if (!storageFileExists(config.selectedVtxPath)) {
        strncpy(config.selectedVtxPath, kEmbeddedDefaultTable, sizeof(config.selectedVtxPath) - 1);
        config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
    }
    return storageLoadVtxTable(config.selectedVtxPath);
}

static void scheduleRestart() {
    restartScheduled = true;
    restartAtMs = millis() + 1200;
}

static bool applyVtxSelection(size_t bandIndex, size_t channelIndex, size_t powerIndex, bool pitMode) {
    const uint8_t band = static_cast<uint8_t>(bandIndex + 1);
    const uint8_t channel = static_cast<uint8_t>(channelIndex + 1);
    const uint8_t powerValue = getPowerValueForIndex(powerIndex);
    const uint16_t frequency = getFrequencyForSelection(bandIndex, channelIndex);

    if (config.protocol == VTX_PROTOCOL_SMARTAUDIO) {
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

static void decodeAndSend(uint16_t pulse) {
    pulse = constrain(pulse, 500U, 2500U);

    const uint32_t nBands = getBandCount();
    const uint32_t nChans = getChannelCount();
    const uint32_t nPows = getPowerCount();
    const uint32_t nPit = 2;
    const uint32_t total = nBands * nChans * nPows * nPit;
    if (total == 0) {
        return;
    }

    const uint32_t idx = map(pulse, 500, 2500, 0, total - 1);
    uint32_t rem = idx;
    const uint32_t pitIdx = rem % nPit; rem /= nPit;
    const uint32_t powIdx = rem % nPows; rem /= nPows;
    const uint32_t chanIdx = rem % nChans; rem /= nChans;
    const uint32_t bandIdx = rem % nBands;
    applyVtxSelection(bandIdx, chanIdx, powIdx, pitIdx != 0);
}

static void initializePwmCapture() {
    pinMode(config.pwmInputPin, INPUT);
    pwmRmt = rmtInit(config.pwmInputPin, RMT_RX_MODE, RMT_MEM_64);
    if (pwmRmt) {
        rmtSetTick(pwmRmt, 1000.0f);
        rmtSetFilter(pwmRmt, true, 100);
        rmtSetRxThreshold(pwmRmt, 5000);
        rmtRead(pwmRmt, pwmRmtCallback, nullptr);
    }
}

static void initializeVtxTransport() {
    Serial1.end();
    smartaudioSetOneWirePin(-1);
    smartaudioSetPrependZero(true);
    smartaudioEnableDebug(config.protocol == VTX_PROTOCOL_SMARTAUDIO);
    trampEnableDebug(config.protocol == VTX_PROTOCOL_TRAMP);

    if (config.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        smartaudioSetOneWirePin(config.vtxUartPin);
    } else {
        Serial1.begin(9600, SERIAL_8N1, config.vtxUartPin, config.vtxUartPin);
    }
    transportReady = true;
}

static void appendStateJson(JsonObject root) {
    root["protocol"] = appConfigProtocolToString(config.protocol);
    root["pwmInputPin"] = config.pwmInputPin;
    root["vtxUartPin"] = config.vtxUartPin;
    root["wifiChannel"] = config.wifiChannel;
    root["espNowEnabled"] = config.espNowEnabled;
    root["selectedVtxPath"] = config.selectedVtxPath;
    root["filteredPulse"] = filteredPulse;
    root["lastMappedIndex"] = lastMappedIndex;
    root["transportReady"] = transportReady;
    root["ssid"] = WiFi.softAPSSID();
    root["ip"] = WiFi.softAPIP().toString();
    JsonArray tables = root.createNestedArray("tables");
    storageListVtxFiles(tables);
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
    DynamicJsonDocument doc(256);
    doc["type"] = "status";
    doc["protocol"] = appConfigProtocolToString(config.protocol);
    doc["pwm"] = config.pwmInputPin;
    doc["vtx"] = config.vtxUartPin;
    doc["table"] = baseNameFromPath(config.selectedVtxPath);
    doc["pulse"] = filteredPulse;
    doc["slot"] = lastMappedIndex;
    sendEspNowJson(address, doc);
}

static bool updateConfigFromJson(JsonVariantConst input, bool* restartRequired, String* message) {
    AppConfig updated = config;

    if (!input["pwmInputPin"].isNull()) {
        updated.pwmInputPin = input["pwmInputPin"].as<uint8_t>();
    }
    if (!input["vtxUartPin"].isNull()) {
        updated.vtxUartPin = input["vtxUartPin"].as<uint8_t>();
    }
    if (!input["wifiChannel"].isNull()) {
        updated.wifiChannel = constrain(input["wifiChannel"].as<int>(), 1, 13);
    }
    if (!input["espNowEnabled"].isNull()) {
        updated.espNowEnabled = input["espNowEnabled"].as<bool>();
    }
    if (!input["protocol"].isNull()) {
        String protocolName = input["protocol"].as<String>();
        uint8_t protocol;
        if (!appConfigProtocolFromString(protocolName, protocol)) {
            if (message) {
                *message = "invalid protocol";
            }
            return false;
        }
        updated.protocol = protocol;
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

    const bool needsRestart =
        updated.pwmInputPin != config.pwmInputPin ||
        updated.vtxUartPin != config.vtxUartPin ||
        updated.protocol != config.protocol ||
        updated.wifiChannel != config.wifiChannel ||
        updated.espNowEnabled != config.espNowEnabled;

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

    DynamicJsonDocument doc(512);
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
        const size_t band = doc["band"] | 0;
        const size_t channel = doc["channel"] | 0;
        const size_t power = doc["power"] | 0;
        const bool pit = doc["pit"] | false;
        if (band > 0 && channel > 0) {
            applyVtxSelection(band - 1, channel - 1, power, pit);
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
    DynamicJsonDocument doc(1024);
    appendStateJson(doc.to<JsonObject>());
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

static void handleApiConfig() {
    DynamicJsonDocument input(512);
    input["pwmInputPin"] = server.arg("pwmInputPin").toInt();
    input["vtxUartPin"] = server.arg("vtxUartPin").toInt();
    input["protocol"] = server.arg("protocol");
    input["wifiChannel"] = server.arg("wifiChannel").toInt();
    input["espNowEnabled"] = server.arg("espNowEnabled") == "1";
    input["selectedVtxPath"] = server.arg("selectedVtxPath");

    bool restartRequired = false;
    String message;
    const bool ok = updateConfigFromJson(input.as<JsonVariantConst>(), &restartRequired, &message);
    DynamicJsonDocument response(256);
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

    if (path == config.selectedVtxPath || server.arg("path").length() == 0) {
        json = storageGetVtxJson();
        ok = json.length() > 0;
        path = config.selectedVtxPath;
    } else if (storageFileExists(path.c_str())) {
        ok = storageLoadVtxTable(path.c_str());
        if (ok) {
            json = storageGetVtxJson();
        }
        loadSelectedVtxTable();
    }

    DynamicJsonDocument response(20480);
    response["ok"] = ok;
    response["path"] = path;
    response["json"] = json;
    response["selected"] = config.selectedVtxPath;
    String body;
    serializeJson(response, body);
    server.send(ok ? 200 : 404, "application/json", body);
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
        uploadFile = LittleFS.open(uploadPath, "w");
        if (!uploadFile) {
            uploadError = "failed to open upload destination";
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            if (uploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
                uploadError = "failed while writing upload";
                uploadFile.close();
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
        }
        if (uploadError.length() == 0) {
            String validationError;
            if (!storageValidateVtxTableFile(uploadPath.c_str(), &validationError)) {
                uploadError = validationError;
                storageDeleteFile(uploadPath.c_str());
            } else {
                strncpy(config.selectedVtxPath, uploadPath.c_str(), sizeof(config.selectedVtxPath) - 1);
                config.selectedVtxPath[sizeof(config.selectedVtxPath) - 1] = '\0';
                storageSaveAppConfig(config);
                loadSelectedVtxTable();
                uploadOk = true;
            }
        }
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

    Serial.printf("PWM input pin: %u\n", config.pwmInputPin);
    Serial.printf("VTX pin: %u\n", config.vtxUartPin);
    Serial.printf("Protocol: %s\n", appConfigProtocolToString(config.protocol));
    Serial.printf("Web UI: http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
    server.handleClient();

    if (pulseAvailable) {
        noInterrupts();
        uint16_t pulse = capturedPulse;
        pulseAvailable = false;
        interrupts();

        pulse = constrain(pulse, 500U, 2500U);
        if (!filterInitialized) {
            filteredPulse = pulse;
            filterInitialized = true;
        } else {
            filteredPulse = static_cast<uint16_t>((filteredPulse * 3U + pulse) / 4U);
        }

        const uint32_t total = getBandCount() * getChannelCount() * getPowerCount() * 2U;
        if (total > 0) {
            const int32_t idx = map(filteredPulse, 500, 2500, 0, total - 1);
            if (idx == lastMappedIndex) {
                candidateMappedIndex = -1;
                candidateCount = 0;
            } else {
                if (idx != candidateMappedIndex) {
                    candidateMappedIndex = idx;
                    candidateCount = 1;
                } else if (candidateCount < 255) {
                    candidateCount++;
                }

                if (candidateCount >= 3) {
                    lastMappedIndex = idx;
                    candidateMappedIndex = -1;
                    candidateCount = 0;
                    decodeAndSend(filteredPulse);
                }
            }
        }
    }

    if (config.protocol == VTX_PROTOCOL_SMARTAUDIO) {
        smartaudioProcess();
    } else {
        trampProcess();
    }

    if (config.espNowEnabled && millis() - lastEspNowBroadcastMs >= 5000 && quickEspNow.readyToSendData()) {
        lastEspNowBroadcastMs = millis();
        sendCompactEspNowStatus(ESPNOW_BROADCAST_ADDRESS);
    }

    if (restartScheduled && millis() >= restartAtMs) {
        ESP.restart();
    }
}
