#include <Arduino.h>

// Pin configuration
#define PWM_INPUT_PIN 4
#define VTX_UART_TX 1 // UART TX pin to VTX (adjust wiring)

// Parameter lists (example sets — adjust to your VTX supported values)
const uint8_t bands[] = {1,2,3,4}; // placeholder band ids
const uint8_t channels[] = {1,2,3,4,5,6,7,8};
const uint8_t powers[] = {25,100,200,400}; // mW
const uint8_t pitModes[] = {0,1}; // off, on

HardwareSerial VTXSerial(0);

unsigned long lastSend = 0;
unsigned long sendInterval = 200; // ms

uint16_t readPulse() {
    // measure pulse width in microseconds (blocking)
    return pulseIn(PWM_INPUT_PIN, HIGH, 30000); // 30ms timeout
}

void sendSmartAudioSet(uint8_t band, uint8_t channel, uint8_t power, uint8_t pit) {
    // Compose a minimal SmartAudio-like frame: 0xAA 0x55 <len> <cmd> <payload...> <crc>
    // This is a simple implementation — may need tweaks for specific VTX versions.
    uint8_t payload[6];
    // Pin configuration
    #define PWM_INPUT_PIN 4
    #define VTX_UART_TX 1 // UART TX pin to VTX (adjust wiring)

    // Parameter lists (example sets — adjust to your VTX supported values)
    const uint8_t bands[] = {1,2,3,4}; // placeholder band ids
    const uint8_t channels[] = {1,2,3,4,5,6,7,8};
    const uint8_t powers[] = {25,100,200,400}; // mW
    const uint8_t pitModes[] = {0,1}; // off, on

    // Use Serial1 (UART0 is Serial on some boards) — configure in setup

    volatile unsigned long lastRisingMicros = 0;
    volatile uint16_t capturedPulse = 0;
    volatile bool pulseAvailable = false;

    unsigned long lastSend = 0;
    unsigned long sendInterval = 200; // ms

    // Forward declarations for protocol helpers
    void smartaudioSendBandChannel(uint8_t band, uint8_t channel);
    void smartaudioSendPower(uint8_t power);
    void smartaudioSendFrequency(uint16_t freq, bool pit);
    void trampSendFrequency(uint16_t freq);

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
    }

    void loop() {
        unsigned long now = millis();
        if (pulseAvailable) {
            noInterrupts();
            uint16_t pulse = capturedPulse;
            pulseAvailable = false;
            interrupts();

            decodeAndSend(pulse);
            Serial.print("Pulse: "); Serial.print(pulse);
            Serial.println(" -> sent VTX settings");
        }

        // also keep periodic tasks if needed
        (void) now;
    }
