#ifdef SENSECAP_INDICATOR

#include "IndicatorSerial.h"
#include "mesh/comms/FakeI2C.h"
#include "mesh/comms/FakeUART.h"
#include <HardwareSerial.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "configuration.h"

SensecapIndicator *sensecapIndicator = nullptr;

SensecapIndicator::SensecapIndicator(HardwareSerial &serial) : OSThread("SensecapIndicator")
{
    if (!running) {
        _serial = &serial;
        _serial->setRxBufferSize(PB_BUFSIZE);
        _serial->setPins(SENSOR_RP2040_RXD, SENSOR_RP2040_TXD);
        _serial->begin(SENSOR_BAUD_RATE);
        running = true;
        LOG_DEBUG("Start indicator communication thread");
    }
}

int32_t SensecapIndicator::runOnce()
{
    // Check for incoming protobuf messages from RP2040
    if (_serial->available() >= 4) {
        uint8_t magic[2];
        _serial->readBytes(magic, 2);

        if (magic[0] == PROTOBUF_MAGIC_BYTE1 && magic[1] == PROTOBUF_MAGIC_BYTE2) {
            uint8_t len_bytes[2];
            _serial->readBytes(len_bytes, 2);
            uint16_t payload_len = (len_bytes[0] << 8) | len_bytes[1];

            if (payload_len > 0 && payload_len < PB_BUFSIZE) {
                int bytes_read = _serial->readBytes(protobuf_buffer, payload_len);

                if (bytes_read == payload_len) {
                    meshtastic_InterdeviceMessage message = meshtastic_InterdeviceMessage_init_default;
                    pb_istream_t stream = pb_istream_from_buffer(protobuf_buffer, payload_len);

                    if (pb_decode(&stream, meshtastic_InterdeviceMessage_fields, &message)) {
                        // Handle incoming messages from RP2040
                        if (message.which_data == meshtastic_InterdeviceMessage_nmea_tag) {
                            // GPS NMEA data - forward to FakeUART which handles GPS
                            if (fakeUART) {
                                fakeUART->handleIncomingNMEA(message.data.nmea);
                            }
                        } else if (message.which_data == meshtastic_InterdeviceMessage_sensor_tag) {
                            // Handle sensor data including beep commands
                            auto &sensor = message.data.sensor;

                            LOG_DEBUG("Received sensor message: type=%d, which_data=%d", sensor.type, sensor.which_data);

                            if (sensor.type == meshtastic_MessageType_BEEP_ON) {
                                // RP2040 requesting ESP32 to beep (if we have local buzzer)
                                if (sensor.which_data == meshtastic_SensorData_uint32_value_tag) {
                                    LOG_DEBUG("Beep ON request from RP2040: %u ms", sensor.data.uint32_value);
                                } else {
                                    LOG_DEBUG("Beep ON request from RP2040 (no duration)");
                                }
                                // Could trigger local ESP32 buzzer here if desired
                            } else if (sensor.type == meshtastic_MessageType_BEEP_OFF) {
                                LOG_DEBUG("Beep OFF request from RP2040");
                                // Could stop local ESP32 buzzer here if desired
                            } else if (sensor.type == meshtastic_MessageType_ACK) {
                                LOG_DEBUG("ACK from RP2040");
                            } else {
                                LOG_DEBUG("Received sensor data from RP2040: type=%d", sensor.type);
                            }
                        }
                    } else {
                        LOG_WARN("Failed to decode protobuf message");
                    }
                }
            }
        }
    }

    return 100; // Check every 100ms
}

bool SensecapIndicator::send_uplink(meshtastic_InterdeviceMessage message)
{
    if (!_serial || !running) {
        return false;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(protobuf_buffer, sizeof(protobuf_buffer));

    if (!pb_encode(&stream, meshtastic_InterdeviceMessage_fields, &message)) {
        LOG_ERROR("Failed to encode protobuf message");
        return false;
    }

    size_t message_length = stream.bytes_written;

    // Send magic bytes + length + payload
    uint8_t header[4] = {PROTOBUF_MAGIC_BYTE1, PROTOBUF_MAGIC_BYTE2, (uint8_t)(message_length >> 8),
                         (uint8_t)(message_length & 0xFF)};

    _serial->write(header, 4);
    _serial->write(protobuf_buffer, message_length);
    _serial->flush();

    return true;
}

void SensecapIndicator::sendBeep(uint32_t packed_value)
{
    meshtastic_InterdeviceMessage message = meshtastic_InterdeviceMessage_init_zero;
    message.which_data = meshtastic_InterdeviceMessage_sensor_tag;
    message.data.sensor.type = meshtastic_MessageType_BEEP_ON;
    message.data.sensor.which_data = meshtastic_SensorData_uint32_value_tag;
    message.data.sensor.data.uint32_value = packed_value;

    bool is_queued = (packed_value & 0x80000000) != 0;
    uint16_t freq = (packed_value >> 16) & 0x7FFF; // Mask out queue bit
    uint16_t dur = packed_value & 0xFFFF;

    LOG_DEBUG("Sending BEEP_ON to RP2040: freq=%u Hz, dur=%u ms, queued=%d (packed=0x%08x)", freq, dur, is_queued, packed_value);

    if (!send_uplink(message)) {
        LOG_WARN("Failed to send BEEP_ON to RP2040");
    }
}

void SensecapIndicator::sendBeep(uint16_t frequency_hz, uint16_t duration_ms)
{
    // Manual beep (non-queued): no high bit set
    uint32_t packed = ((uint32_t)frequency_hz << 16) | duration_ms;
    sendBeep(packed);
}

void SensecapIndicator::stopBeep()
{
    meshtastic_InterdeviceMessage message = meshtastic_InterdeviceMessage_init_default;
    message.which_data = meshtastic_InterdeviceMessage_sensor_tag;
    message.data.sensor.type = meshtastic_MessageType_BEEP_OFF;

    if (send_uplink(message)) {
        LOG_DEBUG("Sent BEEP_OFF to RP2040");
    } else {
        LOG_WARN("Failed to send BEEP_OFF to RP2040");
    }
}

#endif
