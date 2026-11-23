#include "xenta_protocol.h"

namespace xenta_protocol {

bool ParseBatteryReport(const Bytes& bytes, BatteryReport& out) {
    if (bytes.size() < 5 || bytes[0] != 0x03 || (bytes[2] != 0x40 && bytes[2] != 0x41)) return false;
    out.model = bytes[1];
    out.event = bytes[2];
    out.state = bytes[3];
    out.level = bytes[4];
    return true;
}

}  // namespace xenta_protocol