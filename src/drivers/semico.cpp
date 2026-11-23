// Semico (VID 1A2C) tri-mode keyboards using the AA 55 CC 33 vendor HID protocol on interface 0,
// usage page FF02 / usage 0002 (65-byte input and output reports: report ID 0 + 64-byte payload).
// Covers Attack Shark X98 (2.4 GHz dongle 1A2C:8FFF, USB cable 1A2C:A034; tested on dongle) and
// AJAZZ AK820 Max Plus (1A2C:8FFF dongle, 1A2C:A036 cable).
//   request  [00 AA 55 CC 33 0E 00 ...]       read-only status query (0x0E)
//   reply    [AA 55 CC 33 0E percent state]   percent 0..100; state 0 = offline, 1..3 = online
// Hardware testing on the Attack Shark X98 over 2.4 GHz showed state=1 both while charging
// (75% -> 81% -> 88% -> 100%) and while unplugged on battery (96%), so charging is not inferred
// from state=1 to avoid a permanent false charging indicator on battery.
// Sources:
//  * https://github.com/shrey715/openajazz (PROTOCOL.md, openajazz/protocol.py)
//  * https://github.com/AkashPanja/Kraber (Battery/KeyboardBattery.cs)
//  * Live captures on Attack Shark X98 (1A2C:8FFF / 1A2C:A034, issue #265)
#include "drivers.h"

#include <algorithm>

namespace {

constexpr USHORT kVid = 0x1A2C;
constexpr USHORT kPidDongle = 0x8FFF;
constexpr USHORT kPidX98Cable = 0xA034;
constexpr USHORT kPidAk820Cable = 0xA036;

bool IsKnownPid(USHORT pid) {
    return pid == kPidDongle || pid == kPidX98Cable || pid == kPidAk820Cable;
}

bool IsCablePid(USHORT pid) {
    return pid == kPidX98Cable || pid == kPidAk820Cable;
}

const wchar_t* KnownModelName(USHORT pid) {
    if (pid == kPidX98Cable) return L"Attack Shark X98";
    if (pid == kPidAk820Cable) return L"AJAZZ AK820 Max Plus";
    return nullptr;
}

struct QueryResult {
    bool answered = false;
    bool online = false;
    int percent = -1;
};

QueryResult QueryStatus(HidDev& d) {
    std::vector<BYTE> req(65, 0);  // [0] = report ID 0
    req[1] = 0xAA;
    req[2] = 0x55;
    req[3] = 0xCC;
    req[4] = 0x33;
    req[5] = 0x0E;
    d.Flush();
    if (!d.Write(req, 500)) return {};
    ULONGLONG end = GetTickCount64() + 600;
    for (;;) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0) return {};
        std::vector<BYTE> r;
        int n = d.Read(r, left);  // report ID 0 stripped: r[0] is the first payload byte
        if (n < 0) return {};
        if (n < 7 || r[0] != 0xAA || r[1] != 0x55 || r[2] != 0xCC || r[3] != 0x33 || r[4] != 0x0E) continue;
        BYTE pct = r[5];
        BYTE state = r[6];
        if (state == 0) return { true, false, -1 };
        if (state > 3 || pct > 100) return {};
        return { true, true, (int)pct };
    }
}

std::wstring KeyboardName(const HidColl& c, USHORT knownCablePid) {
    if (const wchar_t* known = KnownModelName(c.pid)) return known;
    if (const wchar_t* known = KnownModelName(knownCablePid)) return known;
    std::wstring name = HidProduct(c);
    bool generic = name.empty() || name == L"2.4 Wireless Device" || name == L"USB Gaming Keyboard";
    return generic ? L"Wireless keyboard" : name;
}

}  // namespace

void PollSemico(PollContext& ctx) {
    auto products = ctx.Products(kVid);
    USHORT knownCablePid = 0;
    for (auto [vid, pid] : products)
        if (IsCablePid(pid)) knownCablePid = pid;

    // Poll dongle first so an active 2.4 GHz keyboard answers immediately even if the USB cable
    // is also plugged in for charging (the cable collection times out while switched to 2.4G).
    std::sort(products.begin(), products.end(), [](const auto& a, const auto& b) {
        return (a.second == kPidDongle) > (b.second == kPidDongle);
    });

    bool foundOnline = false;
    std::vector<USHORT> answeredOffline;

    for (auto [vid, pid] : products) {
        if (!IsKnownPid(pid)) continue;
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                 return c.iface == 0 && c.page() == 0xFF02 && c.usage() == 0x0002 &&
                        c.inLen() == 65 && c.outLen() == 65;
             })) {
            HidDev d;
            if (!d.Open(*c)) continue;
            QueryResult q = QueryStatus(d);
            if (!q.answered) continue;
            if (!q.online) {
                answeredOffline.push_back(pid);
                break;
            }
            DeviceStatus s;
            s.key = L"semico:1A2C";
            s.name = KeyboardName(*c, knownCablePid);
            s.kind = DeviceKind::Keyboard;
            s.via = IsCablePid(pid) ? L"USB cable" : L"2.4 GHz receiver";
            s.percent = q.percent;
            s.graceSec = 1800;  // keep last reading dimmed if the keyboard sleeps
            ctx.Add(s, vid, pid);
            foundOnline = true;
            break;
        }
        if (foundOnline) break;
    }

    if (foundOnline) {
        // Claim any sibling PID (e.g. cable plugged in for charging while switched to 2.4G).
        for (auto [vid, pid] : products)
            if (IsKnownPid(pid)) ctx.Claim(vid, pid);
    } else if (!answeredOffline.empty()) {
        DeviceStatus s;
        s.key = L"semico:1A2C";
        s.name = knownCablePid ? KnownModelName(knownCablePid) : L"Wireless keyboard";
        s.kind = DeviceKind::Keyboard;
        s.via = L"2.4 GHz receiver";
        s.connected = false;
        ctx.out.push_back(s);
        for (USHORT pid : answeredOffline) ctx.Claim(kVid, pid);
    }
}
