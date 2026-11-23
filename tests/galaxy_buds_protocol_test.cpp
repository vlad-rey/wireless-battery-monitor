#include "../src/drivers/galaxy_buds_protocol.h"

#include <cassert>
#include <cstdio>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

// Builds a frame the way GalaxyBudsClient's SppMessage.Encode does.
Bytes Frame(std::uint8_t id, const Bytes& payload, bool legacy = false) {
    Bytes body{ id };
    body.insert(body.end(), payload.begin(), payload.end());
    std::uint16_t crc = GalaxyBuds::Crc16(body.data(), body.size());
    std::size_t size = body.size() + 2;
    Bytes f = legacy ? Bytes{ 0xFE, 0x00, (std::uint8_t)size } : Bytes{ 0xFD, (std::uint8_t)size, (std::uint8_t)(size >> 8) };
    f.insert(f.end(), body.begin(), body.end());
    f.push_back((std::uint8_t)crc);
    f.push_back((std::uint8_t)(crc >> 8));
    f.push_back(legacy ? 0xEE : 0xDD);
    return f;
}

int main() {
    // EXTENDED_STATUS_UPDATED frames from GalaxyBudsClient.Tests/TestData/ExtendedStatusUpdate,
    // with the levels its decoder tests expect.
    const Bytes buds2Pro = {
        0xFD, 0x31, 0x00, 0x61, 0x0D, 0x04, 0x64, 0x64, 0x01, 0x00, 0x33, 0x58, 0x00, 0x01, 0xBF, 0x22, 0x00, 0x01,
        0x46, 0x01, 0x46, 0x01, 0x00, 0x00, 0x03, 0x35, 0x00, 0x03, 0x00, 0x10, 0x01, 0x01, 0x01, 0x01, 0x32, 0x03,
        0x01, 0x01, 0x01, 0x00, 0x0C, 0xAE, 0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x01, 0x01, 0xB6, 0x37, 0xDD
    };
    const Bytes budsPlus = {
        0xFD, 0x1B, 0x10, 0x61, 0x0D, 0x00, 0x57, 0x54, 0x01, 0x00, 0x11, 0x65, 0x00, 0x00, 0x00, 0x03,
        0x00, 0x22, 0x01, 0x04, 0x01, 0x04, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x1E, 0x55, 0xDD
    };
    const Bytes budsFe = {
        0xFD, 0x31, 0x10, 0x61, 0x02, 0x06, 0x3A, 0x59, 0x01, 0x00, 0x11, 0x00, 0x00, 0x00, 0x8F, 0x33, 0x00, 0x00,
        0x4A, 0x01, 0x4A, 0x01, 0x03, 0x00, 0x04, 0x65, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x01, 0x01, 0x11, 0x03,
        0x00, 0x00, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x9D, 0x0E, 0x1D, 0x00, 0x50, 0x00, 0xC2, 0xCF, 0xDD
    };
    const Bytes budsOriginal = {
        0xFE, 0x01, 0x12, 0x61, 0x03, 0x00, 0x46, 0x5F, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x01, 0x07, 0x00, 0x22, 0x01, 0xA5, 0x75, 0xEE
    };

    GalaxyBuds::Status s;
    assert(GalaxyBuds::FindStatus(buds2Pro.data(), buds2Pro.size(), false, s));
    assert(s.left == 100 && s.right == 100 && s.caseLevel == 88);
    assert(GalaxyBuds::FindStatus(budsPlus.data(), budsPlus.size(), false, s));
    assert(s.left == 87 && s.right == 84 && s.caseLevel == -1);  // 101 = case level unknown
    assert(GalaxyBuds::FindStatus(budsFe.data(), budsFe.size(), false, s));
    assert(s.left == 58 && s.right == 89 && s.caseLevel == -1);  // 0 = case closed / unknown
    assert(GalaxyBuds::FindStatus(budsOriginal.data(), budsOriginal.size(), true, s));
    assert(s.left == 70 && s.right == 95 && s.caseLevel == -1);  // the original Buds report no case level
    assert(!GalaxyBuds::FindStatus(budsOriginal.data(), budsOriginal.size(), false, s));  // wrong framing

    // A corrupted byte fails the checksum.
    Bytes bad = buds2Pro;
    bad[6] = 0x50;
    assert(!GalaxyBuds::FindStatus(bad.data(), bad.size(), false, s));

    // STATUS_UPDATED (revision, L, R, coupled, main connection, placement, case) after the extended
    // status in one stream, with noise in between: the newest frame wins.
    Bytes stream = { 0x00, 0xFD, 0x12 };
    stream.insert(stream.end(), buds2Pro.begin(), buds2Pro.end());
    Bytes other = Frame(0x40, { 0x01, 0x02 });  // some other message
    stream.insert(stream.end(), other.begin(), other.end());
    Bytes update = Frame(0x60, { 0x0D, 0x4B, 0x00, 0x01, 0x00, 0x13, 0x2A, 0x00 });
    stream.insert(stream.end(), update.begin(), update.end());
    assert(GalaxyBuds::FindStatus(stream.data(), stream.size(), false, s));
    assert(s.left == 75 && s.right == -1 && s.caseLevel == 42);  // right earbud not connected

    // An incomplete frame (still arriving) is not decoded yet.
    assert(!GalaxyBuds::FindStatus(update.data(), update.size() - 1, false, s));
    // Fragments are skipped.
    Bytes frag = update;
    frag[2] |= 0x20;
    assert(!GalaxyBuds::FindStatus(frag.data(), frag.size(), false, s));

    std::puts("galaxy_buds_protocol_test: ok");
    return 0;
}
