#include <Arduino.h>
#include <string.h>
#include "smartaudio.h"
#include "crc8.h"
#include <driver/uart.h>
#include <driver/gpio.h>

// SmartAudio framing used by Betaflight: 0xAA 0x55 <cmd> <len> <payload...> <crc8>
// CRC8 is computed over: 0xAA 0x55 <cmd> <len> <payload...>

// Queue definitions
typedef struct {
    uint8_t buf[32]; // raw cmd,len,payload
    uint8_t len;
    uint8_t cmd;
    uint8_t retries;
    uint32_t lastSendMs;
} sa_cmd_t;

static sa_cmd_t sa_queue[SMARTAUDIO_QUEUE_DEPTH];
static int sa_q_head = 0, sa_q_tail = 0;
static bool sa_debug = false;
static int sa_dir_pin = -1;
static int sa_wire_pin = -1;
static bool sa_prepend_zero = true;
static bool sa_bitbang_enabled = false;
static uint32_t sa_last_send_ms = 0;

static const uint32_t SA_BIT_US = 208;
static const uint32_t SA_FRAME_GAP_MS = 150;
static const uint32_t SA_RESPONSE_TIMEOUT_MS = 120;
static const uint8_t SA_BITBANG_SEND_ATTEMPTS = 2;

smartaudio_device_state_t sa_state = {0,0,0,0,false};
static uint8_t sa_rbuf[128];
static size_t sa_rpos = 0;
static uint8_t sa_outstanding = 0;

static bool sa_queue_is_empty(void) { return sa_q_head == sa_q_tail; }
static bool sa_queue_is_full(void) { return ((sa_q_tail + 1) % SMARTAUDIO_QUEUE_DEPTH) == sa_q_head; }

static bool sa_enqueue_raw(const uint8_t *frame, uint8_t len, uint8_t cmd) {
    if (sa_queue_is_full()) return false;
    sa_cmd_t *e = &sa_queue[sa_q_tail];
    memset(e,0,sizeof(*e));
    memcpy(e->buf, frame, len);
    e->len = len;
    e->cmd = cmd;
    e->retries = 0;
    e->lastSendMs = 0;
    sa_q_tail = (sa_q_tail + 1) % SMARTAUDIO_QUEUE_DEPTH;
    return true;
}

void smartaudioSetDirPin(int pin) {
    sa_dir_pin = pin;
    if (sa_dir_pin >= 0) {
        pinMode(sa_dir_pin, OUTPUT);
        digitalWrite(sa_dir_pin, LOW);
    }
}

void smartaudioSetInvertUART(bool invert) {
    // UART inversion is platform specific; store flag and attempt no-op.
    // On some ESP32 cores you can call uart_set_line_inverse() with UART_INVERSE_TXD/RXD.
    // Here we only store the flag for user visibility and for potential future add-on.
    (void)invert;
}

void smartaudioSetOneWirePin(int pin) {
    sa_wire_pin = pin;
    sa_bitbang_enabled = (pin >= 0);
    if (sa_wire_pin >= 0) {
        gpio_reset_pin((gpio_num_t)sa_wire_pin);
        pinMode(sa_wire_pin, INPUT_PULLUP);
    }
}

void smartaudioSetPrependZero(bool enable) {
    sa_prepend_zero = enable;
}

void smartaudioEnableDebug(bool enable) { sa_debug = enable; }

static void sa_dir_tx(bool on) {
    if (sa_dir_pin < 0) return;
    digitalWrite(sa_dir_pin, on ? HIGH : LOW);
}

static void sa_wire_begin_tx(void) {
    if (sa_wire_pin < 0) {
        return;
    }
    gpio_set_direction((gpio_num_t)sa_wire_pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)sa_wire_pin, 1);
}

static void sa_wire_end_tx(void) {
    if (sa_wire_pin < 0) {
        return;
    }
    pinMode(sa_wire_pin, INPUT_PULLUP);
}

static void sa_write_bitbang_byte(uint8_t byte) {
    noInterrupts();
    gpio_set_level((gpio_num_t)sa_wire_pin, 0);
    delayMicroseconds(SA_BIT_US);
    for (uint8_t bit = 0; bit < 8; bit++) {
        gpio_set_level((gpio_num_t)sa_wire_pin, (byte >> bit) & 0x01);
        delayMicroseconds(SA_BIT_US);
    }
    gpio_set_level((gpio_num_t)sa_wire_pin, 1);
    delayMicroseconds(SA_BIT_US);
    delayMicroseconds(SA_BIT_US);
    interrupts();
}

static void smartaudio_send_frame_raw(const uint8_t *raw, uint8_t rawLen) {
    // raw contains cmd,len,payload
    sa_dir_tx(true);
    uint8_t frame[40];
    if (rawLen + 3 > sizeof(frame)) {
        sa_dir_tx(false);
        return;
    }
    frame[0] = 0xAA;
    frame[1] = 0x55;
    memcpy(&frame[2], raw, rawLen);
    uint8_t crc = crc8_dvb_s2(frame, rawLen + 2);
    if (sa_debug) {
        Serial.print("SA raw: AA 55 ");
        for (uint8_t i = 0; i < rawLen; i++) { Serial.print(raw[i], HEX); Serial.print(" "); }
        Serial.print(crc, HEX); Serial.println();
    }
    if (sa_bitbang_enabled && sa_wire_pin >= 0) {
        sa_wire_begin_tx();
        delayMicroseconds(SA_BIT_US);
        if (sa_prepend_zero) {
            sa_write_bitbang_byte(0x00);
        }
        for (uint8_t i = 0; i < rawLen + 2; i++) {
            sa_write_bitbang_byte(frame[i]);
        }
        sa_write_bitbang_byte(crc);
        sa_wire_end_tx();
    } else {
        Serial1.write(frame, rawLen + 2);
        Serial1.write(crc);
    }
    sa_dir_tx(false);
}

// Backwards-compatible immediate-send wrappers used by main
void smartaudioSendBandChannel(uint8_t band, uint8_t channel) {
    if (band == 0 || channel == 0) {
        return;
    }
    const uint8_t packedChannel = (uint8_t)(((band - 1) * 8) + (channel - 1));
    uint8_t frame[3] = {0x07, 1, packedChannel};
    smartaudio_send_frame_raw(frame, 3);
}

void smartaudioSendPower(uint8_t power) {
    uint8_t frame[3] = {0x05, 1, power};
    smartaudio_send_frame_raw(frame, 3);
}

void smartaudioSendFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t frame[4] = {0x09, 2, (uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF)};
    smartaudio_send_frame_raw(frame, 4);
}

bool smartaudioEnqueueGetSettings(void) {
    uint8_t frame[2] = {0x03, 0};
    return sa_enqueue_raw(frame, 2, 0x03);
}

bool smartaudioEnqueueSetChannel(uint8_t band, uint8_t channel) {
    if (band == 0 || channel == 0) {
        return false;
    }
    // Betaflight sends a single packed channel index: (band - 1) * 8 + (channel - 1)
    const uint8_t packedChannel = (uint8_t)(((band - 1) * 8) + (channel - 1));
    uint8_t frame[3] = {0x07, 1, packedChannel};
    return sa_enqueue_raw(frame, 3, 0x07);
}

bool smartaudioEnqueueSetPower(uint8_t power) {
    // SmartAudio expects the raw power value, not a simple local index.
    uint8_t frame[3] = {0x05, 1, power};
    return sa_enqueue_raw(frame, 3, 0x05);
}

bool smartaudioEnqueueSetFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t frame[4] = {0x09, 2, (uint8_t)((val>>8)&0xFF), (uint8_t)(val & 0xFF)};
    return sa_enqueue_raw(frame, 4, 0x09);
}

bool smartaudioIsBusy(void) {
    return sa_outstanding != 0 || !sa_queue_is_empty();
}

// parse settings response
static bool smartaudioParseSettingsResponse(const uint8_t *buf, size_t len, uint8_t *channel, uint8_t *power, uint8_t *opmode, uint16_t *frequency) {
    // find 0xAA 0x55
    for (size_t i = 0; i + 5 < len; i++) {
        if (buf[i] != 0xAA || buf[i+1] != 0x55) continue;
        uint8_t cmd = buf[i+2];
        uint8_t plen = buf[i+3];
        if (i + 4 + plen >= len) continue;
        const uint8_t *payload = &buf[i+4];
        uint8_t crc = buf[i+4+plen];
        if (crc8_dvb_s2(&buf[i], (uint8_t)(4 + plen)) != crc) continue;
        if ((cmd == 0x01 || cmd == 0x09 || cmd == 0x11) && plen >= 5) {
            *channel = payload[0]; *power = payload[1]; *opmode = payload[2]; *frequency = ((uint16_t)payload[3] << 8) | payload[4];
            return true;
        }
    }
    return false;
}

bool smartaudioProbe(smartaudio_probe_result_t *result, uint32_t timeout_ms) {
    if (!result) return false;
    if (sa_bitbang_enabled) return false;
    smartaudioEnqueueGetSettings();
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        // pull incoming
        while (Serial1.available() && sa_rpos < sizeof(sa_rbuf)) sa_rbuf[sa_rpos++] = (uint8_t)Serial1.read();
        uint8_t ch,pw,op; uint16_t freq;
        if (smartaudioParseSettingsResponse(sa_rbuf, sa_rpos, &ch,&pw,&op,&freq)) {
            result->channel = ch; result->power = pw; result->operationMode = op; result->frequency = freq;
            return true;
        }
    }
    return false;
}

static void sa_handle_incoming(void) {
    if (sa_bitbang_enabled) {
        return;
    }
    while (Serial1.available() && sa_rpos < sizeof(sa_rbuf)) sa_rbuf[sa_rpos++] = (uint8_t)Serial1.read();
    uint8_t ch,pw,op; uint16_t freq;
    if (smartaudioParseSettingsResponse(sa_rbuf, sa_rpos, &ch,&pw,&op,&freq)) {
        sa_state.channel = ch; sa_state.power = pw; sa_state.opMode = op; sa_state.frequency = freq; sa_state.valid = true; sa_rpos = 0;
        // consume matching queued item if head matches
        if (!sa_queue_is_empty()) {
            sa_cmd_t *head = &sa_queue[sa_q_head];
            if (head->cmd == 0x03 || head->cmd == 0x05 || head->cmd == 0x07 || head->cmd == 0x09) {
                sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                sa_outstanding = 0;
            }
        }
    }
}

void smartaudioProcess(void) {
    sa_handle_incoming();
    // if no outstanding, send next queued
    if (sa_outstanding == 0 && !sa_queue_is_empty()) {
        if (millis() - sa_last_send_ms < SA_FRAME_GAP_MS) {
            return;
        }
        sa_cmd_t *e = &sa_queue[sa_q_head];
        if (sa_debug) { Serial.print("SA send cmd=0x"); Serial.println(e->cmd, HEX); }
        smartaudio_send_frame_raw(e->buf, e->len);
        e->retries++;
        e->lastSendMs = millis();
        sa_last_send_ms = e->lastSendMs;
        if (sa_bitbang_enabled) {
            if (e->retries >= SA_BITBANG_SEND_ATTEMPTS) {
                sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                sa_outstanding = 0;
            } else {
                sa_outstanding = e->cmd;
            }
        } else {
            sa_outstanding = e->cmd;
            if (e->retries > SMARTAUDIO_MAX_RETRIES) {
                if (sa_debug) Serial.println("SA drop after retries");
                sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                sa_outstanding = 0;
            }
        }
    } else if (sa_outstanding) {
        // check head timeout
        if (!sa_queue_is_empty()) {
            sa_cmd_t *e = &sa_queue[sa_q_head];
            const uint32_t retryDelayMs = sa_bitbang_enabled ? SA_FRAME_GAP_MS : SA_RESPONSE_TIMEOUT_MS;
            const uint8_t maxAttempts = sa_bitbang_enabled ? SA_BITBANG_SEND_ATTEMPTS : SMARTAUDIO_MAX_RETRIES;
            if (millis() - e->lastSendMs > retryDelayMs) {
                if (e->retries >= maxAttempts) {
                    sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                    sa_outstanding = 0;
                } else {
                    smartaudio_send_frame_raw(e->buf, e->len);
                    e->retries++;
                    e->lastSendMs = millis();
                    sa_last_send_ms = e->lastSendMs;
                }
            }
        } else sa_outstanding = 0;
    }
}
