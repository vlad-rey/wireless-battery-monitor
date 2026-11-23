// Niceboy MW400 wireless mouse on its 2.4 GHz "Wireless Receiver" (248A:FA02).
// Protocol from the Niceboy MW400 configuration app (Niceboy MW400.exe, WillGetBatteryThread and the
// RefreshBatteryStatus slot), its "Threadread bat" debug output, and replies read from an MW400:
//  * Collection: interface 0, the vendor collection with report 05 (usage 0001:0000, 33 bytes in and out).
//  * Request (interrupt OUT): [05][24][FA][48][00 ...]. The app sends it about every 27 s.
//  * Reply (input report 05): [05][xx][FA][48][lo][hi][charging][...]; the app only accepts [3] = 48.
//    raw = lo | hi << 8, millivolts = raw * 990 / 240. lo, hi and charging all 0: mouse not linked.
//  * Percent: the app's own table in 10% steps (>= 4200 mV 100%, 4100 90%, 4000 80%, 3900 70%,
//    3800 60%, 3700 50%, 3500 40%, 3400 30%, 3200 20%, else 10%).
//    Example: 05 49 FA 48 EA 03 00 = raw 1002 = 4133 mV = 90%, not charging.
// The 248A vendor id is shared by other Telink receivers (Machenike, Delux, Darmoshark, Ajazz), so only
// this exact product id is matched.
#include "drivers.h"

namespace {
constexpr USHORT kVendor = 0x248A, kMw400Receiver = 0xFA02;
constexpr BYTE kReport = 0x05;
constexpr int kReplyTimeoutMs = 500;

bool IsVendorCollection(const HidColl& c) {
    return c.iface == 0 && c.page() == 0x0001 && c.usage() == 0x0000 && c.inLen() == 33 && c.outLen() == 33;
}

int PercentFromMillivolts(int mv) {
    static const int kMv[] = { 4200, 4100, 4000, 3900, 3800, 3700, 3500, 3400, 3200 };
    static const int kPct[] = { 100, 90, 80, 70, 60, 50, 40, 30, 20 };
    for (size_t i = 0; i < sizeof(kMv) / sizeof(kMv[0]); i++)
        if (mv >= kMv[i]) return kPct[i];
    return 10;
}

bool IsBatteryReply(const std::vector<BYTE>& r) {
    return r.size() >= 7 && r[0] == kReport && r[2] == 0xFA && r[3] == 0x48;
}

// Waits for the battery reply, skipping any other report 05 traffic, until one absolute deadline.
bool ReadBatteryReply(HidDev& d, std::vector<BYTE>& r) {
    ULONGLONG end = GetTickCount64() + kReplyTimeoutMs;
    for (;;) {
        LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
        if (left <= 0 || d.Read(r, (int)left) < 0) return false;
        if (IsBatteryReply(r)) return true;
    }
}
}  // namespace

void PollNiceboy(PollContext& ctx) {
    if (ctx.IsClaimed(kVendor, kMw400Receiver)) return;
    for (const HidColl* c : ctx.Find(kVendor, kMw400Receiver, IsVendorCollection)) {
        HidDev d;
        if (!d.Open(*c)) continue;
        d.Flush();
        std::vector<BYTE> r;
        if (!d.Write({ kReport, 0x24, 0xFA, 0x48 }) || !ReadBatteryReply(d, r)) continue;
        DeviceStatus s;
        s.key = L"niceboy:" + Hex4(kMw400Receiver);
        s.name = L"Niceboy MW400";
        s.kind = DeviceKind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.graceSec = 1800;
        if ((r[4] | r[5] | r[6]) == 0) {
            s.asleep = true;  // receiver answered, mouse asleep or off: keep the last level
        } else {
            s.millivolts = (r[4] | (r[5] << 8)) * 990 / 240;
            s.percent = PercentFromMillivolts(s.millivolts);
            s.charging = r[6] != 0;
        }
        ctx.Add(s, kVendor, kMw400Receiver);
        break;
    }
}
