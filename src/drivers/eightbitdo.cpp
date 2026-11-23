// 8BitDo wireless controllers (VID 2DC8):
//
// 1. D-input / Bluetooth gamepad input reports (usage page 0001, usage 0005, >= 15 / 34 bytes,
//    report 01 or 04, byte 14: bits 0-6 = percent, bit 7 = charging):
//    * 2DC8:6012  8BitDo Ultimate 2 Wireless (D-input / Bluetooth, firmware 1.03+ 34-byte report)
//    * 2DC8:202F / 202E  8BitDo Ultimate 3-mode
//    * 2DC8:301B / 301A / 310A  8BitDo Ultimate 2C Wireless
//    * 2DC8:6009  8BitDo Pro 3
//    * 2DC8:6003 / 6006  8BitDo Pro 2
//    * 2DC8:6001 / 6101  8BitDo SN30 Pro
//    * 2DC8:6000 / 6100  8BitDo SF30 Pro
//    Layout reference: SDL, src/joystick/hidapi/SDL_hidapi_8bitdo.c (HandleStatePacket).
//
// 2. Vendor config/telemetry channel (usage page FF7A or FFA0, usage 0001, 64-byte reports,
//    output report 81, input report 02):
//    In X-input mode (2DC8:310B) the 2.4 GHz dongle emulates a wired Xbox 360 pad to XInput,
//    but its FF7A:0001 vendor collection answers read-only status queries:
//      [81 04 20 01 ...]             -> [02 04 04 00 20 01 01 00 ...], byte 18 = 1 when linked
//      [81 04 21 01 00 00 06 ...]    -> [02 04 04 00 21 01 06 00 ...], byte 18 = battery % (1..100)
//      [81 04 00 01 ...] (CMD_INFO)  -> [02 04 04 00 00 01 ...], bytes 22..23 = internal PID
//                                       (6013 = 2.4G dongle, 6012 = controller over USB cable)
//    When the Ultimate 2 Wireless is placed on its charging dock, the dock's pogo pins turn the
//    2.4 GHz radio off and the receiver switches from 310B to 6013 (idle). We keep the last known
//    reading visible as asleep so the controller does not disappear while docked.
//    Protocol reference: MiguelSanzBr/8bitdo-ultimate2-linux (docs/PROTOCOL.md).
#include "drivers.h"

namespace {

const USHORT kVid = 0x2DC8;
const USHORT kPidDongle = 0x6013;
const USHORT kPidDongle24GIdle = 0x3109;
const int kReportLen = 34;  // Ultimate 2 v1.02 firmware sends 12-byte reports without battery

struct Model { USHORT pid; const wchar_t* name; bool needsFeature06; };
const Model kModels[] = {
    { 0x310B, L"8BitDo Ultimate 2 Wireless", false },
    { 0x6012, L"8BitDo Ultimate 2", false },
    { 0x6013, L"8BitDo Ultimate 2 Wireless", false },
    { 0x202F, L"8BitDo Ultimate 3-mode", false },
    { 0x202E, L"8BitDo Ultimate 3-mode", false },
    { 0x301B, L"8BitDo Ultimate 2C Wireless", false },
    { 0x301A, L"8BitDo Ultimate 2C Wireless", false },
    { 0x310A, L"8BitDo Ultimate 2C Wireless", false },
    { 0x3012, L"8BitDo Ultimate 2.4G", false },
    { 0x3013, L"8BitDo Ultimate 2.4G", false },
    { 0x3109, L"8BitDo Ultimate 2.4G", false },
    { 0x6007, L"8BitDo Ultimate Bluetooth", false },
    { 0x6009, L"8BitDo Pro 3", true },
    { 0x6003, L"8BitDo Pro 2", true },
    { 0x6006, L"8BitDo Pro 2", true },
    { 0x6001, L"8BitDo SN30 Pro", true },
    { 0x6101, L"8BitDo SN30 Pro", true },
    { 0x6000, L"8BitDo SF30 Pro", true },
    { 0x6100, L"8BitDo SF30 Pro", true },
};

const Model* FindModel(USHORT pid) {
    for (const Model& m : kModels)
        if (m.pid == pid) return &m;
    return nullptr;
}

bool IsUltimate2(USHORT pid) { return pid == 0x310B || pid == 0x6012 || pid == 0x6013; }

std::wstring ModelName(const HidColl& c) {
    if (const Model* m = FindModel(c.pid)) return m->name;
    std::wstring prod = HidProduct(c);
    if (!prod.empty()) {
        if (prod.rfind(L"8BitDo", 0) == 0 || prod.rfind(L"8Bitdo", 0) == 0) return prod;
        return L"8BitDo " + prod;
    }
    return L"8BitDo Wireless Controller";
}

int g_lastUltimate2Pct = -1;

bool QueryVendor(HidDev& d, const std::vector<BYTE>& req, BYTE cmdLo, BYTE cmdHi, std::vector<BYTE>& r) {
    d.Flush();
    if (!d.Write(req)) return false;
    ULONGLONG end = GetTickCount64() + 300;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (d.Read(r, left) < 19) break;
        if (r[0] == 0x02 && r[1] == 0x04 && r[4] == cmdLo && r[5] == cmdHi) return true;
    }
    return false;
}

bool PollVendorCollection(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;

    bool ult2 = IsUltimate2(c.pid);
    s.key = ult2 ? L"8bitdo:ultimate2" : (L"8bitdo:" + Hex4(c.pid));
    s.name = ModelName(c);
    s.kind = DeviceKind::Gamepad;
    s.via = L"2.4 GHz dongle or USB";
    s.graceSec = 1800;

    std::vector<BYTE> r;
    bool linkedKnown = false;
    bool linked = true;
    if (QueryVendor(d, { 0x81, 0x04, 0x20, 0x01 }, 0x20, 0x01, r)) {
        linkedKnown = true;
        linked = r[18] != 0;
    }

    if ((!linkedKnown || linked) && QueryVendor(d, { 0x81, 0x04, 0x21, 0x01, 0x00, 0x00, 0x06 }, 0x21, 0x01, r)) {
        int pct = r[18] & 0x7F;
        if (pct > 0 && pct <= 100) {
            s.connected = true;
            s.percent = pct;
            s.charging = (r[18] & 0x80) != 0 && pct < 100;
            if (ult2) g_lastUltimate2Pct = pct;
            return true;
        }
    }

    if (QueryVendor(d, { 0x81, 0x04, 0x00, 0x01 }, 0x00, 0x01, r) && r.size() >= 24) {
        USHORT internalPid = (USHORT)(r[22] | (r[23] << 8));
        if (internalPid != kPidDongle && internalPid != kPidDongle24GIdle) {
            s.via = L"USB cable";
            s.connected = true;
            s.charging = true;
            if (ult2 && g_lastUltimate2Pct >= 0) s.percent = g_lastUltimate2Pct;
            return true;
        }
        if (ult2 && g_lastUltimate2Pct >= 0) {
            s.connected = true;
            s.percent = -1;
            s.asleep = true;
            return true;
        }
        s.connected = false;
        return true;
    }

    if (c.pid == kPidDongle || c.pid == kPidDongle24GIdle) {
        if (ult2 && g_lastUltimate2Pct >= 0) {
            s.connected = true;
            s.percent = -1;
            s.asleep = true;
        } else {
            s.connected = false;
        }
        return true;
    }
    return false;
}

bool PollDInputGamepad(const HidColl& c, DeviceStatus& s) {
    const Model* m = FindModel(c.pid);
    HidDev d;
    if (m && m->needsFeature06 && c.featLen() > 0) {
        if (d.Open(c)) {
            std::vector<BYTE> feat;
            d.GetFeature(0x06, feat);
        }
    }
    if (!d.ok() && !d.OpenReadOnly(c)) return false;
    int minLen = c.pid == 0x6012 ? kReportLen : 15;
    std::vector<BYTE> r;
    bool found = false;
    ULONGLONG end = GetTickCount64() + 600;
    while (!found && (LONGLONG)end - (LONGLONG)GetTickCount64() > 0)
        found = d.Read(r, 100) >= minLen && (r[0] == 0x01 || r[0] == 0x04) && (r[14] & 0x7F) <= 100;
    if (!found) return false;
    bool ult2 = IsUltimate2(c.pid);
    s.key = ult2 ? L"8bitdo:ultimate2" : (L"8bitdo:" + Hex4(c.pid));
    s.name = ModelName(c);
    s.kind = DeviceKind::Gamepad;
    s.via = c.path.find(L"00001124") != std::wstring::npos ? L"Bluetooth" : L"2.4 GHz dongle or USB";
    s.percent = r[14] & 0x7F;
    s.charging = (r[14] & 0x80) != 0 && s.percent < 100;
    s.graceSec = 180;
    if (ult2 && s.percent > 0) g_lastUltimate2Pct = s.percent;
    return true;
}

}  // namespace

void Poll8BitDo(PollContext& ctx) {
    for (auto [vid, pid] : ctx.Products(kVid)) {
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                 return (c.page() == 0xFF7A || c.page() == 0xFFA0) && c.usage() == 0x0001 &&
                        c.inLen() >= 64 && c.outLen() >= 64;
             })) {
            DeviceStatus s;
            if (PollVendorCollection(*c, s)) {
                ctx.Add(s, vid, pid);
                break;
            }
        }
        if (!ctx.IsClaimed(vid, pid) && FindModel(pid)) {
            for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                     return c.page() == 0x01 && (c.usage() == 0x05 || c.usage() == 0x04) && c.inLen() >= 15;
                 })) {
                DeviceStatus s;
                if (PollDInputGamepad(*c, s)) {
                    ctx.Add(s, vid, pid);
                    break;
                }
            }
        }
    }
}
