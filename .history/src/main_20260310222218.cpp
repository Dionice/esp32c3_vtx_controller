#include <Arduino.h>
#include "smartaudio.h"
#include "tramp.h"
#include "storage.h"

// Protocol selection: set to 0 for SmartAudio, 1 for TRAMP
enum vtx_protocol_t { PROTO_SMARTAUDIO = 0, PROTO_TRAMP = 1 };
static const vtx_protocol_t selected_protocol = PROTO_SMARTAUDIO;

// Pin configuration
#define PWM_INPUT_PIN 2
#define VTX_UART_TX 4 // UART TX pin to VTX (adjust wiring)

// Parameter lists (example sets — adjust to your VTX supported values)
const uint8_t bands[] = {1,2,3,4}; // placeholder band ids
const uint8_t channels[] = {1,2,3,4,5,6,7,8};
const uint8_t powers[] = {0,1,2,3}; // power level indices
const uint8_t pitModes[] = {0,1}; // off, on

volatile unsigned long lastRisingMicros = 0;
volatile uint16_t capturedPulse = 0;
volatile bool pulseAvailable = false;
static int32_t lastMappedIndex = -1;

// Interrupt-based PWM capture (non-blocking)
void IRAM_ATTR pwmIsr() {
    bool level = digitalRead(PWM_INPUT_PIN);
    unsigned long t = micros();
    if (level) {
        lastRisingMicros = t;
    } else {
        unsigned long width = t - lastRisingMicros;
        if (width > 400 && width < 30000) {
            capturedPulse = (uint16_t)width;
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

    // Use SmartAudio to set band/channel and power. For frequency-based VTXs, call frequency setter.
    smartaudioSendBandChannel(band, chan);
    delay(20);
    smartaudioSendPower(power);
    if (pit) {
        // optionally set pit frequency if needed
        // smartaudioSendFrequency(0 /*example*/, true);
    }
}

void setup() {
    pinMode(PWM_INPUT_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(PWM_INPUT_PIN), pwmIsr, CHANGE);

    // SmartAudio default baud; betaflight uses ~4900 for many devices
    Serial1.begin(4900, SERIAL_8N1, -1, VTX_UART_TX);
    Serial.begin(115200);
    Serial.println("ESP32-C3 VTX controller started (Betaflight-like protocols)");

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

        // map pulse to index without sending if unchanged (debounce)
        if (pulse < 500) pulse = 500;
        if (pulse > 2500) pulse = 2500;

        const uint32_t nBands = sizeof(bands)/sizeof(bands[0]);
        const uint32_t nChans = sizeof(channels)/sizeof(channels[0]);
        const uint32_t nPows = sizeof(powers)/sizeof(powers[0]);
        const uint32_t nPit = sizeof(pitModes)/sizeof(pitModes[0]);
        uint32_t total = nBands * nChans * nPows * nPit;
        int32_t idx = map(pulse, 500, 2500, 0, total - 1);
        if (idx != lastMappedIndex) {
            lastMappedIndex = idx;
            decodeAndSend(pulse);
            Serial.print("Pulse: "); Serial.print(pulse);
            Serial.println(" -> sent VTX settings");
        }
    }
    // Process only the selected protocol
    if (selected_protocol == PROTO_SMARTAUDIO) {
        smartaudioProcess();
    } else {
        trampProcess();
    }
}
