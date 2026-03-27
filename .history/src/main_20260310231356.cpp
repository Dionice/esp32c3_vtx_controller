#include <Arduino.h>
#include "esp32-hal-rmt.h"
#include "smartaudio.h"
#include "tramp.h"
#include "storage.h"

// Protocol selection: set to 0 for SmartAudio, 1 for TRAMP
enum vtx_protocol_t { PROTO_SMARTAUDIO = 0, PROTO_TRAMP = 1 };
static const vtx_protocol_t selected_protocol = PROTO_SMARTAUDIO;

// Pin configuration
#define PWM_INPUT_PIN 2
#define VTX_UART_PIN 4 // SmartAudio is normally a single-wire bidirectional connection

// Parameter lists (example sets — adjust to your VTX supported values)
const uint8_t bands[] = {1,2,3,4}; // placeholder band ids
const uint8_t channels[] = {1,2,3,4,5,6,7,8};
const uint8_t powers[] = {0,1,2,3}; // power level indices
const uint8_t pitModes[] = {0,1}; // off, on

volatile uint16_t capturedPulse = 0;
volatile bool pulseAvailable = false;
static int32_t lastMappedIndex = -1;
static uint16_t filteredPulse = 1500;
static bool filterInitialized = false;
static int32_t candidateMappedIndex = -1;
static uint8_t candidateCount = 0;
static rmt_obj_t* pwmRmt = nullptr;

static void pwmRmtCallback(uint32_t* data, size_t len, void* arg) {
    (void)arg;
    if (!data || len == 0) {
        return;
    }

    rmt_data_t* items = reinterpret_cast<rmt_data_t*>(data);
    for (size_t i = 0; i < len; i++) {
        const rmt_data_t item = items[i];

        if (item.level0 == 1 && item.duration0 > 400 && item.duration0 < 3000) {
            capturedPulse = (uint16_t)item.duration0;
            pulseAvailable = true;
        }
        if (item.level1 == 1 && item.duration1 > 400 && item.duration1 < 3000) {
            capturedPulse = (uint16_t)item.duration1;
            pulseAvailable = true;
        }
    }
}

void decodeAndSend(uint16_t pulse) {
    if (pulse < 500) pulse = 500;
    if (pulse > 2500) pulse = 2500;

    const uint32_t nBands = sizeof(bands)/sizeof(bands[0]);
    const uint32_t nChans = sizeof(channels)/sizeof(channels[0]);
    const uint32_t nPows = sizeof(powers)/sizeof(powers[0]);
    const uint32_t nPit = sizeof(pitModes)/sizeof(pitModes[0]);

    uint32_t total = nBands * nChans * nPows * nPit;

    uint32_t idx = map(pulse, 500, 2500, 0, total - 1);

    uint32_t rem = idx;
    uint32_t pitIdx = rem % nPit; rem /= nPit;
    uint32_t powIdx = rem % nPows; rem /= nPows;
    uint32_t chanIdx = rem % nChans; rem /= nChans;
    uint32_t bandIdx = rem % nBands;

    uint8_t band = bands[bandIdx];
    uint8_t chan = channels[chanIdx];
    uint8_t power = powers[powIdx];
    uint8_t pit = pitModes[pitIdx];

    DynamicJsonDocument* doc = storageGetVtxDoc();
    if (doc) {
        JsonArray jsonPowerLevels = (*doc)["vtx_table"]["powerlevels_list"].as<JsonArray>();
        if (powIdx < jsonPowerLevels.size()) {
            power = jsonPowerLevels[powIdx]["value"] | 0;
        }
    }

    // Use SmartAudio to set band/channel and power. For frequency-based VTXs, call frequency setter.
    smartaudioSendBandChannel(band, chan);
    delay(20);
    smartaudioSendPower(power);
    smartaudioSendPitMode(pit != 0);
}

void setup() {
    pinMode(PWM_INPUT_PIN, INPUT);

    pwmRmt = rmtInit(PWM_INPUT_PIN, RMT_RX_MODE, RMT_MEM_64);
    if (pwmRmt) {
        rmtSetTick(pwmRmt, 1000.0f); // 1 us resolution
        rmtSetFilter(pwmRmt, true, 100);
        // End a capture after a few milliseconds of idle so 50 Hz servo PWM frames are delivered.
        rmtSetRxThreshold(pwmRmt, 5000);
        rmtRead(pwmRmt, pwmRmtCallback, nullptr);
    }

    // TRAMP still uses the hardware UART. SmartAudio uses a dedicated one-wire bit-banged transport.
    if (selected_protocol == PROTO_SMARTAUDIO) {
        smartaudioSetOneWirePin(VTX_UART_PIN);
        smartaudioSetPrependZero(true);
    } else {
        Serial1.begin(9600, SERIAL_8N1, VTX_UART_PIN, VTX_UART_PIN);
    }
    Serial.begin(115200);
    Serial.println("ESP32-C3 VTX controller started (Betaflight-like protocols)");
    if (pwmRmt) {
        Serial.println("PWM capture: RMT");
    } else {
        Serial.println("PWM capture: RMT init failed");
    }

    // Initialize filesystem and load VTX table
    if (!storageBegin()) {
        Serial.println("LittleFS failed to mount");
    } else {
        if (!storageEnsureVtxFile("/peak_thor_t35.json")) {
            Serial.println("Failed to ensure VTX JSON exists on LittleFS");
        } else if (!storageLoadVtxTable("/peak_thor_t35.json")) {
            Serial.println("Failed to parse VTX JSON from LittleFS");
        } else {
            DynamicJsonDocument* doc = storageGetVtxDoc();
            if (doc) {
                JsonArray bands = (*doc)["vtx_table"]["bands_list"].as<JsonArray>();
                JsonArray pows = (*doc)["vtx_table"]["powerlevels_list"].as<JsonArray>();
                Serial.print("Loaded VTX table: bands="); Serial.print(bands.size());
                Serial.print(" powerlevels="); Serial.println(pows.size());
            }
        }
    }

    // Protocol selection is hardcoded.
    if (selected_protocol == PROTO_SMARTAUDIO) {
        Serial.println("Protocol: SmartAudio (hardcoded)");
        smartaudioEnableDebug(true);
    } else {
        Serial.println("Protocol: TRAMP (hardcoded)");
    }
}

void loop() {
    if (pulseAvailable) {
        noInterrupts();
        uint16_t pulse = capturedPulse;
        pulseAvailable = false;
        interrupts();

        // Smooth the pulse slightly and require a few consecutive readings before applying a new slot.
        if (pulse < 500) pulse = 500;
        if (pulse > 2500) pulse = 2500;

        if (!filterInitialized) {
            filteredPulse = pulse;
            filterInitialized = true;
        } else {
            filteredPulse = (uint16_t)((filteredPulse * 3U + pulse) / 4U);
        }

        const uint32_t nBands = sizeof(bands)/sizeof(bands[0]);
        const uint32_t nChans = sizeof(channels)/sizeof(channels[0]);
        const uint32_t nPows = sizeof(powers)/sizeof(powers[0]);
        const uint32_t nPit = sizeof(pitModes)/sizeof(pitModes[0]);
        uint32_t total = nBands * nChans * nPows * nPit;
        int32_t idx = map(filteredPulse, 500, 2500, 0, total - 1);

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
                Serial.print("Pulse: "); Serial.print(pulse);
                Serial.print(" filtered: "); Serial.print(filteredPulse);
                Serial.println(" -> sent VTX settings");
            }
        }
    }
    // Process only the selected protocol
    if (selected_protocol == PROTO_SMARTAUDIO) {
        smartaudioProcess();
    } else {
        trampProcess();
    }
}
