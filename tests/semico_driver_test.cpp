// Exercise the Semico keyboard driver with a simulated HID transport; no USB writes or hardware needed.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>
#include <map>

namespace {

std::map<std::wstring, std::vector<std::vector<BYTE>>> replies;
std::vector<std::vector<BYTE>> writes;
int opens = 0;

HidColl Collection(USHORT vid, USHORT pid, const wchar_t* path, int iface = 0, USHORT page = 0xFF02,
                   USHORT usage = 0x0002, USHORT len = 65) {
    HidColl c;
    c.path = path;
    c.vid = vid;
    c.pid = pid;
    c.iface = iface;
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.InputReportByteLength = len;
    c.caps.OutputReportByteLength = len;
    return c;
}

std::vector<BYTE> Reply(BYTE percent, BYTE state) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0xAA;
    r[1] = 0x55;
    r[2] = 0xCC;
    r[3] = 0x33;
    r[4] = 0x0E;
    r[5] = percent;
    r[6] = state;
    return r;
}

}  // namespace

// Stand-ins for the Win32 HID transport. The production driver is linked unchanged.
bool HidDev::Open(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    ++opens;
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    writes.push_back(data);
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    auto& q = replies[c_.path];
    if (q.empty()) return -1;
    out = q.front();
    q.erase(q.begin());
    return (int)out.size();
}
std::wstring HidProduct(const HidColl& c) {
    if (c.pid == 0x8FFF) return L"2.4 Wireless Device";
    if (c.pid == 0xA034) return L"USB Gaming Keyboard";
    return L"";
}

int main() {
    // 1. Live Attack Shark X98 capture on 2.4 GHz dongle (unplugged on battery: 96%, state=1).
    writes.clear();
    PollContext ctx;
    ctx.colls = { Collection(0x1A2C, 0x8FFF, L"dongle") };
    replies = { { L"dongle", { { 0x01, 0x02, 0x03 }, Reply(0x60, 0x01) } } };
    PollSemico(ctx);
    assert(writes.size() == 1 && writes[0].size() == 65);
    assert(writes[0][0] == 0x00 && writes[0][1] == 0xAA && writes[0][2] == 0x55 &&
           writes[0][3] == 0xCC && writes[0][4] == 0x33 && writes[0][5] == 0x0E);
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"semico:1A2C");
    assert(ctx.out[0].name == L"Wireless keyboard" && ctx.out[0].kind == DeviceKind::Keyboard);
    assert(ctx.out[0].via == L"2.4 GHz receiver" && ctx.out[0].percent == 96 && !ctx.out[0].charging);
    assert(ctx.out[0].connected && ctx.out[0].graceSec == 1800);
    assert(ctx.IsClaimed(0x1A2C, 0x8FFF));

    // 2. Dongle active + Attack Shark X98 USB cable also plugged in (cable times out while in 2.4G):
    //    uses the X98 model name, emits a single entry, and claims both PIDs.
    ctx = {};
    ctx.colls = { Collection(0x1A2C, 0xA034, L"cable"), Collection(0x1A2C, 0x8FFF, L"dongle") };
    replies = { { L"dongle", { Reply(0x58, 0x01) } } };
    PollSemico(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"semico:1A2C");
    assert(ctx.out[0].name == L"Attack Shark X98" && ctx.out[0].percent == 88);
    assert(ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.IsClaimed(0x1A2C, 0x8FFF) && ctx.IsClaimed(0x1A2C, 0xA034));

    // 3. Dongle reports offline (state=0), wired cable (A034) answers online (100%, state=3).
    ctx = {};
    ctx.colls = { Collection(0x1A2C, 0x8FFF, L"dongle"), Collection(0x1A2C, 0xA034, L"cable") };
    replies = { { L"dongle", { Reply(0x00, 0x00) } }, { L"cable", { Reply(0x64, 0x03) } } };
    PollSemico(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"semico:1A2C");
    assert(ctx.out[0].name == L"Attack Shark X98" && ctx.out[0].percent == 100);
    assert(ctx.out[0].via == L"USB cable" && ctx.out[0].connected);

    // 4. Dongle reports offline (state=0) with no cable answering: connected=false, no invented %.
    ctx = {};
    ctx.colls = { Collection(0x1A2C, 0x8FFF, L"dongle") };
    replies = { { L"dongle", { Reply(0x00, 0x00) } } };
    PollSemico(ctx);
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && ctx.out[0].percent == -1);
    assert(ctx.IsClaimed(0x1A2C, 0x8FFF));

    // 5. Malformed percent (>100) or invalid state (>3) is rejected without claiming.
    ctx = {};
    ctx.colls = { Collection(0x1A2C, 0x8FFF, L"dongle") };
    replies = { { L"dongle", { Reply(101, 2) } } };
    PollSemico(ctx);
    assert(ctx.out.empty() && ctx.claimed.empty());

    ctx = {};
    ctx.colls = { Collection(0x1A2C, 0x8FFF, L"dongle") };
    replies = { { L"dongle", { Reply(50, 4) } } };
    PollSemico(ctx);
    assert(ctx.out.empty() && ctx.claimed.empty());

    // 6. Strict filtering: wrong vendor, unknown PID, wrong interface, wrong page/usage/size never opened.
    opens = 0;
    ctx = {};
    ctx.colls = {
        Collection(0x1234, 0x8FFF, L"other-vid"),
        Collection(0x1A2C, 0x1234, L"unknown-pid"),
        Collection(0x1A2C, 0x8FFF, L"wrong-iface", 3),
        Collection(0x1A2C, 0x8FFF, L"wrong-page", 0, 0xFF01, 0xFF01, 4),
        Collection(0x1A2C, 0x8FFF, L"wrong-len", 0, 0xFF02, 0x0002, 33),
    };
    PollSemico(ctx);
    assert(opens == 0 && ctx.out.empty());

    std::puts("Semico driver tests passed (dongle, cable, offline, bounds, filtering).");
}
