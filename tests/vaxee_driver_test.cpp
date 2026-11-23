// Simulated HID replies; checks read-only commands, validation and device filtering.
#include "../src/drivers/drivers.h"
#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> replies;
int opens = 0, queries = 0;
bool sendOk = true;
bool openOk = true;

HidColl Collection() {
    HidColl c;
    c.path = L"test-receiver";
    c.vid = 0x3057;
    c.pid = 0x1002;
    c.caps.UsagePage = 0xFF05;
    c.caps.Usage = 1;
    c.caps.FeatureReportByteLength = 64;
    return c;
}
std::vector<BYTE> Reply(BYTE command, BYTE value) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x0E;
    r[1] = 0xA5;
    r[2] = command;
    r[3] = 1;
    r[4] = 1;
    r[5] = value;
    return r;
}
PollContext Poll(std::vector<std::vector<BYTE>> data, USHORT pid = 0x1002) {
    PollContext unplugged;
    PollVaxee(unplugged); // Reset driver state between independent test cases.
    replies = std::move(data);
    queries = 0;
    PollContext ctx;
    ctx.colls = {Collection()};
    ctx.colls[0].pid = pid;
    if (pid == 0x1011) ctx.colls[0].path = L"test-cable";
    PollVaxee(ctx);
    return ctx;
}
}
bool HidDev::OpenFeatureOnly(const HidColl& c) {
    ++opens;
    if (!openOk) return false;
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    auto expected = std::vector<BYTE>(64, 0);
    expected[0] = 0x0E;
    expected[1] = 0xA5;
    expected[2] = data[2];
    expected[3] = 1;
    expected[4] = 1;
    assert(data[2] == 0x0B || data[2] == 0x10);
    assert(data == expected);
    ++queries;
    return sendOk;
}
bool HidDev::GetFeature(BYTE id, std::vector<BYTE>& out) {
    assert(id == 0x0E);
    if (replies.empty()) return false;
    out = replies.front();
    replies.erase(replies.begin());
    return true;
}
int main() {
    auto ctx = Poll({Reply(0x0B, 13), Reply(0x10, 0)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 65 && !ctx.out[0].charging);
    assert(ctx.IsClaimed(0x3057, 0x1002) && queries == 2);
    assert(ctx.out[0].via == L"2.4 GHz receiver");
    int before = opens;
    PollVaxee(ctx);
    assert(opens == before);
    ctx = Poll({Reply(0x0B, 12), Reply(0x10, 0)}, 0x2002); // 8K receiver
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 60);
    assert(ctx.out[0].name == L"VAXEE 8K Wireless");
    assert(ctx.out[0].via == L"2.4 GHz receiver" && ctx.IsClaimed(0x3057, 0x2002));
    ctx = Poll({Reply(0x0B, 14), Reply(0x10, 1)}, 0x1011);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 70 && ctx.out[0].charging);
    assert(ctx.out[0].name == L"ZYGEN NP-01S V2 Wireless" && ctx.out[0].via == L"USB cable");
    assert(ctx.IsClaimed(0x3057, 0x1011));
    PollContext unplugged;
    PollVaxee(unplugged);
    assert(unplugged.out.size() == 1 && !unplugged.out[0].connected);
    assert(unplugged.claimed.empty()); // No hardware is claimed when it is physically gone.
    PollContext unpluggedAgain;
    PollVaxee(unpluggedAgain);
    assert(unpluggedAgain.out.empty());
    ctx = Poll({Reply(0x0B, 20), Reply(0x10, 1)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].charging);
    ctx = Poll({Reply(0x0B, 0)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 0);
    ctx = Poll({Reply(0x0B, 21), Reply(0x0B, 7), Reply(0x10, 0)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 35 && queries == 3);
    ctx = Poll({Reply(0x0B, 21), Reply(0x10, 10)});
    assert(ctx.out.empty() && ctx.claimed.empty());
    auto badHeader = Reply(0x0B, 10);
    badHeader[1] = 0;
    ctx = Poll({badHeader, std::vector<BYTE>(5)});
    assert(ctx.out.empty());
    for (int offset : {0, 2, 4}) {
        auto bad = Reply(0x0B, 10);
        bad[offset] = 0;
        ctx = Poll({bad, bad});
        assert(ctx.out.empty() && ctx.claimed.empty() && queries == 2);
    }
    // A stale battery reply must not be interpreted as a charging response.
    ctx = Poll({Reply(0x0B, 10), Reply(0x0B, 10)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 50 && !ctx.out[0].charging);
    ctx = Poll({});
    assert(ctx.out.empty());
    sendOk = false;
    ctx = Poll({Reply(0x0B, 10)});
    assert(ctx.out.empty());
    sendOk = true;
    openOk = false;
    ctx = Poll({Reply(0x0B, 10)});
    assert(ctx.out.empty() && queries == 0);
    openOk = true;
    ctx = {};
    auto wrongVendor = Collection(), wrongPid = Collection(), wrongPage = Collection();
    auto wrongUsage = Collection(), wrongLength = Collection();
    wrongVendor.vid = 0x1234;
    wrongPid.pid = 0x0002; // Do not send VAXEE commands to ZOWIE or other mice.
    wrongPage.caps.UsagePage = 1;
    wrongUsage.caps.Usage = 2;
    wrongLength.caps.FeatureReportByteLength = 63;
    ctx.colls = {wrongVendor, wrongPid, wrongPage, wrongUsage, wrongLength};
    before = opens;
    PollVaxee(ctx);
    assert(opens == before && ctx.out.empty());
    puts("VAXEE driver tests passed");
}
