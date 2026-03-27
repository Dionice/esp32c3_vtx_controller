#include "storage.h"
#include <LittleFS.h>
#include "embedded_vtxtable.h"

static DynamicJsonDocument* s_doc = nullptr;

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
    return true;
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
