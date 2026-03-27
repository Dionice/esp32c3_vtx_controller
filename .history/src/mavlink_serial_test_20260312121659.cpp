#include <Arduino.h>

extern "C" {
#include <common/mavlink.h>
}

namespace {

HardwareSerial kFlightControllerSerial(1);

constexpr uint32_t kUsbBaud = 115200;
constexpr uint32_t kMavlinkBaud = 57600;
constexpr int kMavlinkRxPin = 4;
constexpr int kMavlinkTxPin = 5;
constexpr uint8_t kLocalSystemId = 1;
constexpr uint8_t kLocalComponentId = MAV_COMP_ID_TELEMETRY_RADIO;
constexpr uint16_t kTestCommandId = 31001;
constexpr uint32_t kHeartbeatIntervalMs = 2000;
constexpr uint32_t kSerialHeartbeatIntervalMs = 1000;
constexpr uint32_t kStatusTextIntervalMs = 5000;
constexpr size_t kSerialReadChunkSize = 128;
constexpr uint32_t kFirmwareVersion = 0x020200FFUL;
constexpr uint32_t kMiddlewareVersion = 18;
constexpr uint16_t kVendorId = 0x1205;
constexpr uint16_t kProductId = 0x0001;

struct RxSummary {
    uint32_t total = 0;
    uint32_t heartbeat = 0;
    uint32_t statustext = 0;
    uint32_t attitude = 0;
    uint32_t globalPositionInt = 0;
    uint32_t sysStatus = 0;
    uint32_t rcChannels = 0;
    uint32_t commandLong = 0;
    uint32_t commandAck = 0;
    uint32_t requestMessage = 0;
    uint32_t other = 0;
    uint8_t lastSystemId = 0;
    uint8_t lastComponentId = 0;
    uint8_t lastHeartbeatType = 0;
    uint8_t lastHeartbeatAutopilot = 0;
    uint8_t lastHeartbeatBaseMode = 0;
    uint8_t lastHeartbeatSystemStatus = 0;
    bool hasSource = false;
    bool hasHeartbeat = false;
};

unsigned long lastHeartbeatAtMs = 0;
unsigned long lastSerialHeartbeatAtMs = 0;
unsigned long lastStatusTextAtMs = 0;
mavlink_status_t mavlinkStatus = {};
RxSummary rxSummary = {};

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
                                    MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
                                    0,
                                    MAV_STATE_ACTIVE);
    writeMessage(message);
}

void sendAutopilotVersion(const mavlink_message_t& sourceMessage) {
    mavlink_autopilot_version_t version = {};
    version.capabilities = MAV_PROTOCOL_CAPABILITY_MAVLINK2 | MAV_PROTOCOL_CAPABILITY_PARAM_ENCODE_BYTEWISE;
    version.flight_sw_version = kFirmwareVersion;
    version.middleware_sw_version = kMiddlewareVersion;
    version.vendor_id = kVendorId;
    version.product_id = kProductId;

    mavlink_message_t message = {};
    mavlink_msg_autopilot_version_encode_chan(kLocalSystemId,
                                              kLocalComponentId,
                                              MAVLINK_COMM_0,
                                              &message,
                                              &version);
    writeMessage(message);
    Serial.printf("TX AUTOPILOT_VERSION to=%u/%u caps=0x%08lX\n",
                  static_cast<unsigned>(sourceMessage.sysid),
                  static_cast<unsigned>(sourceMessage.compid),
                  static_cast<unsigned long>(version.capabilities));
}

void sendStatusText() {
    mavlink_message_t message = {};
    char text[50] = {};
    snprintf(text,
             sizeof(text),
             "ESP32 link alive %lus",
             static_cast<unsigned long>(millis() / 1000UL));
    mavlink_msg_statustext_pack_chan(kLocalSystemId,
                                     kLocalComponentId,
                                     MAVLINK_COMM_0,
                                     &message,
                                     MAV_SEVERITY_INFO,
                                     text,
                                     0,
                                     0);
    writeMessage(message);
    Serial.printf("TX STATUSTEXT '%s'\n", text);
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
    rxSummary.lastHeartbeatType = heartbeat.type;
    rxSummary.lastHeartbeatAutopilot = heartbeat.autopilot;
    rxSummary.lastHeartbeatBaseMode = heartbeat.base_mode;
    rxSummary.lastHeartbeatSystemStatus = heartbeat.system_status;
    rxSummary.hasHeartbeat = true;
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

    if (!isCommandForThisNode(command)) {
        Serial.println("IGNORED COMMAND_LONG because target does not match local sysid/compid");
        return;
    }

    if (command.command == MAV_CMD_REQUEST_MESSAGE) {
        rxSummary.requestMessage++;
        const uint32_t requestedMessageId = static_cast<uint32_t>(command.param1);
        if (requestedMessageId == MAVLINK_MSG_ID_AUTOPILOT_VERSION) {
            sendCommandAck(message, command, MAV_RESULT_ACCEPTED);
            sendAutopilotVersion(message);
        } else {
            sendCommandAck(message, command, MAV_RESULT_UNSUPPORTED);
            Serial.printf("UNSUPPORTED REQUEST_MESSAGE msgid=%lu\n",
                          static_cast<unsigned long>(requestedMessageId));
        }
        return;
    }

    if (command.command == kTestCommandId) {
        sendCommandAck(message, command, MAV_RESULT_ACCEPTED);
        return;
    }

    Serial.printf("IGNORED COMMAND_LONG command=%u\n", static_cast<unsigned>(command.command));
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
    rxSummary.total++;
    rxSummary.lastSystemId = message.sysid;
    rxSummary.lastComponentId = message.compid;
    rxSummary.hasSource = true;

    switch (message.msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT:
            rxSummary.heartbeat++;
            logHeartbeat(message);
            break;

        case MAVLINK_MSG_ID_STATUSTEXT:
            rxSummary.statustext++;
            break;

        case MAVLINK_MSG_ID_ATTITUDE:
            rxSummary.attitude++;
            break;

        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
            rxSummary.globalPositionInt++;
            break;

        case MAVLINK_MSG_ID_SYS_STATUS:
            rxSummary.sysStatus++;
            break;

        case MAVLINK_MSG_ID_RC_CHANNELS:
            rxSummary.rcChannels++;
            break;

        case MAVLINK_MSG_ID_COMMAND_LONG:
            rxSummary.commandLong++;
            logCommandLong(message);
            break;

        case MAVLINK_MSG_ID_COMMAND_ACK:
            rxSummary.commandAck++;
            logCommandAck(message);
            break;

        default:
            rxSummary.other++;
            break;
    }
}

void readFlightControllerMavlink() {
    mavlink_message_t message = {};
    uint8_t buffer[kSerialReadChunkSize] = {};
    while (kFlightControllerSerial.available() > 0) {
        const size_t bytesToRead = min(static_cast<size_t>(kFlightControllerSerial.available()), sizeof(buffer));
        const size_t bytesRead = kFlightControllerSerial.readBytes(buffer, bytesToRead);
        for (size_t index = 0; index < bytesRead; ++index) {
            if (mavlink_parse_char(MAVLINK_COMM_0, buffer[index], &message, &mavlinkStatus)) {
                handleMessage(message);
            }
        }
    }
}

void emitSerialHeartbeat() {
    Serial.printf("ESP alive uptime=%lus uart_pending=%d rx=%lu",
                  static_cast<unsigned long>(millis() / 1000UL),
                  kFlightControllerSerial.available(),
                  static_cast<unsigned long>(rxSummary.total));

    if (rxSummary.hasSource) {
        Serial.printf(" from=%u/%u",
                      static_cast<unsigned>(rxSummary.lastSystemId),
                      static_cast<unsigned>(rxSummary.lastComponentId));
    }

    Serial.printf(" hb=%lu att=%lu gps=%lu sys=%lu rc=%lu text=%lu cmd=%lu req=%lu ack=%lu other=%lu",
                  static_cast<unsigned long>(rxSummary.heartbeat),
                  static_cast<unsigned long>(rxSummary.attitude),
                  static_cast<unsigned long>(rxSummary.globalPositionInt),
                  static_cast<unsigned long>(rxSummary.sysStatus),
                  static_cast<unsigned long>(rxSummary.rcChannels),
                  static_cast<unsigned long>(rxSummary.statustext),
                  static_cast<unsigned long>(rxSummary.commandLong),
                  static_cast<unsigned long>(rxSummary.requestMessage),
                  static_cast<unsigned long>(rxSummary.commandAck),
                  static_cast<unsigned long>(rxSummary.other));

    if (rxSummary.hasHeartbeat) {
        Serial.printf(" fc_state(type=%u autopilot=%u mode=%u status=%u)",
                      static_cast<unsigned>(rxSummary.lastHeartbeatType),
                      static_cast<unsigned>(rxSummary.lastHeartbeatAutopilot),
                      static_cast<unsigned>(rxSummary.lastHeartbeatBaseMode),
                      static_cast<unsigned>(rxSummary.lastHeartbeatSystemStatus));
    }

    Serial.println();
    rxSummary = {};
}

}  // namespace

void setup() {
    Serial.begin(kUsbBaud);
    delay(500);
    Serial.println();
    Serial.println("ESP32-C3 MAVLink serial test starting");
    Serial.println("DroneBridge-compatible UART defaults: RX=GPIO4 TX=GPIO5 baud=57600 compid=158");
    Serial.printf("Wire FC TX -> ESP32 GPIO%d, FC RX -> ESP32 GPIO%d, baud=%lu\n",
                  kMavlinkRxPin,
                  kMavlinkTxPin,
                  static_cast<unsigned long>(kMavlinkBaud));
    Serial.printf("Local MAVLink identity sys=%u comp=%u\n",
                  static_cast<unsigned>(kLocalSystemId),
                  static_cast<unsigned>(kLocalComponentId));
    Serial.printf("Mission Planner test target should be sys=%u comp=%u command=%u\n",
                  static_cast<unsigned>(kLocalSystemId),
                  static_cast<unsigned>(kLocalComponentId),
                  static_cast<unsigned>(kTestCommandId));

    kFlightControllerSerial.begin(kMavlinkBaud, SERIAL_8N1, kMavlinkRxPin, kMavlinkTxPin);
    emitSerialHeartbeat();
    sendHeartbeat();
    sendStatusText();
    lastHeartbeatAtMs = millis();
    lastSerialHeartbeatAtMs = millis();
    lastStatusTextAtMs = millis();
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

    if (now - lastStatusTextAtMs >= kStatusTextIntervalMs) {
        sendStatusText();
        lastStatusTextAtMs = now;
    }
}