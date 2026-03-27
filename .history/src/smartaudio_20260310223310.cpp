#include <Arduino.h>
#include <string.h>
#include "smartaudio.h"
#include "crc8.h"
#include <driver/uart.h>

// SmartAudio framing: 0xAA 0x55 <len> <cmd> <payload...> <crc8>
// CRC8 is computed over: <len> <cmd> <payload...>

// Queue definitions
typedef struct {
    uint8_t buf[32]; // raw len,cmd,payload
    uint8_t len;
    uint8_t cmd;
    uint8_t retries;
    uint32_t lastSendMs;
} sa_cmd_t;

static sa_cmd_t sa_queue[SMARTAUDIO_QUEUE_DEPTH];
static int sa_q_head = 0, sa_q_tail = 0;
static bool sa_debug = false;
static int sa_dir_pin = -1;

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

void smartaudioEnableDebug(bool enable) { sa_debug = enable; }

static void sa_dir_tx(bool on) {
    if (sa_dir_pin < 0) return;
    digitalWrite(sa_dir_pin, on ? HIGH : LOW);
}

static void smartaudio_send_frame_raw(const uint8_t *raw, uint8_t rawLen) {
    // raw contains len,cmd,payload
    sa_dir_tx(true);
    uint8_t crc = crc8_dvb_s2(raw, rawLen);
    if (sa_debug) {
        Serial.print("SA raw: AA 55 ");
        for (uint8_t i = 0; i < rawLen; i++) { Serial.print(raw[i], HEX); Serial.print(" "); }
        Serial.print(crc, HEX); Serial.println();
    }
    Serial1.write(0xAA);
    Serial1.write(0x55);
    Serial1.write(raw, rawLen);
    Serial1.write(crc);
    sa_dir_tx(false);
}

// Backwards-compatible immediate-send wrappers used by main
void smartaudioSendBandChannel(uint8_t band, uint8_t channel) {
    smartaudioEnqueueSetChannel(band, channel);
}

void smartaudioSendPower(uint8_t power) {
    smartaudioEnqueueSetPower(power);
}

void smartaudioSendFrequency(uint16_t frequency, bool pitmodeFrequency) {
    smartaudioEnqueueSetFrequency(frequency, pitmodeFrequency);
}

bool smartaudioEnqueueGetSettings(void) {
    uint8_t frame[2] = {0, 0x03};
    return sa_enqueue_raw(frame, 2, 0x03);
}

bool smartaudioEnqueueSetChannel(uint8_t band, uint8_t channel) {
    if (band == 0 || channel == 0) {
        return false;
    }
    // Betaflight sends a single packed channel index: (band - 1) * 8 + (channel - 1)
    const uint8_t packedChannel = (uint8_t)(((band - 1) * 8) + (channel - 1));
    uint8_t frame[3] = {1, 0x07, packedChannel};
    return sa_enqueue_raw(frame, 3, 0x07);
}

bool smartaudioEnqueueSetPower(uint8_t power) {
    // SmartAudio expects the raw power value, not a simple local index.
    uint8_t frame[3] = {1, 0x05, power};
    return sa_enqueue_raw(frame, 3, 0x05);
}

bool smartaudioEnqueueSetFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t frame[4] = {2, 0x09, (uint8_t)((val>>8)&0xFF), (uint8_t)(val & 0xFF)};
    return sa_enqueue_raw(frame, 4, 0x09);
}

// parse settings response
static bool smartaudioParseSettingsResponse(const uint8_t *buf, size_t len, uint8_t *channel, uint8_t *power, uint8_t *opmode, uint16_t *frequency) {
    // find 0xAA 0x55
    for (size_t i = 0; i + 5 < len; i++) {
        if (buf[i] != 0xAA || buf[i+1] != 0x55) continue;
        uint8_t plen = buf[i+2];
        uint8_t cmd = buf[i+3];
        if (i + 3 + plen + 1 >= len) continue;
        const uint8_t *payload = &buf[i+4];
        uint8_t crc = buf[i+4+plen];
        uint8_t crcBuf[2 + 32];
        uint8_t cc=0; crcBuf[cc++]=plen; crcBuf[cc++]=cmd; for (uint8_t j=0;j<plen && cc<sizeof(crcBuf);j++) crcBuf[cc++]=payload[j];
        if (crc8_dvb_s2(crcBuf, cc) != crc) continue;
        if (plen >= 5) {
            *channel = payload[0]; *power = payload[1]; *opmode = payload[2]; *frequency = ((uint16_t)payload[3] << 8) | payload[4];
            return true;
        }
    }
    return false;
}

bool smartaudioProbe(smartaudio_probe_result_t *result, uint32_t timeout_ms) {
    if (!result) return false;
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
        sa_cmd_t *e = &sa_queue[sa_q_head];
        if (sa_debug) { Serial.print("SA send cmd=0x"); Serial.println(e->cmd, HEX); }
        smartaudio_send_frame_raw(e->buf, e->len);
        e->retries++;
        e->lastSendMs = millis();
        sa_outstanding = e->cmd;
        if (e->retries > SMARTAUDIO_MAX_RETRIES) {
            if (sa_debug) Serial.println("SA drop after retries");
            sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
            sa_outstanding = 0;
        }
    } else if (sa_outstanding) {
        // check head timeout
        if (!sa_queue_is_empty()) {
            sa_cmd_t *e = &sa_queue[sa_q_head];
            if (millis() - e->lastSendMs > 300) {
                if (e->retries >= SMARTAUDIO_MAX_RETRIES) { sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH; sa_outstanding = 0; }
                else { smartaudio_send_frame_raw(e->buf, e->len); e->retries++; e->lastSendMs = millis(); }
            }
        } else sa_outstanding = 0;
    }
}
