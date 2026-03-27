#include "storage.h"
#include <LittleFS.h>
#include "embedded_vtxtable.h"

static DynamicJsonDocument* s_doc = nullptr;
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

bool storageBegin() {
    // begin LittleFS; do not format automatically
    if (!LittleFS.begin()) {
        return false;
    }
    return true;
}

bool storageEnsureVtxFile(const char* path) {
    if (LittleFS.exists(path)) return true;

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
    if (s_doc) {
        delete s_doc;
        s_doc = nullptr;
    }

    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t len = f.size();
    // allocate a bit more than file size to be safe
    size_t cap = len * 2 + 2048;
    s_doc = new DynamicJsonDocument(cap);
    DeserializationError err = deserializeJson(*s_doc, f);
    f.close();
    if (err) {
        delete s_doc;
        s_doc = nullptr;
        return false;
    }
    if (!storageValidateVtxTableDocument(*s_doc, nullptr)) {
        delete s_doc;
        s_doc = nullptr;
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
    config->pwmInputPin = doc["pwmInputPin"] | config->pwmInputPin;
    config->vtxUartPin = doc["vtxUartPin"] | config->vtxUartPin;
    config->protocol = doc["protocol"] | config->protocol;
    config->wifiChannel = doc["wifiChannel"] | config->wifiChannel;
    config->espNowEnabled = doc["espNowEnabled"] | config->espNowEnabled;
    const char* path = doc["selectedVtxPath"] | config->selectedVtxPath;
    strncpy(config->selectedVtxPath, path, sizeof(config->selectedVtxPath) - 1);
    config->selectedVtxPath[sizeof(config->selectedVtxPath) - 1] = '\0';
    return true;
}

bool storageSaveAppConfig(const AppConfig& config) {
    DynamicJsonDocument doc(1024);
    doc["pwmInputPin"] = config.pwmInputPin;
    doc["vtxUartPin"] = config.vtxUartPin;
    doc["protocol"] = config.protocol;
    doc["wifiChannel"] = config.wifiChannel;
    doc["espNowEnabled"] = config.espNowEnabled;
    doc["selectedVtxPath"] = config.selectedVtxPath;

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

void storageFree() {
    if (s_doc) {
        delete s_doc;
        s_doc = nullptr;
    }
    LittleFS.end();
}
