// MCHOSE wireless headsets on the "MC" protocol (VID 3837). Currently supports the MCHOSE V9 Pro 2
// (2.4 GHz dongle 3837:6011) on vendor collection FF07:0222 (report ID 0x55, 63-byte input/output reports).
// The MCHOSE R9 (3837:6013) uses the same protocol in M HUB but is not enabled here because it is untested.
//
// Protocol from MCHOSE's own M HUB driver (cdn.mchose.com.cn customPage bundle: buildMCFrame,
// createReadBatteryCommand, parseBatteryResponse; MC_HEADSET_REPORT_ID = 85), verified on a V9 Pro 2:
//   Frame (after the report ID): [0] AA start, [1] 01 version, [2] flags (1 = XOR checksum), [3] length
//   (sub-command + body), [4..5] message type (00 01 request/response, 00 04 notify), [6] sub-command,
//   [7..] body, then one checksum byte (0 when flags is 0).
//   Read battery: [55][AA 01 00 01 00 01 0B 00 ...] (read-only). The reply, and the headset's own
//   battery notification (sub-command 0x22), carry body[0] = 0 when the headset is off or asleep,
//   otherwise body[1] = charging (bit 7) | percent (bits 0..6).
#include "drivers.h"

namespace {

constexpr USHORT kMchoseVid = 0x3837;
constexpr USHORT kV9Pro2Pid = 0x6011;
constexpr BYTE kReportId = 0x55;
constexpr BYTE kStart = 0xAA;
constexpr BYTE kVersion = 0x01;
constexpr BYTE kReadBattery = 0x0B;
constexpr BYTE kNotifyBattery = 0x22;

enum class Reply { None, Off, Level };

// Checks one input report (r[0] = report ID) for an MC battery reply or battery notification.
Reply ParseBattery(const std::vector<BYTE>& r, int& percent, bool& charging) {
    if (r.size() < 9 || r[0] != kReportId) return Reply::None;
    const BYTE* f = r.data() + 1;
    size_t n = r.size() - 1;
    if (f[0] != kStart || f[1] != kVersion || f[4] != 0x00) return Reply::None;
    bool reply = f[5] == 0x01 && f[6] == kReadBattery;
    bool notify = f[5] == 0x04 && f[6] == kNotifyBattery;
    if (!reply && !notify) return Reply::None;

    size_t len = f[3];  // sub-command + body
    if (len < 3 || 6 + len >= n) return Reply::None;
    BYTE check = 0;
    if (f[2] == 1) {
        check = f[2] ^ f[3] ^ f[4] ^ f[5];
        for (size_t i = 0; i < len; i++) check ^= f[6 + i];
    }
    if (f[6 + len] != check) return Reply::None;

    const BYTE* body = f + 7;
    if (body[0] == 0) return Reply::Off;
    percent = body[1] & 0x7F;
    charging = (body[1] & 0x80) != 0;
    return percent <= 100 ? Reply::Level : Reply::None;
}

Reply QueryV9Pro2(const HidColl& c, int& percent, bool& charging) {
    HidDev d;
    if (!d.Open(c)) return Reply::None;
    d.Flush();
    // Request frame: start, version, flags 0, length 1, type 00 01, sub-command, checksum 0.
    if (!d.Write({ kReportId, kStart, kVersion, 0x00, 0x01, 0x00, 0x01, kReadBattery, 0x00 })) return Reply::None;

    std::vector<BYTE> r;
    ULONGLONG end = GetTickCount64() + 1000;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (d.Read(r, left) < 0) break;
        Reply got = ParseBattery(r, percent, charging);
        if (got != Reply::None) return got;
    }
    return Reply::None;
}

}  // namespace

void PollMchoseHeadset(PollContext& ctx) {
    if (ctx.IsClaimed(kMchoseVid, kV9Pro2Pid)) return;
    for (const HidColl* c : ctx.Find(kMchoseVid, kV9Pro2Pid, [](const HidColl& c) {
             return c.page() == 0xFF07 && c.usage() == 0x0222 && c.outLen() >= 64 && c.inLen() >= 64;
         })) {
        int pct = -1;
        bool chg = false;
        Reply got = QueryV9Pro2(*c, pct, chg);
        if (got == Reply::None) continue;

        DeviceStatus s;
        s.key = L"mchose:v9pro2";
        s.name = HidProduct(*c);
        if (s.name.empty()) s.name = L"MCHOSE V9 Pro 2";
        s.kind = DeviceKind::Headset;
        s.via = L"2.4 GHz receiver";
        if (got == Reply::Off) {
            s.asleep = true;  // the dongle answered, the headset is off or asleep: keep the last value
        } else {
            s.percent = pct;
            s.charging = chg;
        }
        s.graceSec = 1800;
        ctx.Add(s, kMchoseVid, kV9Pro2Pid);
        break;
    }
}
