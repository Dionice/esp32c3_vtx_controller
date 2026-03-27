#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Initialize filesystem. Returns true on success.
bool storageBegin();

// Ensure the VTX file exists at `path` on the filesystem; if missing, write embedded copy.
bool storageEnsureVtxFile(const char* path);

// Load the VTX JSON into a dynamically allocated DynamicJsonDocument on the heap.
// Returns true on success. Call storageFree() when finished or before re-loading.
bool storageLoadVtxTable(const char* path);

// Get pointer to the loaded DynamicJsonDocument (or nullptr if not loaded).
DynamicJsonDocument* storageGetVtxDoc();

// Free resources and unmount filesystem.
void storageFree();

#endif // STORAGE_H
