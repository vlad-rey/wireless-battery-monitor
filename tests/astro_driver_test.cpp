#include "../src/drivers/drivers.h"
#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> replies;
bool openOk = true, writeOk = true;
int opens = 0;
HidColl Collection() {
    HidColl c;
    c.path = L"astro-test";
    c.vid = 0x046D; c.pid = 0x0B0B;
    c.caps.UsagePage = 0xFF32; c.caps.Usage = 0x74;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = 64;
    return c;
}
std::vector<BYTE> Reply(BYTE level, BYTE dock = 0) {
    return {2, 12, 6, 0, 6, 12, level, 100, dock};
}
PollContext Poll(std::vector<std::vector<BYTE>> data, HidColl c = Collection()) {
    replies = std::move(data);
    PollContext ctx; ctx.colls = {c}; PollAstro(ctx); return ctx;
}
}
std::wstring Hex4(USHORT v) { wchar_t b[8]; swprintf(b, 8, L"%04X", v); return b; }  // used by the Gen 3 / Gen 4 / A20 paths
bool HidDev::Open(const HidColl& c) { ++opens; c_ = c; return openOk; }
void HidDev::Close() {}
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    std::vector<BYTE> expected(64, 0);
    expected[0] = 2; expected[1] = 12; expected[2] = 3;
    expected[4] = 6; expected[5] = 12;
    assert(data == expected); return writeOk;
}
int HidDev::Read(std::vector<BYTE>& out, int timeout) {
    assert(timeout > 0 && timeout <= 1000);
    if (replies.empty()) return -1;
    out = replies.front(); replies.erase(replies.begin()); return (int)out.size();
}
int main() {
    auto ctx = Poll({Reply(99)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 99 && !ctx.out[0].charging);
    assert(ctx.out[0].name == L"ASTRO A50 X" && ctx.out[0].kind == DeviceKind::Headset);
    assert(ctx.IsClaimed(0x046D, 0x0B0B));
    int before = opens; PollAstro(ctx); assert(opens == before);
    ctx = Poll({Reply(0, 1)});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 0 && ctx.out[0].charging);
    auto push = Reply(100); push[5] = 0;
    ctx = Poll({push}); assert(ctx.out.size() == 1 && ctx.out[0].percent == 100);
    auto unrelated = Reply(50); unrelated[4] = 8;
    ctx = Poll({unrelated, Reply(60)}); assert(ctx.out.size() == 1 && ctx.out[0].percent == 60);
    for (int field : {0, 1, 2, 3, 4, 5, 6}) {
        auto bad = Reply(50); bad[field] = 255;
        ctx = Poll({bad}); assert(ctx.out.empty() && ctx.claimed.empty());
    }
    ctx = Poll({{2, 12, 6, 0, 6, 12, 50}}); assert(ctx.out.empty());
    ctx = Poll({}); assert(ctx.out.empty());
    auto c = Collection(); c.pid = 0x0B1C;
    ctx = Poll({Reply(75)}, c); assert(ctx.out.size() == 1 && ctx.out[0].name == L"ASTRO A50 Gen 5");
    for (int field = 0; field < 5; ++field) {
        c = Collection();
        if (field == 0) c.vid = 0x1234;
        if (field == 1) c.pid = 0x1234;
        if (field == 2) c.caps.UsagePage = 0xFF00;
        if (field == 3) c.caps.Usage = 1;
        if (field == 4) c.caps.OutputReportByteLength = 20;
        before = opens; ctx = Poll({Reply(50)}, c);
        assert(ctx.out.empty() && opens == before);
    }
    openOk = false; ctx = Poll({Reply(50)}); assert(ctx.out.empty());
    openOk = true; writeOk = false; ctx = Poll({Reply(50)}); assert(ctx.out.empty());
    puts("Astro driver tests passed");
}
