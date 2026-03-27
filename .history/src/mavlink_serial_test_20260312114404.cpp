#include <Arduino.h>

extern "C" {
#include <common/mavlink.h>
}

namespace {

HardwareSerial kFlightControllerSerial(1);

constexpr uint32_t kUsbBaud = 115200;
constexpr uint32_t kMavlinkBaud = 115200;
constexpr int kMavlinkRxPin = 5;
constexpr int kMavlinkTxPin = 6;
constexpr uint8_t kLocalSystemId = 2;
constexpr uint8_t kLocalComponentId = 158;
constexpr uint16_t kTestCommandId = 31001;
constexpr uint32_t kHeartbeatIntervalMs = 1000;
constexpr uint32_t kSerialHeartbeatIntervalMs = 1000;

unsigned long lastHeartbeatAtMs = 0;
unsigned long lastSerialHeartbeatAtMs = 0;
mavlink_status_t mavlinkStatus = {};

bool isCommandForThisNode(const mavlink_command_long_t& command) {
    const bool systemMatches = command.target_system == 0 || command.target_system == kLocalSystemId;
    const bool componentMatches = command.target_component == 0 || command.target_component == kLocalComponentId;
    return systemMatches && componentMatches;
}

void writeMessage(const mavlink_message_t& message) {
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN] = {};
    const uint16_t length = mavlink_msg_to_send_buffer(buffer, &message);
    if (length > 0) {
        kFlightControllerSerial.write(buffer, length);
    }
}

void sendHeartbeat() {
    mavlink_message_t message = {};
    mavlink_msg_heartbeat_pack_chan(kLocalSystemId,
                                    kLocalComponentId,
                                    MAVLINK_COMM_0,
                                    &message,
                                    MAV_TYPE_ONBOARD_CONTROLLER,
                                    MAV_AUTOPILOT_INVALID,
                                    0,
                                    0,
                                    MAV_STATE_ACTIVE);
    writeMessage(message);
    Serial.printf("TX HEARTBEAT sys=%u comp=%u\n", static_cast<unsigned>(kLocalSystemId), static_cast<unsigned>(kLocalComponentId));
}

void sendCommandAck(const mavlink_message_t& sourceMessage,
                    const mavlink_command_long_t& command,
                    uint8_t result) {
    mavlink_message_t ackMessage = {};
    mavlink_msg_command_ack_pack_chan(kLocalSystemId,
                                      kLocalComponentId,
                                      MAVLINK_COMM_0,
                                      &ackMessage,
                                      command.command,
                                      result,
                                      0,
                                      0,
                                      sourceMessage.sysid,
                                      sourceMessage.compid);
    writeMessage(ackMessage);
    Serial.printf("TX ACK command=%u result=%u to=%u/%u\n",
                  static_cast<unsigned>(command.command),
                  static_cast<unsigned>(result),
                  static_cast<unsigned>(sourceMessage.sysid),
                  static_cast<unsigned>(sourceMessage.compid));
}

void logHeartbeat(const mavlink_message_t& message) {
    mavlink_heartbeat_t heartbeat = {};
    mavlink_msg_heartbeat_decode(&message, &heartbeat);
    Serial.printf("RX HEARTBEAT from=%u/%u type=%u autopilot=%u base_mode=%u system_status=%u\n",
                  static_cast<unsigned>(message.sysid),
                  static_cast<unsigned>(message.compid),
                  static_cast<unsigned>(heartbeat.type),
                  static_cast<unsigned>(heartbeat.autopilot),
                  static_cast<unsigned>(heartbeat.base_mode),
                  static_cast<unsigned>(heartbeat.system_status));
}

void logCommandLong(const mavlink_message_t& message) {
    mavlink_command_long_t command = {};
    mavlink_msg_command_long_decode(&message, &command);

    Serial.printf("RX COMMAND_LONG from=%u/%u target=%u/%u command=%u params=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f]\n",
                  static_cast<unsigned>(message.sysid),
                  static_cast<unsigned>(message.compid),
                  static_cast<unsigned>(command.target_system),
                  static_cast<unsigned>(command.target_component),
                  static_cast<unsigned>(command.command),
                  static_cast<double>(command.param1),
                  static_cast<double>(command.param2),
                  static_cast<double>(command.param3),
                  static_cast<double>(command.param4),
                  static_cast<double>(command.param5),
                  static_cast<double>(command.param6),
                  static_cast<double>(command.param7));

    if (command.command != kTestCommandId) {
        return;
    }

    if (!isCommandForThisNode(command)) {
        Serial.println("IGNORED command 31001 because target does not match local sysid/compid");
        return;
    }

    sendCommandAck(message, command, MAV_RESULT_ACCEPTED);
}

void logCommandAck(const mavlink_message_t& message) {
    mavlink_command_ack_t ack = {};
    mavlink_msg_command_ack_decode(&message, &ack);
    Serial.printf("RX COMMAND_ACK from=%u/%u command=%u result=%u target=%u/%u\n",
                  static_cast<unsigned>(message.sysid),
                  static_cast<unsigned>(message.compid),
                  static_cast<unsigned>(ack.command),
                  static_cast<unsigned>(ack.result),
                  static_cast<unsigned>(ack.target_system),
                  static_cast<unsigned>(ack.target_component));
}

void handleMessage(const mavlink_message_t& message) {
    switch (message.msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT:
            logHeartbeat(message);
            break;

        case MAVLINK_MSG_ID_COMMAND_LONG:
            logCommandLong(message);
            break;

        case MAVLINK_MSG_ID_COMMAND_ACK:
            logCommandAck(message);
            break;

        default:
            Serial.printf("RX msgid=%lu from=%u/%u len=%u\n",
                          static_cast<unsigned long>(message.msgid),
                          static_cast<unsigned>(message.sysid),
                          static_cast<unsigned>(message.compid),
                          static_cast<unsigned>(message.len));
            break;
    }
}

void readFlightControllerMavlink() {
    mavlink_message_t message = {};
    while (kFlightControllerSerial.available() > 0) {
        const uint8_t byte = static_cast<uint8_t>(kFlightControllerSerial.read());
        if (mavlink_parse_char(MAVLINK_COMM_0, byte, &message, &mavlinkStatus)) {
            handleMessage(message);
        }
    }
}

void emitSerialHeartbeat() {
    Serial.printf("ESP alive uptime=%lus fc_uart_rx_pending=%d\n",
                  static_cast<unsigned long>(millis() / 1000UL),
                  kFlightControllerSerial.available());
}

}  // namespace

void setup() {
    Serial.begin(kUsbBaud);
    delay(500);
    Serial.println();
    Serial.println("ESP32-C3 MAVLink serial test starting");
    Serial.printf("Wire FC TX -> ESP32 GPIO%d, FC RX -> ESP32 GPIO%d, baud=%lu\n",
                  kMavlinkRxPin,
                  kMavlinkTxPin,
                  static_cast<unsigned long>(kMavlinkBaud));
    Serial.printf("Local MAVLink identity sys=%u comp=%u\n",
                  static_cast<unsigned>(kLocalSystemId),
                  static_cast<unsigned>(kLocalComponentId));

    kFlightControllerSerial.begin(kMavlinkBaud, SERIAL_8N1, kMavlinkRxPin, kMavlinkTxPin);
    emitSerialHeartbeat();
    sendHeartbeat();
    lastHeartbeatAtMs = millis();
    lastSerialHeartbeatAtMs = millis();
}

void loop() {
    readFlightControllerMavlink();

    const unsigned long now = millis();
    if (now - lastSerialHeartbeatAtMs >= kSerialHeartbeatIntervalMs) {
        emitSerialHeartbeat();
        lastSerialHeartbeatAtMs = now;
    }

    if (now - lastHeartbeatAtMs >= kHeartbeatIntervalMs) {
        sendHeartbeat();
        lastHeartbeatAtMs = now;
    }
}