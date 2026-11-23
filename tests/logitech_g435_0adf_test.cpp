#include "../src/drivers/logitech_g435_0adf.h"

#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    // CRC-16/XMODEM check value.
    const std::uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    assert(G435Adf::Crc16(check, sizeof(check)) == 0x31C3);

    // Frame layout: cmd, body length, CRC (little endian) over the frame with the CRC zeroed.
    auto f = G435Adf::Frame(0x84, { 0x01 });
    assert(f.size() == 64 && f[0] == 0x84 && f[1] == 1 && f[4] == 0x01 && f[5] == 0);
    auto zeroed = f;
    zeroed[2] = zeroed[3] = 0;
    std::uint16_t crc = G435Adf::Crc16(zeroed.data(), zeroed.size());
    assert(f[2] == (crc & 0xFF) && f[3] == (crc >> 8));

    // GET_BATTERY command as sent by the original tool (sequence 5 after the init and mode steps).
    auto pa = G435Adf::Pa(0x06, 5, 1, { 0x00, 0x31 });
    assert((pa == std::vector<std::uint8_t>{ 0x50, 0x41, 0x06, 0x05, 0x01, 0x00, 0x31 }));

    // Battery reply (synthetic, built on the pattern the original tool matches): 3780 mV = 0x0EC4.
    std::vector<std::uint8_t> reply(64, 0);
    const std::uint8_t body[] = { 0x50, 0x49, 0x00, 0x07, 0x00, 0x06, 0x85, 0x00, 0xC4, 0x0E };
    for (size_t i = 0; i < sizeof(body); i++) reply[10 + i] = body[i];
    assert(G435Adf::ParseMillivolts(reply.data(), reply.size()) == 3780);

    // Implausible voltage (dongle answering itself) and missing reply are rejected.
    reply[18] = 0xD2;
    reply[19] = 0x00;
    assert(G435Adf::ParseMillivolts(reply.data(), reply.size()) == -1);
    std::vector<std::uint8_t> empty(64, 0);
    assert(G435Adf::ParseMillivolts(empty.data(), empty.size()) == -1);

    // Voltage curve.
    assert(G435Adf::Percent(4250) == 100 && G435Adf::Percent(4200) == 100);
    assert(G435Adf::Percent(3800) == 55 && G435Adf::Percent(3775) == 50);
    assert(G435Adf::Percent(3400) == 0 && G435Adf::Percent(3000) == 0);

    std::puts("logitech_g435_0adf_test: ok");
    return 0;
}
