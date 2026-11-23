// Exercise the production driver with simulated replies; no hardware access.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace {
std::deque<std::vector<BYTE>> replies;
std::vector<std::vector<BYTE>> writes;
int opens = 0, queries = 0, featureReads = 0;
bool openOk = true, sendOk = true;

HidColl Collection() {
    HidColl c;
    c.vid = 0x1915;
    c.pid = 0xAE1C;
    c.caps.UsagePage = 0xFFA0;
    c.caps.Usage = 1;
    c.caps.FeatureReportByteLength = 704;
    return c;
}

HidColl TenColl(USHORT pid = 0xEB01) {
    HidColl c;
    c.vid = 0x093A;
    c.pid = pid;
    c.path = L"test";
    c.caps.UsagePage = 0xFF01;
    c.caps.Usage = 1;
    c.caps.FeatureReportByteLength = 16;
    return c;
}

std::vector<BYTE> Status(BYTE battery, bool charging = false, bool online = true) {
    std::vector<BYTE> r(32, 0);
    r[0] = 5;
    r[1] = 0x15;
    r[9] = battery;
    r[10] = charging;
    r[12] = online;
    return r;
}

std::vector<BYTE> TenReply(BYTE cmd, BYTE value) {
    std::vector<BYTE> r(16, 0);
    r[0] = 0x06;
    r[1] = cmd;
    r[8] = value;
    return r;
}

PollContext Poll(std::vector<std::vector<BYTE>> data) {
    replies.assign(data.begin(), data.end());
    writes.clear();
    queries = 0;
    featureReads = 0;
    PollContext ctx;
    ctx.colls = { Collection() };
    PollNinjutso(ctx);
    return ctx;
}

PollContext PollTenRun(std::deque<std::vector<BYTE>> data, HidColl c = TenColl()) {
    replies = std::move(data);
    writes.clear();
    opens = 0;
    queries = 0;
    featureReads = 0;
    PollContext ctx;
    ctx.colls = { c };
    PollNinjutso(ctx);
    return ctx;
}
}  // namespace

bool HidDev::OpenFeatureOnly(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    ++opens;
    return openOk;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    if (c_.vid == 0x093A) {
        CHECK(data.size() == 16);
        CHECK(data[0] == 0x06 && data[4] == 1);
        for (size_t i = 2; i < data.size(); ++i)
            if (i != 4) CHECK(data[i] == 0);
        writes.push_back(data);
        ++queries;
        return sendOk;
    }
    auto expected = std::vector<BYTE>(32, 0);
    expected[0] = 5;
    expected[1] = 0x15;
    expected[4] = 1;
    assert(data == expected);  // only the documented state query can be sent
    ++queries;
    return sendOk;
}
bool HidDev::GetFeature(BYTE id, std::vector<BYTE>& out) {
    if (c_.vid == 0x093A) {
        CHECK(id == 0x06);
        ++featureReads;
    } else {
        assert(id == 5);
    }
    if (replies.empty()) return false;
    out = replies.front();
    replies.pop_front();
    return true;
}
std::wstring HidProduct(const HidColl&) { return L"Ninjutso Ten Air"; }
std::wstring Hex4(USHORT value) {
    wchar_t s[8];
    swprintf(s, 8, L"%04X", value);
    return s;
}

int main() {
    auto ctx = Poll({ Status(32) });
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x1915, 0xAE1C));
    const auto& s = ctx.out[0];
    assert(s.percent == 32 && s.connected && !s.charging);
    assert(s.name == L"Ninjutso Sora V2" && s.key == L"ninjutso:1915:AE1C");
    assert(s.kind == DeviceKind::Mouse && s.via == L"2.4 GHz receiver");
    assert(queries == 1);
    int before = opens;
    PollNinjutso(ctx);
    assert(opens == before && ctx.out.size() == 1);

    ctx = Poll({ Status(0, true) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].charging);
    ctx = Poll({ Status(32, true, false), Status(31) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 31 && queries == 2);
    ctx = Poll({ Status(0, true, false), Status(0, true, false), Status(0, true, false), Status(0, true, false) });
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && !ctx.out[0].charging && ctx.out[0].percent == -1);
    assert(queries == 4);

    auto wrongId = Status(32), wrongCommand = Status(32);
    wrongId[0] = 4;
    wrongCommand[1] = 0x28;
    ctx = Poll({ wrongId, wrongCommand, std::vector<BYTE>(12), Status(101) });
    assert(ctx.out.empty() && ctx.claimed.empty() && queries == 4);
    ctx = Poll({});
    assert(ctx.out.empty() && ctx.claimed.empty() && queries == 4);
    sendOk = false;
    ctx = Poll({ Status(32) });
    assert(ctx.out.empty() && queries == 1);
    sendOk = true;

    ctx = {};
    auto otherVendor = Collection(), otherProduct = Collection(), otherPage = Collection();
    auto otherUsage = Collection(), shortReport = Collection();
    otherVendor.vid = 0x1234;
    otherProduct.pid = 0xAE8C;  // 8K receiver is outside the verified scope
    otherPage.caps.UsagePage = 1;
    otherUsage.caps.Usage = 2;
    shortReport.caps.FeatureReportByteLength = 31;
    ctx.colls = { otherVendor, otherProduct, otherPage, otherUsage, shortReport };
    before = opens;
    PollNinjutso(ctx);
    assert(opens == before && ctx.out.empty());

    // Ninjutso Ten / Ten Air (093A:EB01 receiver, 093A:E020 cable)
    ctx = PollTenRun({ TenReply(0x13, 1), TenReply(0x12, 67), TenReply(0x11, 0) });
    CHECK(ctx.out.size() == 1 && ctx.IsClaimed(0x093A, 0xEB01));
    CHECK(ctx.out[0].percent == 67 && !ctx.out[0].charging && ctx.out[0].connected);
    CHECK(ctx.out[0].name == L"Ninjutso Ten Air" && ctx.out[0].kind == DeviceKind::Mouse);
    CHECK(ctx.out[0].via == L"2.4 GHz receiver" && ctx.out[0].key == L"ninjutso-ten:EB01");
    CHECK(writes.size() == 3 && writes[0][1] == 0x13 && writes[1][1] == 0x12 && writes[2][1] == 0x11);

    ctx = PollTenRun({ TenReply(0x13, 0), TenReply(0x12, 67), TenReply(0x11, 0) });
    CHECK(ctx.out.empty() && ctx.claimed.empty() && writes.size() == 1);
    ctx = PollTenRun({ TenReply(0x13, 1), TenReply(0x12, 101), TenReply(0x11, 0) });
    CHECK(ctx.out.empty() && ctx.claimed.empty());
    ctx = PollTenRun({ { 0x06, 0 }, TenReply(0x13, 1), TenReply(0x12, 20), TenReply(0x11, 1) });
    CHECK(ctx.out.size() == 1 && ctx.out[0].charging && writes.size() == 3);

    for (BYTE pct : { 0, 100 }) {
        ctx = PollTenRun({ TenReply(0x13, 1), TenReply(0x12, pct), TenReply(0x11, 1) }, TenColl(0xE020));
        CHECK(ctx.out.size() == 1 && ctx.out[0].percent == pct && ctx.out[0].charging);
        CHECK(ctx.out[0].via == L"USB cable" && ctx.IsClaimed(0x093A, 0xE020));
    }

    auto tenWrongId = TenReply(0x13, 1);
    tenWrongId[0] = 7;
    for (auto bad : std::vector<std::vector<BYTE>>{ {}, { 0x06 }, tenWrongId, TenReply(0x12, 1), std::vector<BYTE>(17, 0) }) {
        ctx = PollTenRun({ bad, bad, bad, TenReply(0x13, 1) });
        CHECK(ctx.out.empty() && ctx.claimed.empty() && writes.size() == 1 && featureReads == 3);
    }
    ctx = PollTenRun({});
    CHECK(ctx.out.empty() && ctx.claimed.empty() && featureReads == 3);
    sendOk = false;
    ctx = PollTenRun({});
    CHECK(ctx.out.empty() && featureReads == 0 && writes.size() == 1);
    sendOk = true;
    openOk = false;
    ctx = PollTenRun({});
    CHECK(ctx.out.empty() && writes.empty());
    openOk = true;

    for (int variant = 0; variant < 7; ++variant) {
        auto c = TenColl();
        if (variant == 0) c.vid = 0x1234;
        if (variant == 1) c.pid = 0xEA01;
        if (variant == 2) c.caps.UsagePage = 0xFF00;
        if (variant == 3) c.caps.UsagePage = 0xFF02;
        if (variant == 4) c.caps.Usage = 2;
        if (variant == 5) c.caps.FeatureReportByteLength = 64;
        if (variant == 6) c.caps.FeatureReportByteLength = 0;
        ctx = PollTenRun({}, c);
        CHECK(opens == 0 && writes.empty() && ctx.claimed.empty());
    }

    ctx = {};
    ctx.colls = { TenColl() };
    ctx.Claim(0x093A, 0xEB01);
    opens = 0;
    PollNinjutso(ctx);
    CHECK(opens == 0);

    puts("Ninjutso driver tests passed");
}
