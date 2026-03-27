#include <Arduino.h>
#include "smartaudio.h"
#include "crc8.h"

// SmartAudio framing: 0xAA 0x55 <len> <cmd> <payload...> <crc8>
// CRC8 is computed over: <len> <cmd> <payload...>

static void smartaudioSendFrame(uint8_t command, const uint8_t *payload, uint8_t payloadLen) {
    uint8_t header[2] = { 0xAA, 0x55 };
    Serial1.write(header, 2);

    uint8_t len = payloadLen;
    Serial1.write(len);
    Serial1.write(command);

    if (payloadLen) Serial1.write(payload, payloadLen);

    // compute CRC8 over len, command and payload
    uint8_t crcBuf[2 + 32];
    uint8_t crcCount = 0;
    crcBuf[crcCount++] = len;
    crcBuf[crcCount++] = command;
    for (uint8_t i = 0; i < payloadLen && crcCount < sizeof(crcBuf); i++) crcBuf[crcCount++] = payload[i];
    uint8_t crc = crc8_dvb_s2(crcBuf, crcCount);
    Serial1.write(crc);
}

void smartaudioSendBandChannel(uint8_t band, uint8_t channel) {
    uint8_t payload[2] = { band, channel };
    // SMARTAUDIO_CMD_SET_CHANNEL is 0x07 per Betaflight
    smartaudioSendFrame(0x07, payload, 2);
}

void smartaudioSendPower(uint8_t power) {
    uint8_t payload[1] = { power };
    // SMARTAUDIO_CMD_SET_POWER is 0x05
    smartaudioSendFrame(0x05, payload, 1);
}

void smartaudioSendFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t payload[2] = { (uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF) };
    // SMARTAUDIO_CMD_SET_FREQUENCY is 0x09
    smartaudioSendFrame(0x09, payload, 2);
}

// Parse a SmartAudio response buffer (simple parser matching Get Settings response)
static bool smartaudioParseSettingsResponse(const uint8_t *buf, size_t len, uint8_t *channel, uint8_t *power, uint8_t *opmode, uint16_t *frequency) {
    if (len < 2 + 1 + 5 + 1) return false; // header(2) + len + payload(5) + crc
    // find start code 0xAA 0x55
    size_t i = 0;
    for (; i + 4 < len; i++) {
    #include <driver/uart.h>
    #include <string.h>
        if (buf[i] == 0xAA && buf[i+1] == 0x55) break;
    }
    if (i + 4 >= len) return false;

    // Command queue structures
    typedef struct {
        uint8_t buf[32];
        uint8_t len;
        uint8_t cmd; // command code
        uint8_t retries;
        uint32_t lastSendMs;
    } sa_cmd_t;

    static sa_cmd_t sa_queue[SMARTAUDIO_QUEUE_DEPTH];
    static int sa_q_head = 0;
    static int sa_q_tail = 0;
    static bool sa_debug = false;
    static int sa_dir_pin = -1;
    static bool sa_invert_uart = false;

    static bool sa_queue_is_full(void) {
        return ((sa_q_tail + 1) % SMARTAUDIO_QUEUE_DEPTH) == sa_q_head;
    }

    static bool sa_queue_is_empty(void) {
        return sa_q_head == sa_q_tail;
    }

    static bool sa_enqueue(const uint8_t *buf, uint8_t len, uint8_t cmd) {
        if (sa_queue_is_full()) return false;
        sa_cmd_t *entry = &sa_queue[sa_q_tail];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->buf, buf, len);
        entry->len = len;
        entry->cmd = cmd;
        entry->retries = 0;
        entry->lastSendMs = 0;
        sa_q_tail = (sa_q_tail + 1) % SMARTAUDIO_QUEUE_DEPTH;
        return true;
    }

    static bool sa_dequeue(sa_cmd_t *out) {
        if (sa_queue_is_empty()) return false;
        *out = sa_queue[sa_q_head];
        // mark removed
        sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
        return true;
    }

    void smartaudioSetDirPin(int pin) {
        sa_dir_pin = pin;
        if (sa_dir_pin >= 0) {
            pinMode(sa_dir_pin, OUTPUT);
            digitalWrite(sa_dir_pin, LOW); // RX by default
        }
    }

    void smartaudioSetInvertUART(bool invert) {
        sa_invert_uart = invert;
        if (invert) {
            // try to invert TX/RX lines on UART0 and UART1 (best-effort)
            uart_set_line_inverse(UART_NUM_0, UART_INVERSE_TXD | UART_INVERSE_RXD);
            uart_set_line_inverse(UART_NUM_1, UART_INVERSE_TXD | UART_INVERSE_RXD);
        } else {
            uart_set_line_inverse(UART_NUM_0, 0);
            uart_set_line_inverse(UART_NUM_1, 0);
        }
    }

    void smartaudioEnableDebug(bool enable) { sa_debug = enable; }

    // Utility: toggle dir pin when half-duplex is used
    static void sa_dir_tx_enable(bool tx) {
        if (sa_dir_pin < 0) return;
        digitalWrite(sa_dir_pin, tx ? HIGH : LOW);
    const uint8_t plen = buf[i+2];
    const uint8_t cmd = buf[i+3];
    if (plen < 5) return false;
        uint8_t tbuf[2] = {0x03, 0};
        sa_enqueue(NULL, 0, 0x03);

    // compute crc over len, cmd and payload
    uint8_t crcBuf[2 + 32];
        uint8_t payload[2] = { band, channel };
        uint8_t frame[4];
        frame[0] = 2; frame[1] = 0x07; frame[2] = band; frame[3] = channel;
        sa_enqueue(frame, 4, 0x07);
    uint8_t calc = crc8_dvb_s2(crcBuf, crcCount);
    if (calc != crc) return false;

        uint8_t frame[3];
        frame[0] = 1; frame[1] = 0x05; frame[2] = power;
        sa_enqueue(frame, 3, 0x05);
    *frequency = (uint16_t)payload[3] << 8 | payload[4];
    return true;
}
        uint16_t val = frequency & 0x3FFF;
        if (pitmodeFrequency) val |= (1 << 15);
        uint8_t frame[4];
        frame[0] = 2; frame[1] = 0x09; frame[2] = (uint8_t)((val >> 8) & 0xFF); frame[3] = (uint8_t)(val & 0xFF);
        sa_enqueue(frame, 4, 0x09);
    uint32_t start = millis();
    uint8_t buf[64];
    size_t pos = 0;
    while (millis() - start < timeout_ms) {
        while (Serial1.available() && pos < sizeof(buf)) {
            buf[pos++] = (uint8_t)Serial1.read();
        }
        if (pos >= 8) {
            uint8_t channel, power, opmode; uint16_t freq;
            if (smartaudioParseSettingsResponse(buf, pos, &channel, &power, &opmode, &freq)) {
                result->channel = channel;
                result->power = power;
                result->operationMode = opmode;
                result->frequency = freq;
                return true;
                // acknowledge any outstanding queue item matching GET or SET
                // if outstanding command matches (or queue not using outstanding), we clear outstanding
                sa_outstanding = 0;
                // advance queue head if first queued item was the one
                if (!sa_queue_is_empty()) {
                    // peek head
                    sa_cmd_t *head = &sa_queue[sa_q_head];
                    if (head->cmd == 0x03 || head->cmd == 0x05 || head->cmd == 0x07 || head->cmd == 0x09) {
                        // consume one
                        sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                    }
                }
        }
    }
    return false;
}

// --- State machine implementation ---

smartaudio_device_state_t sa_state = {0,0,0,0,false};

static uint32_t sa_lastTransmissionMs = 0;
static uint8_t sa_outstanding = 0; // outstanding command code
static uint8_t sa_osbuf[32];
        // If no outstanding, send next queued command
        if (!sa_outstanding && !sa_queue_is_empty()) {
            sa_cmd_t entry = sa_queue[sa_q_head];
            // prepare to send
            if (sa_debug) {
                Serial.print("SA: sending cmd=0x"); Serial.println(entry.cmd, HEX);
            }
            sa_dir_tx_enable(true);
            // send raw frame: header + entry.buf
            // entry.buf already contains len,cmd,payload
            Serial1.write(0xAA);
            Serial1.write(0x55);
            Serial1.write(entry.buf, entry.len);
            // compute CRC8 over len+cmd+payload
            uint8_t crc = crc8_dvb_s2(entry.buf, entry.len);
            Serial1.write(crc);
            sa_dir_tx_enable(false);
            sa_outstanding = entry.cmd;
            sa_queue[sa_q_head].lastSendMs = millis();
            sa_queue[sa_q_head].retries++;
            // if retries exceeded, drop it
            if (sa_queue[sa_q_head].retries > SMARTAUDIO_MAX_RETRIES) {
                if (sa_debug) { Serial.println("SA: retries exceeded, dropping"); }
                sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                sa_outstanding = 0;
            }
        } else if (sa_outstanding) {
            // check timeout and retransmit if needed
            if (millis() - sa_lastTransmissionMs > 200) {
                // retransmit current head if matches
                if (!sa_queue_is_empty()) {
                    sa_cmd_t *head = &sa_queue[sa_q_head];
                    if (head->cmd == sa_outstanding && head->retries < SMARTAUDIO_MAX_RETRIES) {
                        if (sa_debug) { Serial.println("SA: retransmit"); }
                        sa_dir_tx_enable(true);
                        Serial1.write(0xAA);
                        Serial1.write(0x55);
                        Serial1.write(head->buf, head->len);
                        uint8_t crc = crc8_dvb_s2(head->buf, head->len);
                        Serial1.write(crc);
                        sa_dir_tx_enable(false);
                        head->retries++;
                        head->lastSendMs = millis();
                    } else {
                        // give up and drop
                        sa_q_head = (sa_q_head + 1) % SMARTAUDIO_QUEUE_DEPTH;
                        sa_outstanding = 0;
                    }
                } else {
                    sa_outstanding = 0;
                }
                sa_lastTransmissionMs = millis();
            }
        }
    sa_lastTransmissionMs = millis();

    // Queue helper API implementations
    bool smartaudioEnqueueGetSettings(void) {
        uint8_t frame[2]; frame[0] = 0; frame[1] = 0x03; // len=0, cmd=0x03
        return sa_enqueue(frame+0, 2, 0x03);
    }

    bool smartaudioEnqueueSetChannel(uint8_t band, uint8_t channel) {
        uint8_t frame[4]; frame[0] = 2; frame[1] = 0x07; frame[2] = band; frame[3] = channel;
        return sa_enqueue(frame, 4, 0x07);
    }

    bool smartaudioEnqueueSetPower(uint8_t power) {
        uint8_t frame[3]; frame[0] = 1; frame[1] = 0x05; frame[2] = power;
        return sa_enqueue(frame, 3, 0x05);
    }

    bool smartaudioEnqueueSetFrequency(uint16_t frequency, bool pitmodeFrequency) {
        uint16_t val = frequency & 0x3FFF;
        if (pitmodeFrequency) val |= (1 << 15);
        uint8_t frame[4]; frame[0] = 2; frame[1] = 0x09; frame[2] = (uint8_t)((val>>8)&0xFF); frame[3] = (uint8_t)(val & 0xFF);
        return sa_enqueue(frame, 4, 0x09);
    }
}

void smartaudioRequestSetPower(uint8_t power) {
    uint8_t payload[1] = { power };
    smartaudioSendFrame(0x05, payload, 1);
    sa_outstanding = 0x05;
    sa_lastTransmissionMs = millis();
}

void smartaudioRequestSetFrequency(uint16_t frequency, bool pitmodeFrequency) {
    uint16_t val = frequency & 0x3FFF;
    if (pitmodeFrequency) val |= (1 << 15);
    uint8_t payload[2] = { (uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF) };
    smartaudioSendFrame(0x09, payload, 2);
    sa_outstanding = 0x09;
    sa_lastTransmissionMs = millis();
}

// Incoming byte buffer
static uint8_t sa_rbuf[64];
static size_t sa_rpos = 0;

static void sa_handle_incoming(void) {
    while (Serial1.available() && sa_rpos < sizeof(sa_rbuf)) {
        sa_rbuf[sa_rpos++] = (uint8_t)Serial1.read();
    }
    if (sa_rpos >= 6) {
        // try parse; support v1/v2 get settings responses and simpler ACKs
        uint8_t channel, power, opmode; uint16_t freq;
        if (smartaudioParseSettingsResponse(sa_rbuf, sa_rpos, &channel, &power, &opmode, &freq)) {
            sa_state.channel = channel;
            sa_state.power = power;
            sa_state.opMode = opmode;
            sa_state.frequency = freq;
            sa_state.valid = true;
            sa_outstanding = 0;
            // clear buffer
            sa_rpos = 0;
        } else {
            // not parsed yet; if buffer full or no valid header, shift/drop
            if (sa_rpos == sizeof(sa_rbuf)) sa_rpos = 0;
        }
    }
}

void smartaudioProcess(void) {
    sa_handle_incoming();
    // timeout handling for outstanding commands
    if (sa_outstanding && millis() - sa_lastTransmissionMs > 200) {
        // retransmit once
        if (sa_oslen > 0) {
            Serial1.write(sa_osbuf, sa_oslen);
            sa_lastTransmissionMs = millis();
            // clear stored outstanding to avoid infinite retransmit here
            sa_outstanding = 0;
            sa_oslen = 0;
        } else {
            sa_outstanding = 0; // give up
        }
    }
}

