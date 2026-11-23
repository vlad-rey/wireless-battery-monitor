// Production poller with a simulated HID transport; fixtures captured from an L7 Arctic.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

void PollMachenike(PollContext& ctx);

namespace {
const std::vector<BYTE> kWireless = {
    0x51,0x06,0x01,0x00,0x8A,0x24,0x4E,0xFF,0x05,0x01,0x09,0x40,0x00,0,0,0,0,0,0,0,0
};
const std::vector<BYTE> kCable = {
    0x51,0x06,0x01,0x00,0x8A,0x24,0x4E,0xFF,0x05,0x01,0x00,0x4A,0x01,0,0,0,0,0,0,0,0
};
// Receiver remains present while the mouse uses its cable: cached percentage is not live.
const std::vector<BYTE> kOffline = {
    0x51,0x06,0x01,0x00,0x8A,0x24,0x4E,0xFF,0x05,0x01,0x01,0x40,0x01,0,0,0,0,0,0,0,0
};
int failures = 0, opens = 0, sends = 0, reads = 0;
bool openOk = true, sendOk = true, repeatReply = false;
std::wstring product = L"L7 Mix";
std::vector<std::vector<BYTE>> replies;
const char* current = "";

void Check(bool result, const char* expression, int line) {
    if (!result) { std::printf("FAIL %s:%d %s\n", current, line, expression); ++failures; }
}
#define CHECK(x) Check((x), #x, __LINE__)

HidColl Collection(bool cable = false) {
    HidColl c;
    c.vid = 0x248A;
    c.pid = cable ? 0xFF4E : 0xFF4C;
    c.iface = cable ? 2 : 1;
    c.path = cable ? L"cable" : L"receiver";
    c.caps.UsagePage = 0x008C;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = 33;
    c.caps.FeatureReportByteLength = 1001;
    return c;
}

PollContext Context(bool cable = false) {
    opens = sends = reads = 0;
    openOk = sendOk = true;
    repeatReply = false;
    product = L"L7 Mix";
    replies.clear();
    PollContext ctx;
    ctx.colls = { Collection(cable) };
    return ctx;
}

void Connected(const std::vector<BYTE>& reply, int percent, bool cable, bool charging) {
    auto ctx = Context(cable);
    replies = { reply };
    PollMachenike(ctx);
    CHECK(ctx.out.size() == 1);
    CHECK(ctx.IsClaimed(0x248A, cable ? 0xFF4E : 0xFF4C));
    CHECK(opens == 1 && sends == 1);
    if (ctx.out.size() == 1) {
        const auto& s = ctx.out[0];
        CHECK(s.percent == percent && s.charging == charging && s.connected && !s.asleep);
        CHECK(s.kind == DeviceKind::Mouse);
        CHECK(s.name == L"Machenike L7 Arctic / L7 Mix");
        CHECK(s.via == (cable ? L"USB cable" : L"2.4 GHz receiver"));
        auto before = opens;
        PollMachenike(ctx);
        CHECK(opens == before && ctx.out.size() == 1);
    }
}
}  // namespace

bool HidDev::OpenFeatureOnly(const HidColl& c) {
    c_ = c;
    ++opens;
    // The status report is 21 bytes; 1001 is the largest *other* report on this collection.
    assert(c.featLen() == 21);
    return openOk;
}
void HidDev::Close() {}
bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    const std::vector<BYTE> expected = {0x51,0x06,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    assert(data == expected && c_.featLen() == 21);
    ++sends;
    return sendOk;
}
bool HidDev::GetFeature(BYTE id, std::vector<BYTE>& out) {
    assert(id == 0x51 && c_.featLen() == 21);
    ++reads;
    if (replies.empty()) return false;
    out = replies.front();
    if (!repeatReply) replies.erase(replies.begin());
    return true;
}
std::wstring HidProduct(const HidColl&) { return product; }

int main() {
    current = "captured wireless 64%";
    Connected(kWireless, 64, false, false);
    current = "captured cable 74%, charging, no wireless connected bit";
    Connected(kCable, 74, true, true);
    current = "valid zero and full are percentages";
    for (BYTE pct : { BYTE(0), BYTE(100) }) {
        auto r = kWireless;
        r[11] = pct;
        Connected(r, pct, false, false);
    }

    current = "offline receiver must not show cached 64% or charging";
    auto ctx = Context();
    replies = { kOffline };
    PollMachenike(ctx);
    CHECK(ctx.out.size() == 1);
    if (ctx.out.size() == 1) CHECK(!ctx.out[0].connected && ctx.out[0].percent == -1 && !ctx.out[0].charging);

    current = "cable wins, receiver suppressed, stable identity";
    ctx = Context(true);
    ctx.colls.push_back(Collection());
    replies = { kCable };
    PollMachenike(ctx);
    CHECK(ctx.out.size() == 1 && sends == 1 && ctx.IsClaimed(0x248A, 0xFF4C));
    std::wstring cableKey = ctx.out.empty() ? L"" : ctx.out[0].key;
    ctx = Context();
    replies = { kWireless };
    PollMachenike(ctx);
    if (ctx.out.size() == 1) CHECK(ctx.out[0].key == cableKey);

    current = "strict identity and collection, no device commands";
    for (int field = 0; field < 8; ++field) {
        ctx = Context();
        auto& c = ctx.colls[0];
        switch (field) {
        case 0: c.vid = 0x249A; break;
        case 1: c.pid = 0xFF4D; break;
        case 2: c.iface = 2; break;
        case 3: c.caps.UsagePage = 0xFF00; break;
        case 4: c.caps.Usage = 2; break;
        case 5: c.caps.InputReportByteLength = 32; break;
        case 6: c.caps.OutputReportByteLength = 32; break;
        case 7: c.caps.FeatureReportByteLength = 1000; break;
        }
        replies = { kWireless };
        PollMachenike(ctx);
        CHECK(ctx.out.empty() && ctx.claimed.empty() && opens == 0 && sends == 0);
    }
    ctx = Context();
    product = L"Unrelated mouse";
    PollMachenike(ctx);
    CHECK(opens == 0 && ctx.claimed.empty());

    current = "malformed reply skipped before valid status";
    auto wrongReport = kWireless, wrongCommand = kWireless, wrongDevice = kWireless, wrongMode = kWireless;
    auto badPercent = kWireless, badCharge = kWireless;
    wrongReport[0] = 0x52;
    wrongCommand[1] = 4;
    wrongDevice[6] = 0x4D;
    wrongMode[10] = 0x0F;
    badPercent[11] = 101;
    badCharge[12] = 0xFF;
    ctx = Context();
    replies = { wrongReport, wrongCommand, wrongDevice, wrongMode, badPercent, badCharge, std::vector<BYTE>(12), kWireless };
    PollMachenike(ctx);
    CHECK(ctx.out.size() == 1 && reads >= 8);
    if (ctx.out.size() == 1) CHECK(ctx.out[0].percent == 64);

    current = "wrong cable collection rejected";
    ctx = Context(true);
    ctx.colls[0].iface = 1;
    PollMachenike(ctx);
    CHECK(opens == 0 && ctx.claimed.empty());

    current = "known mouse reserved after transport failure, no Ajazz probing";
    ctx = Context(); openOk = false;
    PollMachenike(ctx);
    CHECK(ctx.out.empty() && sends == 0 && ctx.IsClaimed(0x248A, 0xFF4C));
    ctx = Context(); sendOk = false;
    PollMachenike(ctx);
    CHECK(ctx.out.empty() && sends == 1 && reads == 0 && ctx.IsClaimed(0x248A, 0xFF4C));

    current = "missing or continuously unrelated reply has bounded deadline";
    for (bool unrelated : { false, true }) {
        ctx = Context();
        if (unrelated) { replies = { wrongCommand }; repeatReply = true; }
        auto start = GetTickCount64();
        PollMachenike(ctx);
        CHECK(GetTickCount64() - start < 800);
        CHECK(ctx.out.empty() && sends == 1 && ctx.IsClaimed(0x248A, 0xFF4C));
    }
    if (failures) { std::printf("Machenike: %d failed checks\n", failures); return 1; }
    std::puts("Machenike driver tests passed (captured replies, strict matching, zero, charging, offline, dedup, malformed, deadline).");
}
