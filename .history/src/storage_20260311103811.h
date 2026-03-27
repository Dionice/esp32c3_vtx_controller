#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "app_config.h"

// Initialize filesystem. Returns true on success.
bool storageBegin();

// Ensure the VTX file exists at `path` on the filesystem; if missing, write embedded copy.
bool storageEnsureVtxFile(const char* path);

// Returns true when a filesystem entry exists.
bool storageFileExists(const char* path);

// Load the VTX JSON into a dynamically allocated DynamicJsonDocument on the heap.
// Returns true on success. Call storageFree() when finished or before re-loading.
bool storageLoadVtxTable(const char* path);

// Load a VTX JSON string into memory without requiring it to come from disk.
bool storageLoadVtxTableJson(const String& json, String* errorMessage = nullptr);

// Load a VTX JSON file into a caller-owned document and optional raw JSON string.
bool storageLoadVtxTableFileTo(const char* path, DynamicJsonDocument** outDoc, String* outJson = nullptr, String* errorMessage = nullptr);

// Parse a VTX JSON string into a caller-owned document.
bool storageParseVtxTableJsonTo(const String& json, DynamicJsonDocument** outDoc, String* errorMessage = nullptr);

// Save a VTX JSON string to disk and validate it.
bool storageSaveVtxTableJson(const char* path, const String& json, String* errorMessage = nullptr);

// Validate a VTX JSON file on disk.
bool storageValidateVtxTableFile(const char* path, String* errorMessage = nullptr);

// Populate a JsonArray with available VTX JSON files stored on LittleFS.
void storageListVtxFiles(JsonArray array);

// Persist application configuration.
bool storageLoadAppConfig(AppConfig* config);
bool storageSaveAppConfig(const AppConfig& config);

// Remove a file from LittleFS.
bool storageDeleteFile(const char* path);

// Get pointer to the loaded DynamicJsonDocument (or nullptr if not loaded).
DynamicJsonDocument* storageGetVtxDoc();

// Get the raw loaded VTX JSON string.
const String& storageGetVtxJson();

// Free resources and unmount filesystem.
void storageFree();

#endif // STORAGE_H
