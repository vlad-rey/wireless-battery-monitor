#include "../src/drivers/xenta_protocol.h"

#include <cassert>
#include <cstdio>

int main() {
    xenta_protocol::BatteryReport report;
    assert(xenta_protocol::ParseBatteryReport({0x03, 0x50, 0x41, 0x01, 0x45}, report));
    assert(report.model == 0x50 && report.event == 0x41 && report.state == 0x01 && report.level == 69);

    assert(xenta_protocol::ParseBatteryReport({0x03, 0x30, 0x40, 0x01, 0x64}, report));
    assert(report.model == 0x30 && report.event == 0x40 && report.level == 100);
    assert(xenta_protocol::ParseBatteryReport({0x03, 0x3D, 0x40, 0x01, 0x32}, report));

    assert(!xenta_protocol::ParseBatteryReport({0x02, 0x50, 0x41, 0x01, 0x45}, report));
    assert(!xenta_protocol::ParseBatteryReport({0x03, 0x50, 0x01, 0x01, 0x45}, report));
    assert(!xenta_protocol::ParseBatteryReport({0x03, 0x50, 0x41, 0x01}, report));

    std::puts("xenta_protocol_test: ok");
    return 0;
}