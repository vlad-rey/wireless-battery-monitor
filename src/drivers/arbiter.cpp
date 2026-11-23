// Arbiter Studio Akitsu wireless mice on the shared DriverCore vendor id 373E:
//   * Akitsu (PAW3395):        0009 USB cable, 000A 8K receiver
//   * Akitsu 3950 (PAW3950):   0038 USB cable, 0039 8K receiver
//   * Akitsu Medium (PAW3950): 0066 USB cable, 0067 8K receiver (both verified on hardware)
// Runs before PollLamzu, which scans all unclaimed 373E products with the same DriverCore query and
// would otherwise report these mice as "LAMZU wireless mouse".
// Protocol and model ids from Arbiter Web HUB (webapp.arbiterstudio.com/Config/env-models.json,
// "IsNewProtocol" getBatPer() in the DriverCore bundle; see also lamzu.cpp and wlmouse.cpp):
//   Vendor collection FFFF:0000 (65-byte feature report 0):
//   SET_FEATURE [00][00][00][02][02][00][83][00 ...], wait ~100 ms, GET_FEATURE report 0 ->
//   [00][status][..][..][02][..][83][charging][percent] (or without the leading report id byte).
//   status A1 / A2 = answered, A0 = mouse asleep; charging 1 = on the cable; percent 0 on a receiver
//   means the mouse is switched off or not linked. Read-only: no settings or mode writes are sent.
#include "drivers.h"

#include <algorithm>

namespace {

constexpr USHORT kVid = 0x373E;

// Cables come before receivers: when both the USB cable and the 8K receiver are plugged in, an
// answering cable claims its paired receiver so the mouse is listed once.
struct Model { USHORT pid; bool receiver; const wchar_t* name; USHORT receiverPid; };
const Model kModels[] = {
    { 0x0009, false, L"Arbiter Studio Akitsu", 0x000A },
    { 0x0038, false, L"Arbiter Studio Akitsu 3950", 0x0039 },
    { 0x0066, false, L"Arbiter Studio Akitsu Medium", 0x0067 },
    { 0x000A, true, L"Arbiter Studio Akitsu", 0 },
    { 0x0039, true, L"Arbiter Studio Akitsu 3950", 0 },
    { 0x0067, true, L"Arbiter Studio Akitsu Medium", 0 },
};

enum class Reply { Battery, Asleep, None };

Reply Decode(const std::vector<BYTE>& r, int& pct, bool& charging) {
    BYTE status = 0, chg = 0, level = 0;
    if (r.size() >= 9 && r[4] == 0x02 && r[6] == 0x83) {
        status = r[1];
        chg = r[7];
        level = r[8];
    } else if (r.size() >= 8 && r[3] == 0x02 && r[5] == 0x83) {
        status = r[0];
        chg = r[6];
        level = r[7];
    } else {
        return Reply::None;
    }
    if (status == 0xA0) return Reply::Asleep;
    if (status != 0xA1 && status != 0xA2) return Reply::None;
    if (level > 100) return Reply::None;
    pct = level;
    charging = chg == 0x01;
    return Reply::Battery;
}

Reply Query(HidDev& d, int& pct, bool& charging) {
    std::vector<BYTE> req(65, 0);  // [0] = report id 0
    req[3] = 0x02;
    req[4] = 0x02;
    req[6] = 0x83;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!d.SetFeature(req)) return Reply::None;
        for (int read = 0; read < 3; ++read) {
            Sleep(read == 0 ? 100 : 60);
            std::vector<BYTE> r;
            if (!d.GetFeature(0x00, r)) return Reply::None;
            Reply rep = Decode(r, pct, charging);
            if (rep != Reply::None) return rep;
        }
    }
    return Reply::None;
}

}  // namespace

void PollArbiter(PollContext& ctx) {
    for (const Model& m : kModels) {
        if (ctx.IsClaimed(kVid, m.pid)) continue;
        auto colls = ctx.Find(kVid, m.pid, [](const HidColl& c) { return c.page() == 0xFFFF && c.featLen() == 65; });
        if (colls.empty()) continue;
        std::stable_sort(colls.begin(), colls.end(), [](const HidColl* a, const HidColl* b) {
            return (a->usage() == 0) > (b->usage() == 0);
        });
        bool handled = false;
        for (const HidColl* c : colls) {
            HidDev d;
            if (!d.OpenFeatureOnly(*c)) continue;
            int pct = -1;
            bool charging = false;
            Reply rep = Query(d, pct, charging);
            if (rep == Reply::None) continue;
            DeviceStatus s;
            // Share one key between cable and receiver so plugging in the cable updates the existing
            // entry in place instead of leaving the receiver entry dimmed for graceSec.
            USHORT keyPid = m.receiverPid ? m.receiverPid : m.pid;
            s.key = L"arbiter:" + Hex4(kVid) + L":" + Hex4(keyPid);
            s.name = m.name;
            s.kind = DeviceKind::Mouse;
            s.via = m.receiver ? L"2.4 GHz receiver" : L"USB cable";
            if (m.receiver || !ctx.Find(kVid, m.receiverPid, [](const HidColl& col) {
                    return col.page() == 0xFFFF && col.featLen() == 65;
                }).empty()) {
                s.graceSec = 1800;
            }
            if (rep == Reply::Asleep) {
                s.asleep = true;
            } else {
                s.connected = pct > 0 || !m.receiver;  // 0 from a dongle = mouse off or out of range
                if (s.connected) s.percent = pct;
                s.charging = charging;
            }
            ctx.Add(s, kVid, m.pid);
            if (m.receiverPid) ctx.Claim(kVid, m.receiverPid);
            handled = true;
            break;
        }
        // Claim present Akitsu hardware even when silent so PollLamzu does not mislabel it on 373E,
        // and so the app keeps the last reading across sleep via graceSec.
        if (!handled) ctx.Claim(kVid, m.pid);
    }
}
