#include "storage.h"
#include <LittleFS.h>
#include "embedded_vtxtable.h"

static DynamicJsonDocument* s_doc = nullptr;
static String s_vtxJson;
static const char* kConfigPath = "/config.json";

static bool storageValidateVtxTableDocument(JsonVariantConst root, String* errorMessage) {
    JsonVariantConst vtxTable = root["vtx_table"];
    if (vtxTable.isNull()) {
        if (errorMessage) {
            *errorMessage = "missing vtx_table";
        }
        return false;
    }

    JsonArrayConst bands = vtxTable["bands_list"].as<JsonArrayConst>();
    JsonArrayConst powers = vtxTable["powerlevels_list"].as<JsonArrayConst>();
    if (bands.isNull() || bands.isNull() || bands.size() == 0) {
        if (errorMessage) {
            *errorMessage = "missing bands_list";
        }
        return false;
    }
    if (powers.isNull() || powers.size() == 0) {
        if (errorMessage) {
            *errorMessage = "missing powerlevels_list";
        }
        return false;
    }
    return true;
}

static bool storageReadJsonFile(const char* path, DynamicJsonDocument& doc) {
    if (!LittleFS.exists(path)) {
        return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }

    DeserializationError err = deserializeJson(doc, file);
    file.close();
    return !err;
}

static bool storageReadTextFile(const char* path, String& text) {
    if (!LittleFS.exists(path)) {
        return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }

    text = file.readString();
    file.close();
    return true;
}

static bool storageLoadVtxTableDocument(DynamicJsonDocument& doc, const String& rawJson, String* errorMessage) {
    if (!storageValidateVtxTableDocument(doc.as<JsonVariantConst>(), errorMessage)) {
        return false;
    }

    if (s_doc) {
        delete s_doc;
        s_doc = nullptr;
    }

    s_doc = new DynamicJsonDocument(doc.capacity());
    if (!s_doc) {
        if (errorMessage) {
            *errorMessage = "out of memory";
        }
        return false;
    }

    *s_doc = doc;
    s_vtxJson = rawJson;
    return true;
}

static bool storageCloneVtxTableDocument(DynamicJsonDocument& doc, DynamicJsonDocument** outDoc, String* errorMessage) {
    if (!outDoc) {
        if (errorMessage) {
            *errorMessage = "missing output document";
        }
        return false;
    }

    DynamicJsonDocument* clone = new DynamicJsonDocument(doc.capacity());
    if (!clone) {
        if (errorMessage) {
            *errorMessage = "out of memory";
        }
        return false;
    }

    *clone = doc;
    *outDoc = clone;
    return true;
}

bool storageBegin() {
    // begin LittleFS; do not format automatically
    if (!LittleFS.begin()) {
        return false;
    }
    return true;
}

bool storageEnsureVtxFile(const char* path) {
    if (LittleFS.exists(path)) {
        String existing;
        if (storageReadTextFile(path, existing) && existing == String(EMBEDDED_VTXTABLE)) {
            return true;
        }
    }

    File f = LittleFS.open(path, "w");
    if (!f) return false;
    // EMBEDDED_VTXTABLE is a PROGMEM C-string
    f.print(EMBEDDED_VTXTABLE);
    f.close();
    return true;
}

bool storageFileExists(const char* path) {
    return LittleFS.exists(path);
}

bool storageLoadVtxTable(const char* path) {
    String rawJson;
    if (!storageReadTextFile(path, rawJson)) {
        return false;
    }
    return storageLoadVtxTableJson(rawJson, nullptr);
}

bool storageLoadVtxTableJson(const String& json, String* errorMessage) {
    DynamicJsonDocument doc(json.length() * 2 + 2048);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        if (errorMessage) {
            *errorMessage = err.c_str();
        }
        return false;
    }
    return storageLoadVtxTableDocument(doc, json, errorMessage);
}

bool storageParseVtxTableJsonTo(const String& json, DynamicJsonDocument** outDoc, String* errorMessage) {
    DynamicJsonDocument doc(json.length() * 2 + 2048);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        if (errorMessage) {
            *errorMessage = err.c_str();
        }
        return false;
    }
    if (!storageValidateVtxTableDocument(doc.as<JsonVariantConst>(), errorMessage)) {
        return false;
    }
    return storageCloneVtxTableDocument(doc, outDoc, errorMessage);
}

bool storageLoadVtxTableFileTo(const char* path, DynamicJsonDocument** outDoc, String* outJson, String* errorMessage) {
    String rawJson;
    if (!storageReadTextFile(path, rawJson)) {
        if (errorMessage) {
            *errorMessage = "failed to read VTX table file";
        }
        return false;
    }

    if (!storageParseVtxTableJsonTo(rawJson, outDoc, errorMessage)) {
        return false;
    }

    if (outJson) {
        *outJson = rawJson;
    }
    return true;
}

bool storageSaveVtxTableJson(const char* path, const String& json, String* errorMessage) {
    DynamicJsonDocument doc(json.length() * 2 + 2048);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        if (errorMessage) {
            *errorMessage = err.c_str();
        }
        return false;
    }
    if (!storageValidateVtxTableDocument(doc.as<JsonVariantConst>(), errorMessage)) {
        return false;
    }

    File file = LittleFS.open(path, "w");
    if (!file) {
        if (errorMessage) {
            *errorMessage = "failed to open destination";
        }
        return false;
    }
    const size_t written = file.print(json);
    file.close();
    if (written != json.length()) {
        if (errorMessage) {
            *errorMessage = "failed to write file";
        }
        return false;
    }

    return true;
}

bool storageValidateVtxTableFile(const char* path, String* errorMessage) {
    DynamicJsonDocument doc(16384);
    if (!storageReadJsonFile(path, doc)) {
        if (errorMessage) {
            *errorMessage = "invalid JSON";
        }
        return false;
    }
    return storageValidateVtxTableDocument(doc.as<JsonVariantConst>(), errorMessage);
}

void storageListVtxFiles(JsonArray array) {
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        return;
    }

    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (name.endsWith(".json")) {
                array.add(name);
            }
        }
        entry = root.openNextFile();
    }
}

bool storageLoadAppConfig(AppConfig* config) {
    if (!config) {
        return false;
    }

    DynamicJsonDocument doc(1024);
    if (!storageReadJsonFile(kConfigPath, doc)) {
        return false;
    }

    appConfigSetDefaults(*config);
    config->wifiChannel = doc["wifiChannel"] | config->wifiChannel;
    config->espNowEnabled = doc["espNowEnabled"] | config->espNowEnabled;
    const char* legacyPath = doc["selectedVtxPath"] | DEFAULT_VTX_TABLE_PATH;

    JsonArray devices = doc["devices"].as<JsonArray>();
    if (!devices.isNull() && devices.size() > 0) {
        config->deviceCount = appConfigClampDeviceCount(devices.size());
        for (uint8_t index = 0; index < config->deviceCount; index++) {
            JsonObject deviceObject = devices[index].as<JsonObject>();
            VtxDeviceConfig& device = config->devices[index];
            const char* name = deviceObject["name"] | device.name;
            strncpy(device.name, name, sizeof(device.name) - 1);
            device.name[sizeof(device.name) - 1] = '\0';
            device.pwmInputPin = deviceObject["pwmInputPin"] | device.pwmInputPin;
            device.vtxControlPin = deviceObject["vtxControlPin"] | device.vtxControlPin;
            device.protocol = deviceObject["protocol"] | device.protocol;
            device.enabled = deviceObject["enabled"] | device.enabled;
            const char* vtxTablePath = deviceObject["vtxTablePath"] | legacyPath;
            strncpy(device.vtxTablePath, vtxTablePath, sizeof(device.vtxTablePath) - 1);
            device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
        }
    } else {
        config->deviceCount = 1;
        VtxDeviceConfig& device = config->devices[0];
        device.pwmInputPin = doc["pwmInputPin"] | device.pwmInputPin;
        device.vtxControlPin = doc["vtxUartPin"] | device.vtxControlPin;
        device.protocol = doc["protocol"] | device.protocol;
        device.enabled = true;
        strncpy(device.vtxTablePath, legacyPath, sizeof(device.vtxTablePath) - 1);
        device.vtxTablePath[sizeof(device.vtxTablePath) - 1] = '\0';
    }
    return true;
}

bool storageSaveAppConfig(const AppConfig& config) {
    DynamicJsonDocument doc(1024);
    doc["wifiChannel"] = config.wifiChannel;
    doc["espNowEnabled"] = config.espNowEnabled;
    JsonArray devices = doc.createNestedArray("devices");
    const uint8_t deviceCount = appConfigClampDeviceCount(config.deviceCount);
    for (uint8_t index = 0; index < deviceCount; index++) {
        const VtxDeviceConfig& device = config.devices[index];
        JsonObject deviceObject = devices.createNestedObject();
        deviceObject["name"] = device.name;
        deviceObject["pwmInputPin"] = device.pwmInputPin;
        deviceObject["vtxControlPin"] = device.vtxControlPin;
        deviceObject["protocol"] = device.protocol;
        deviceObject["enabled"] = device.enabled;
        deviceObject["vtxTablePath"] = device.vtxTablePath;
    }

    File file = LittleFS.open(kConfigPath, "w");
    if (!file) {
        return false;
    }

    const bool ok = serializeJson(doc, file) > 0;
    file.close();
    return ok;
}

bool storageDeleteFile(const char* path) {
    if (!LittleFS.exists(path)) {
        return true;
    }
    return LittleFS.remove(path);
}

DynamicJsonDocument* storageGetVtxDoc() {
    return s_doc;
}

const String& storageGetVtxJson() {
    return s_vtxJson;
}

void storageFree() {
    if (s_doc) {
        delete s_doc;
        s_doc = nullptr;
    }
    s_vtxJson = "";
    LittleFS.end();
}
