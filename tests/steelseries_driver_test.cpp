// Exercise the real driver with a simulated transport; no hardware commands are sent.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {
std::vector<BYTE> reply;
int opens = 0, writes = 0, reads = 0;
bool openOk = true, writeOk = true;

PollContext Context(USHORT pid = 0x225D) {
    opens = writes = reads = 0;
    openOk = writeOk = true;
    reply.assign(64, 0);
    reply[0] = 0x06;
    reply[1] = 0xB0;
    reply[6] = 4;
    reply[15] = 0x08;
    HidColl c;
    c.vid = 0x1038;
    c.pid = pid;
    c.iface = 4;
    c.caps.UsagePage = 0xFFC0;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = 64;
    c.caps.OutputReportByteLength = 64;
    c.caps.FeatureReportByteLength = 1024;
    PollContext ctx;
    ctx.colls = { c };
    return ctx;
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    c_ = c;
    ++opens;
    return openOk;
}
void HidDev::Close() {}
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& request, int timeoutMs) {
    assert((request == std::vector<BYTE>{ 0x06, 0xB0 }));
    assert(timeoutMs > 0 && timeoutMs <= 500);
    ++writes;
    // The 225D base station rejects this report ID on its FF00 collection.
    return writeOk && c_.page() == 0xFFC0;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    ++reads;
    out = reply;
    return out.empty() ? -1 : (int)out.size();
}
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
int MapTo100(int v, int lo, int hi) { return Clamp100((v - lo) * 100 / (hi - lo)); }

int main() {
    auto ctx = Context();
    PollSteelSeries(ctx);
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x1038, 0x225D));
    const auto& s = ctx.out[0];
    assert(s.name == L"SteelSeries Arctis Nova Pro Wireless" && s.key == L"steelseries:225D");
    assert(s.kind == DeviceKind::Headset && s.via == L"2.4 GHz receiver");
    assert(s.percent == 50 && s.connected && !s.charging);
    assert(opens == 1 && writes == 1 && reads == 1);
    ctx = Context(0x12E0);
    PollSteelSeries(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"steelseries:12E0" && ctx.out[0].percent == 50);

    ctx = Context();
    ctx.colls.push_back(ctx.colls[0]);
    ctx.colls[1].caps.UsagePage = 0xFF00;
    ctx.colls[1].caps.FeatureReportByteLength = 64;
    PollSteelSeries(ctx);
    assert(ctx.out.size() == 1 && opens == 1 && writes == 1 && reads == 1);
    PollSteelSeries(ctx);
    assert(opens == 1 && writes == 1);  // already claimed

    ctx = Context();
    auto other = ctx.colls[0];
    other.caps.UsagePage = 0xFF00;
    other.caps.FeatureReportByteLength = 64;
    ctx.colls.insert(ctx.colls.begin(), other);
    PollSteelSeries(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 50);
    assert(opens == 2 && writes == 2 && reads == 1);  // skip FF00 when it is enumerated first

    for (BYTE level : { BYTE(0), BYTE(8) }) {
        ctx = Context();
        reply[6] = level;
        reply[15] = 0x02;
        PollSteelSeries(ctx);
        assert(ctx.out.size() == 1 && ctx.out[0].percent == (level == 0 ? 0 : 100));
        assert(ctx.out[0].connected && ctx.out[0].charging);
    }

    ctx = Context();
    reply[15] = 0x01;
    PollSteelSeries(ctx);
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && !ctx.out[0].charging && ctx.out[0].percent == -1);

    for (size_t length : { size_t(0), size_t(15) }) {
        ctx = Context();
        reply.resize(length);
        PollSteelSeries(ctx);
        assert(ctx.out.empty() && ctx.claimed.empty() && reads == 1);
    }

    ctx = Context();
    writeOk = false;
    PollSteelSeries(ctx);
    assert(ctx.out.empty() && ctx.claimed.empty() && writes == 1 && reads == 0);
    ctx = Context();
    openOk = false;
    PollSteelSeries(ctx);
    assert(ctx.out.empty() && ctx.claimed.empty() && opens == 1 && writes == 0);

    for (int invalid = 0; invalid < 4; ++invalid) {
        ctx = Context();
        if (invalid == 0) ctx.colls[0].vid = 0x1039;
        if (invalid == 1) ctx.colls[0].pid = 0x225E;
        if (invalid == 2) ctx.colls[0].iface = 3;
        if (invalid == 3) ctx.colls[0].caps.OutputReportByteLength = 0;
        PollSteelSeries(ctx);
        assert(ctx.out.empty() && ctx.claimed.empty() && opens == 0 && writes == 0);
    }
    puts("SteelSeries driver tests passed (Nova Pro PIDs, collections, battery, charging, offline, failures, filtering)");
}
