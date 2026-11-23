// Alienware AW920H Tri-Mode Wireless Gaming Headset on its 2.4 GHz dongle (413C:A515).
// Protocol from Alienware Command Center: the packet log its AlienFX SubAgent writes and the
// GetRFBatteryStatus / IsChecksumOk methods of its AW920H plugin (HeadsetDevice.cs in
// AW920H_VID_413C_PID_A515.dll; it sends the same query once a minute). Checked on an AW920H:
//  * Collection: interface 5, usage page FF13:0001; output report 06 / input report 07, 62 bytes each.
//  * Request: HidD_SetOutputReport [06][C0][0A][00 ...].
//  * Reply, read with HidD_GetInputReport 07 every ~15 ms: [07][C0][0A][09][00][linked][percent]
//    [charging][critical][..][..][critical percent][charge mode][checksum]:
//    [13] = XOR of bytes 3..12, and Command Center also rejects a reply whose [6] is 0.
//    [7] = 1 while charging ([12] = 0 normal, 1 / 2 quick charge). [8] = 1 when the battery is
//    critical; the level is then taken from [11] instead of [6].
//    [07][00][80] means "no answer yet"; the dongle answers after 2-3 reads (30-100 ms) while the
//    headset is on, and keeps answering 00 80 while it is off.
//    Example: 07 C0 0A 09 00 01 5F 00 00 1C 10 5F 02 06 = 95%, not charging.
//  * About 1 query in 100 is dropped and never answered (measured: none of those answered within 3 s),
//    so the query is resent; Command Center likewise retries. A headset that still does not answer is
//    left out of the results so the tray keeps its last level for graceSec instead of flickering.
// In Bluetooth mode (413C:A514) Windows keeps the level itself, so PollBluetooth covers it.
#include "drivers.h"

#include <algorithm>

namespace {
constexpr USHORT kVendor = 0x413C, kAw920hDongle = 0xA515;
constexpr BYTE kRequestId = 0x06, kReplyId = 0x07;
constexpr BYTE kBatteryCmd0 = 0xC0, kBatteryCmd1 = 0x0A;
constexpr int kAttempts = 3;
constexpr int kAttemptMs = 180;     // a normal reply takes 30-100 ms
constexpr int kTotalTimeoutMs = 600;

bool IsVendorCollection(const HidColl& c) {
    return c.iface == 5 && c.page() == 0xFF13 && c.usage() == 0x0001 && c.inLen() == 62 && c.outLen() == 62;
}

struct Battery {
    int percent = -1;  // -1: no valid reply (headset off or out of range)
    bool charging = false;
};

// Decodes a battery reply the way Command Center does; false when `r` is not a valid one.
bool DecodeBattery(const std::vector<BYTE>& r, Battery& b) {
    if (r.size() < 14 || r[0] != kReplyId || r[1] != kBatteryCmd0 || r[2] != kBatteryCmd1 || r[3] != 0x09) return false;
    BYTE x = 0;
    for (int i = 3; i <= 12; i++) x ^= r[i];
    if (x != r[13] || r[6] == 0) return false;
    int pct = r[8] == 1 ? r[11] : r[6];
    if (pct > 100) return false;
    b.percent = pct;
    b.charging = r[7] == 1;
    return true;
}

Battery QueryBattery(HidDev& d) {
    std::vector<BYTE> req(62, 0);
    req[0] = kRequestId;
    req[1] = kBatteryCmd0;
    req[2] = kBatteryCmd1;
    // Sends the output report the way Command Center does (HidD_SetOutputReport). The reply is
    // normally fetched over the control pipe below, but take it if it also arrives as an interrupt report.
    std::vector<BYTE> r;
    Battery b;
    ULONGLONG deadline = GetTickCount64() + kTotalTimeoutMs;
    for (int attempt = 0; attempt < kAttempts && GetTickCount64() < deadline; attempt++) {
        if (d.SetOutputAndRead(req, r, 20) > 0 && DecodeBattery(r, b)) return b;
        ULONGLONG attemptEnd = (std::min)(deadline, GetTickCount64() + kAttemptMs);
        do {
            Sleep(15);
            if (d.GetInputReport(kReplyId, r) && DecodeBattery(r, b)) return b;
        } while (GetTickCount64() < attemptEnd);
    }
    return Battery{};
}
}  // namespace

void PollAlienware(PollContext& ctx) {
    if (ctx.IsClaimed(kVendor, kAw920hDongle)) return;
    for (const HidColl* c : ctx.Find(kVendor, kAw920hDongle, IsVendorCollection)) {
        HidDev d;
        if (!d.Open(*c)) continue;
        Battery b = QueryBattery(d);
        if (b.percent < 0) {
            // Headset off, out of range, or every query dropped: leave it out (the tray keeps the last
            // level for graceSec, then removes it) and keep the generic fallbacks off this dongle.
            ctx.Claim(kVendor, kAw920hDongle);
            break;
        }
        DeviceStatus s;
        s.key = L"alienware:" + Hex4(kAw920hDongle);
        s.name = L"Alienware AW920H";
        s.kind = DeviceKind::Headset;
        s.via = L"2.4 GHz dongle";
        s.percent = b.percent;
        s.charging = b.charging;
        s.graceSec = 180;
        ctx.Add(s, kVendor, kAw920hDongle);
        break;
    }
}
