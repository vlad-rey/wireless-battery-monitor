// Switch Pro-compatible controllers (057E:2009), including the GameSir Tarantula Pro
// over its 2.4 GHz receiver in Switch mode.
// SDL's SDL_hidapi_switch.c documents byte 2 of full input report 0x30:
// bits 7..5 are the battery level (0..4) and bit 4 indicates charging.
// Steam Input or another host must enable full reports. The driver only listens.
#include "drivers.h"

void PollSwitchPro(PollContext& ctx) {
    constexpr USHORT vid = 0x057E;
    constexpr USHORT pid = 0x2009;
    static constexpr const wchar_t* levels[] = {
        L"Empty", L"Critical", L"Low", L"Medium", L"Full",
    };

    for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
             return c.page() == 0x01 && (c.usage() == 0x04 || c.usage() == 0x05) && c.inLen() >= 13;
         })) {
        HidDev d;
        if (!d.OpenReadOnly(*c)) continue;

        const ULONGLONG deadline = GetTickCount64() + 350;
        std::vector<BYTE> report;
        for (ULONGLONG now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
            const int size = d.Read(report, (int)(deadline - now));
            if (size < 0) break;
            if (size < 13 || report[0] != 0x30) continue;

            const int level = (report[2] & 0xE0) >> 5;
            if (level > 4) continue;

            // Use the HID path for identity, as in the PlayStation driver.
            UINT pathHash = 2166136261u;
            for (wchar_t ch : c->path) {
                pathHash ^= ch;
                pathHash *= 16777619u;
            }
            DeviceStatus s;
            s.key = L"switchpro:" + std::to_wstring(pathHash);
            s.name = HidProduct(*c);
            if (s.name.empty()) s.name = L"Nintendo Switch Pro Controller";
            s.kind = DeviceKind::Gamepad;
            s.via = L"Switch mode";
            s.percent = 25 * level;
            s.levelText = levels[level];
            s.charging = (report[2] & 0x10) != 0 && level < 4;
            s.graceSec = 300;
            ctx.Add(s, vid, pid);
            break;
        }
    }
}
