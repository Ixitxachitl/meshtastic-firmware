#ifdef SENSECAP_INDICATOR

#include "FakeUART.h"
#include "configuration.h"
#include "mesh/IndicatorSerial.h"
#include "mesh/generated/meshtastic/interdevice.pb.h"
#include <cstring>

FakeUART *fakeUART = nullptr;

FakeUART::FakeUART()
{
    rx_head = 0;
    rx_tail = 0;
}

int FakeUART::available()
{
    if (rx_head >= rx_tail) {
        return rx_head - rx_tail;
    } else {
        return sizeof(rx_buffer) - rx_tail + rx_head;
    }
}

int FakeUART::read()
{
    if (available() == 0) {
        return -1;
    }

    uint8_t c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % sizeof(rx_buffer);
    return c;
}

int FakeUART::peek()
{
    if (available() == 0) {
        return -1;
    }
    return rx_buffer[rx_tail];
}

size_t FakeUART::write(uint8_t c)
{
    return write(&c, 1);
}

size_t FakeUART::write(const uint8_t *buffer, size_t size)
{
    return write((char *)buffer, size);
}

size_t FakeUART::write(char *buffer, size_t size)
{
    if (!sensecapIndicator || size == 0) {
        return 0;
    }

    // GPS writes NMEA sentences - send them to RP2040 via protobuf
    meshtastic_InterdeviceMessage message = meshtastic_InterdeviceMessage_init_default;

    // Limit size to fit in protobuf message
    if (size > sizeof(message.data.nmea) - 1) {
        size = sizeof(message.data.nmea) - 1;
    }

    memcpy(message.data.nmea, buffer, size);
    message.data.nmea[size] = '\0'; // Null terminate
    message.which_data = meshtastic_InterdeviceMessage_nmea_tag;

    // LOG_DEBUG("FakeUART::write(%s)", message.data.nmea); // Suppress verbose GPS NMEA logging

    sensecapIndicator->send_uplink(message);

    return size;
}

void FakeUART::flush()
{
    // No-op for FakeUART - there's no hardware buffer to flush
    // The protobuf messages are sent immediately in write()
}

void FakeUART::handleIncomingNMEA(const char *nmea)
{
    // Incoming NMEA from RP2040 - buffer it for GPS to read
    size_t len = strlen(nmea);

    for (size_t i = 0; i < len; i++) {
        rx_buffer[rx_head] = nmea[i];
        rx_head = (rx_head + 1) % sizeof(rx_buffer);

        // Check for buffer overflow
        if (rx_head == rx_tail) {
            // Buffer full, advance tail
            rx_tail = (rx_tail + 1) % sizeof(rx_buffer);
        }
    }

    // LOG_DEBUG("FakeUART buffered NMEA: %s", nmea); // Suppress verbose GPS NMEA logging
}

#endif
