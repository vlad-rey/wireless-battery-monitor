// MSI wireless mice on their 2.4 GHz dongles.
//
// Protocol (taken from MSI Center's own Gaming Gear module, checked against real mice):
//   Output report (report ID 0, 64-byte payload zero-padded): [00][0F 20 00 ...]
//   Input reply (after stripping the leading 0 report ID):
//     [0] = 0F, [1] = 20, [14] = battery: bit 7 = charging, bits 0..6 = percent (0..100)
// A level of 0 while not charging is treated as "asleep" and the last known value is kept: MSI
// Center's log shows readings of 0 between normal ones, most likely while the mouse is off or asleep.
// MSI Center also sends 41 80 ("remote control") first. That switches the mouse into a software
// control mode, so it is deliberately not sent here: the 0F 20 query is enough.
// Unknown requests are answered with FF AA 00 00 plus an echo of the request, so a dongle that is
// never asked (see the passive GM41 below) stays silent.
//
// VERSA 300 ELITE WIRELESS (0DB0:9E22) and CLUTCH GM31 Lightweight Wireless (receiver 0DB0:0D3C,
// USB cable 0DB0:0D3B) both answer the query above, on collection FF0D:00B0 and FF00:0001.
// The GM31 was confirmed against MSI Center on a live mouse: the pushed frame
// 0F 20 00 00 23 05 7A 91 00 00 00 00 05 80 56 xx reads 0x56 = 86 %, the level MSI Center shows.
//
// CLUTCH GM41 Lightweight Wireless (receiver 0DB0:0D4C, USB cable 0DB0:0D4B) has no query: its
// collection (FF10:0006) only pushes A0 <raw> 07 <flags> 94 00 94 while the mouse moves, and <raw>
// falls as the battery drains. Sources: DARL0K/Wireless-Device-Battery-Monitor PR #1 and
// liamgibsonn/mouse-protocol PR #1 (USBPcap captures of MSI Center), so both GM41 ids and the
// cable ids here are untested.
#include "drivers.h"

namespace {

constexpr USHORT kMsiVid = 0x0DB0;

// Query: the dongle is asked for a status frame. Passive: the dongle pushes and cannot be asked.
enum class Mode { Query, Passive };

struct Model {
    USHORT pid;
    bool cable;
    Mode mode;
    USHORT page;
    USHORT usage;
    const wchar_t* name;
};

const Model kModels[] = {
    { 0x9E22, false, Mode::Query,   0xFF0D, 0x00B0, L"MSI VERSA 300 ELITE WIRELESS" },
    { 0x0D3C, false, Mode::Query,   0xFF00, 0x0001, L"MSI Clutch GM31 Lightweight Wireless" },
    { 0x0D3B, true,  Mode::Query,   0xFF00, 0x0001, L"MSI Clutch GM31 Lightweight Wireless" },
    { 0x0D4C, false, Mode::Passive, 0xFF10, 0x0006, L"MSI Clutch GM41 Lightweight Wireless" },
    { 0x0D4B, true,  Mode::Passive, 0xFF10, 0x0006, L"MSI Clutch GM41 Lightweight Wireless" },
};

bool Matches(const Model& m, const HidColl& c) {
    if (c.iface != 1 || c.page() != m.page || c.usage() != m.usage) return false;
    return m.mode == Mode::Query ? c.outLen() >= 65 && c.inLen() >= 65 : c.inLen() >= 7;
}

bool QueryStatus(const HidColl& c, int& percent, bool& charging) {
    HidDev d;
    if (!d.Open(c)) return false;
    d.Flush();
    if (!d.Write({ 0x00, 0x0F, 0x20 })) return false;

    std::vector<BYTE> r;
    ULONGLONG end = GetTickCount64() + 1000;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        int n = d.Read(r, left);
        if (n < 0) break;
        if (n >= 15 && r[0] == 0x0F && r[1] == 0x20) {
            int level = r[14] & 0x7F;
            if (level > 100) return false;
            percent = level;
            charging = (r[14] & 0x80) != 0;
            return true;
        }
    }
    return false;
}

// Linear fit through (0x4D, 57 %), (0x48, 54 %) and (0x45, 53 %): about 0.51 % per step.
int Gm41RawToPercent(int raw) { return Clamp100((raw * 51 + 1760 + 50) / 100); }

std::wstring Hex2(BYTE v) {
    const wchar_t* d = L"0123456789ABCDEF";
    return { d[v >> 4], d[v & 15] };
}

// The GM41 only reports while the mouse is moving, so this waits a little longer than the rules
// allow for a query; a sleeping mouse simply yields nothing and the app keeps its last value.
bool ListenGm41(const HidColl& c, int& percent, int& raw, BYTE& flags) {
    HidDev d;
    if (!d.OpenReadOnly(c)) return false;

    std::vector<BYTE> r;
    ULONGLONG end = GetTickCount64() + 2000;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        int n = d.Read(r, left);
        if (n < 0) break;
        if (n < 7 || r[0] != 0xA0 || r[2] != 0x07) continue;
        raw = r[1];
        flags = r[3];
        percent = Gm41RawToPercent(raw);
        return true;
    }
    return false;
}

}  // namespace

void PollMsi(PollContext& ctx) {
    if (ctx.IsClaimed(kMsiVid, 0x9E22)) return;
    for (const Model& m : kModels) {
        if (ctx.IsClaimed(kMsiVid, m.pid)) continue;
        for (const HidColl* c : ctx.Find(kMsiVid, m.pid, [&m](const HidColl& c) { return Matches(m, c); })) {
            int pct = -1;
            int raw = 0;
            BYTE flags = 0;
            bool chg = false;
            bool ok = m.mode == Mode::Query ? QueryStatus(*c, pct, chg) : ListenGm41(*c, pct, raw, flags);
            if (!ok) break;

            DeviceStatus s;
            s.key = L"msi:" + Hex4(m.pid);
            s.name = m.name;  // the Clutch dongles report a serial number as their product string
            s.kind = DeviceKind::Mouse;
            s.via = m.cable ? L"USB cable" : L"2.4 GHz receiver";
            if (m.mode == Mode::Passive) s.detail = L"approximate (raw 0x" + Hex2((BYTE)raw) + L", flags 0x" + Hex2(flags) + L")";
            if (pct == 0 && !chg) {
                s.asleep = true;
            } else {
                s.percent = pct;
                s.charging = chg;
            }
            s.graceSec = 1800;
            ctx.Add(s, kMsiVid, m.pid);
            break;
        }
    }
}
