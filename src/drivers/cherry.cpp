// CHERRY MX 3.0S Wireless on its 2.4 GHz dongle (USB 046A:00EA, "MX 3.0S Dongle") or on the USB
// cable (046A:00E3, "MX 3.0S Wireless Keyboard"). Over Bluetooth Windows' own level is used instead.
//
// Both expose a 64-byte vendor channel on usage page FF1C, usage 0092 (a second FF1C:0092
// collection with 9-byte reports is not used). The framing and the battery command come from the
// community tool B1Chat/BatteryWatch (MIT), which verified them on the CHERRY PIXIU 75 receiver
// (046A:01E4); the CHERRY MX 3.0S Wireless uses the same usage page, usage and report size.
//
//   frame:  [0] report ID 4, [1..2] checksum (LE) = sum(bytes[3..end]) & 0xFFFF, [3] type, [4..] payload
//   query:  type 0x1A, payload 03 00 00 55 (read-only status request)
//   reply:  [0] 4, [3] 0x1A, [8] battery percent, [9] non-zero while USB powered
//
// Verified on a real MX 3.0S Wireless through the dongle. The cable (00E3) uses the same channel layout
// and is assumed to answer the same query. The matching and the reply checks below are deliberately
// strict, so a device that answers differently is skipped rather than misreported.
#include "drivers.h"

namespace {

constexpr USHORT kVid = 0x046A;  // CHERRY
constexpr USHORT kPage = 0xFF1C;
constexpr USHORT kUsage = 0x0092;
constexpr size_t kLen = 64;  // report length including the report ID
constexpr BYTE kReportId = 0x04;
constexpr BYTE kStatusType = 0x1A;

struct Model {
    USHORT pid;
    const wchar_t* via;
};
// Cable first: when it is plugged in the keyboard is powered and awake, and the dongle may still be
// attached too. Only one entry is ever reported for the keyboard.
constexpr Model kModels[] = { { 0x00E3, L"USB cable" }, { 0x00EA, L"2.4 GHz receiver" } };

// The one and only frame this driver ever sends.
std::vector<BYTE> BuildQuery() {
    std::vector<BYTE> f(kLen, 0);
    f[0] = kReportId;
    f[3] = kStatusType;
    f[4] = 0x03;
    f[7] = 0x55;
    unsigned sum = 0;
    for (size_t i = 3; i < f.size(); ++i) sum += f[i];
    f[1] = (BYTE)(sum & 0xFF);
    f[2] = (BYTE)((sum >> 8) & 0xFF);
    return f;
}

bool IsStatusReply(const std::vector<BYTE>& r) { return r.size() >= 10 && r[0] == kReportId && r[3] == kStatusType; }

}  // namespace

// Sends the status query on one collection. Returns false when nothing usable answered.
static bool Query(const HidColl& c, std::vector<BYTE>& reply) {
    HidDev d;
    if (!d.Open(c)) return false;
    d.Flush();
    const std::vector<BYTE> query = BuildQuery();

    // The dongle only answers while the keyboard is awake: two tries, well under a second in all.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!d.Write(query, 300)) return false;
        const ULONGLONG end = GetTickCount64() + 400;
        for (;;) {
            const LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
            if (left <= 0) break;
            std::vector<BYTE> r;
            if (d.Read(r, (int)left) < 0) break;
            if (IsStatusReply(r)) {
                reply = std::move(r);
                return true;
            }
        }
    }
    return false;
}

void PollCherry(PollContext& ctx) {
    for (const Model& m : kModels) {
        if (ctx.IsClaimed(kVid, m.pid)) continue;
        for (const HidColl* c : ctx.Find(kVid, m.pid, [](const HidColl& c) {
                 return c.page() == kPage && c.usage() == kUsage && c.inLen() == kLen && c.outLen() == kLen;
             })) {
            std::vector<BYTE> reply;
            if (!Query(*c, reply)) continue;  // keyboard off or asleep: show nothing (last value is kept for a while)

            DeviceStatus s;
            s.key = L"cherry:mx30s";  // same entry whichever way the keyboard is connected
            s.name = L"CHERRY MX 3.0S Wireless";
            s.kind = DeviceKind::Keyboard;
            s.via = m.via;
            if (reply[8] <= 100) {
                s.percent = reply[8];
                s.charging = reply[9] != 0;  // USB powered, i.e. plugged in
            } else {
                s.asleep = true;  // answered without a usable level
            }
            s.graceSec = 1800;  // the dongle stops answering while the keyboard sleeps
            ctx.Add(s, kVid, m.pid);
            return;
        }
    }
}
