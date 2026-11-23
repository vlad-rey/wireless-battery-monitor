#include "../src/drivers/rapoo_protocol.h"

#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    // Captured on the user's VT3 Max Gen 2, receiver 24AE:1417, FF00:0002.
    const std::vector<std::uint8_t> captured = {
        0x07, 0x20, 0x03, 0x20, 0x03, 0x20, 0x03, 0x01, 0x19, 0x00,
        0x04, 0x84, 0x00, 0x17, 0x46, 0x00, 0x02, 0x03, 0xBC
    };
    Rapoo::Battery b;
    assert(Rapoo::DecodeBattery(captured.data(), captured.size(), b));
    assert(b.percent == 25 && !b.charging && b.mousePid == 0x4617);
    assert(b.connection == Rapoo::Connection::Receiver);

    // Also captured live after connecting the cable (before its percentage increased).
    const std::vector<std::uint8_t> capturedCable = {
        0x07, 0x22, 0x03, 0x20, 0x03, 0x20, 0x03, 0x02, 0x19, 0x00,
        0x04, 0x84, 0x00, 0x17, 0x46, 0x00, 0x02, 0x03, 0x00
    };
    assert(Rapoo::DecodeBattery(capturedCable.data(), capturedCable.size(), b));
    assert(b.percent == 25 && b.charging && b.mousePid == 0x4617);
    assert(b.connection == Rapoo::Connection::Cable);

    // Captured on a VT9 Gen 2, receiver 24AE:1414, FF00:0002 (16-byte input report).
    const std::vector<std::uint8_t> capturedVt9 = {
        0x07, 0x10, 0x03, 0x40, 0x06, 0x40, 0x06, 0x01,
        0x64, 0x00, 0x03, 0x01, 0x00, 0x14, 0x46, 0x01
    };
    assert(Rapoo::DecodeBattery(capturedVt9.data(), capturedVt9.size(), b));
    assert(b.percent == 100 && !b.charging && b.mousePid == 0x4614);
    assert(b.connection == Rapoo::Connection::Receiver);

    // Captured on the same VT9 Gen 2 over USB cable 24AE:4614, FF00:0002 (19-byte input report).
    const std::vector<std::uint8_t> capturedVt9Cable = {
        0x07, 0x12, 0x04, 0x80, 0x0C, 0x80, 0x0C, 0x02, 0x64, 0x00,
        0x03, 0x01, 0x00, 0x14, 0x46, 0x01, 0x02, 0x02, 0x00
    };
    assert(Rapoo::DecodeBattery(capturedVt9Cable.data(), capturedVt9Cable.size(), b));
    assert(b.percent == 100 && b.charging && b.mousePid == 0x4614);
    assert(b.connection == Rapoo::Connection::Cable);

    auto modern = captured;
    modern[1] = 0x22; // sensor bits must not affect the connection type
    modern[7] = 2;
    modern[8] = 100;
    assert(Rapoo::DecodeBattery(modern.data(), modern.size(), b));
    assert(b.charging && b.percent == 100 && b.connection == Rapoo::Connection::Cable);
    modern[1] = 0x21;
    modern[7] = 1;
    modern[8] = 0;
    assert(Rapoo::DecodeBattery(modern.data(), modern.size(), b));
    assert(b.percent == 0 && !b.charging && b.connection == Rapoo::Connection::Bluetooth);
    modern[1] = 0x23;
    assert(!Rapoo::DecodeBattery(modern.data(), modern.size(), b));

    std::vector<std::uint8_t> legacy = { 0xBB, 0xB0, 0x81, 0x20, 0x03, 1, 37 };
    const auto legacyProfile = Rapoo::Profile::LegacyVT3Pro;
    assert(!Rapoo::DecodeBattery(legacy.data(), legacy.size(), b));
    assert(!Rapoo::DecodeBattery(captured.data(), captured.size(), b, legacyProfile));
    assert(Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    assert(b.percent == 37 && !b.charging && b.mousePid == 0);
    assert(b.connection == Rapoo::Connection::Unknown);
    legacy[5] = 2;
    legacy[6] = 0x7F;
    assert(Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    assert(b.charging && b.percent == -1); // unknown must never become 100%
    legacy[6] = 100;
    assert(Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    assert(b.percent == 100 && b.charging);
    legacy[5] = 1;
    assert(Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    assert(!b.charging); // a previous charging reading must not leak into this one

    // VT1 Pro Max (24AE:1233 receiver / 24AE:4433 cable), USBPcap captures from issue #136.
    const std::vector<std::uint8_t> vt1ProMaxReceiver = { 0xBB, 0xB0, 0x71, 0x84, 0x03, 0x01, 0x64 };
    assert(Rapoo::DecodeBattery(vt1ProMaxReceiver.data(), vt1ProMaxReceiver.size(), b, legacyProfile));
    assert(b.percent == 100 && !b.charging);
    const std::vector<std::uint8_t> vt1ProMaxCable = { 0xBB, 0xB0, 0x71, 0x84, 0x03, 0x02, 0x5F };
    assert(Rapoo::DecodeBattery(vt1ProMaxCable.data(), vt1ProMaxCable.size(), b, legacyProfile));
    assert(b.percent == 95 && b.charging);

    for (std::size_t size = 0; size < 9; size++)
        assert(!Rapoo::DecodeBattery(captured.data(), size, b));
    for (std::size_t size = 0; size < 7; size++)
        assert(!Rapoo::DecodeBattery(legacy.data(), size, b, legacyProfile));
    assert(!Rapoo::DecodeBattery(nullptr, 19, b));
    // Short modern reports have no optional model ID.
    assert(Rapoo::DecodeBattery(captured.data(), 9, b));
    assert(b.mousePid == 0);

    for (unsigned state : { 0U, 3U, 255U }) {
        modern = captured;
        modern[7] = (std::uint8_t)state;
        assert(!Rapoo::DecodeBattery(modern.data(), modern.size(), b));
        legacy[5] = (std::uint8_t)state;
        assert(!Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    }
    for (unsigned percent : { 101U, 126U, 127U, 128U, 255U }) {
        modern = captured;
        modern[8] = (std::uint8_t)percent;
        assert(!Rapoo::DecodeBattery(modern.data(), modern.size(), b));
        legacy[5] = 1;
        legacy[6] = (std::uint8_t)percent;
        assert(!Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    }
    legacy = { 0xBC, 0xB0, 0x81, 0x20, 0x03, 1, 50, 0, 0, 0 };
    assert(!Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    legacy[0] = 0xBB;
    legacy[1] = 0;
    assert(!Rapoo::DecodeBattery(legacy.data(), legacy.size(), b, legacyProfile));
    modern = capturedCable;
    modern[8] = 0x7F;
    assert(!Rapoo::DecodeBattery(modern.data(), modern.size(), b)); // sentinel is legacy-only
    modern = captured;
    modern[0] = 0x08; // CH584 has a separate protocol; do not claim it as Nordic support
    assert(!Rapoo::DecodeBattery(modern.data(), modern.size(), b));
    std::puts("Rapoo protocol tests passed (captured frame, charging, bounds, malformed reports).");
}
