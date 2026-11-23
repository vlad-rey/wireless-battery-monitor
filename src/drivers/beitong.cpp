// BEITONG (Betop) KP20 / BTP-KP20 controller on its 2.4 GHz receiver (20BC:5128, XInput mode).
// XInput reports the receiver as a wired pad, so the battery is read from the vendor collection
// (usage page 00FF, usage 0003, 64-byte reports with report ID 02) using the same "get base info"
// request BEITONG iControl sends when its home page shows the battery.
//
// Every byte after the report ID is scrambled in both directions with a fixed nibble cipher:
//   enc(x) = S[x >> 4] << 4 | ((x & 0x0F) ^ 0x08),  S = 0 A 6 F 1 C 2 9 E 3 7 D 4 B 5 8
// Zero padding after the payload is sent as plain zeros.
//   Request (plain): 02 15                       -> on the wire: 02 AD 00 00 ...
//   Reply   (plain): 02 15 <battery 0..100> 00 <charge state>
// The receiver answers without iControl's config mode, so no ping or mode change is needed.
// Other frames on the same collection (02 25 key state, 02 21 sync) only appear while iControl is
// open and are skipped.
// Protocol: reverse engineered from BEITONG iControl 1.0.5 (kp20.js) and USB traffic of a
// BTP-KP20 receiver; the cipher was confirmed against 32,000+ decoded bytes with no mismatch.
#include "drivers.h"

namespace {

const USHORT kVid = 0x20BC;
const BYTE kReportId = 0x02;
const BYTE kGetBaseInfo = 0x15;  // command 5 (status), sub-command 1 (base info)

const BYTE kNibble[16] = { 0x0, 0xA, 0x6, 0xF, 0x1, 0xC, 0x2, 0x9, 0xE, 0x3, 0x7, 0xD, 0x4, 0xB, 0x5, 0x8 };

BYTE Enc(BYTE x) { return (BYTE)((kNibble[x >> 4] << 4) | ((x & 0x0F) ^ 0x08)); }

BYTE Dec(BYTE x) {
    BYTE hi = 0;
    for (BYTE i = 0; i < 16; i++)
        if (kNibble[i] == (x >> 4)) hi = i;
    return (BYTE)((hi << 4) | ((x & 0x0F) ^ 0x08));
}

bool IsKp20Receiver(USHORT pid) { return pid == 0x5128; }

}  // namespace

void PollBeitong(PollContext& ctx) {
    for (auto [vid, pid] : ctx.Products(kVid)) {
        if (!IsKp20Receiver(pid)) continue;
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                 return c.page() == 0x00FF && c.usage() == 0x0003 && c.outLen() >= 3 && c.inLen() >= 5;
             })) {
            HidDev d;
            if (!d.Open(*c)) continue;
            d.Flush();
            if (!d.Write({ kReportId, Enc(kGetBaseInfo) })) continue;
            std::vector<BYTE> r;
            bool found = false;
            ULONGLONG end = GetTickCount64() + 800;
            while (!found && (LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
                if (d.Read(r, 200) < 5) continue;
                found = r[0] == kReportId && Dec(r[1]) == kGetBaseInfo;
            }
            if (!found) continue;
            BYTE level = Dec(r[2]);
            if (level > 100) continue;
            DeviceStatus s;
            s.key = L"beitong:" + Hex4(pid);
            s.name = L"BEITONG KP20";
            s.kind = DeviceKind::Gamepad;
            s.via = L"2.4 GHz receiver";
            s.percent = level;
            s.charging = Dec(r[4]) == 1;
            ctx.Add(s, vid, pid);
            break;
        }
    }
}
