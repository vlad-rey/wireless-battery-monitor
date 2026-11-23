// Linux driver orchestration.
//
// Deliberately separate from src/devices.cpp so the Windows build is untouched. This first phase
// enables only the well-known vendors whose drivers do not depend on Windows-specific device
// paths. Add more Poll* calls here (and the matching .cpp to Makefile.linux) in later phases.
#include "../devices.h"

#include <cstdio>

#include "../drivers/drivers.h"
#include "../hid.h"

std::vector<DeviceStatus> PollAllDevices() {
    PollContext ctx;
    ctx.colls = EnumHid();

    // Phase 1: famous vendors, HID only.
    PollGlorious(ctx);       // also G-Wolves / Hitscan (defines the shared G-Wolves model table)
    PollKysona(ctx);         // also Pulsar, ATK/VXE and other popular Chinese mice
    PollLogitech(ctx);       // Unifying / Bolt / Lightspeed / Bluetooth HID++
    PollRazer(ctx);
    PollSteelSeries(ctx);
    PollCorsair(ctx);
    PollHyperX(ctx);
    PollPlayStation(ctx);    // DualShock 4 / DualSense over USB or Bluetooth HID
    PollCoolerMaster(ctx);
    PollFinalmouse(ctx);     // UltralightX / Starlight X dongles
    return ctx.out;
}

std::wstring DumpHidCollections() {
    std::wstring out = L"VID:PID    IF  UsagePg:Usage  In  Out Feat  Product\n";
    for (const auto& c : EnumHid()) {
        wchar_t line[256];
        swprintf(line, 256, L"%04X:%04X  %2d  %04X:%04X    %3u %3u %3u   ", c.vid, c.pid, c.iface,
                 c.page(), c.usage(), c.inLen(), c.outLen(), c.featLen());
        out += line + HidProduct(c) + L"\n";
    }
    return out;
}
