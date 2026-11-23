// Exercise the real driver with a simulated transport; no hardware commands are sent.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> replies;
std::vector<std::vector<BYTE>> wiredReplies;
std::vector<USHORT> queriedPids;
int opens = 0, sends = 0, reads = 0;
int readDelayMs = 0;
bool openOk = true, featureOpenOk = true, sendOk = true;

HidColl Collection(bool input, USHORT pid = 0x1074) {
    HidColl c;
    c.vid = 0x260D;
    c.pid = pid;
    c.iface = 1;
    c.path = input ? L"input" : L"feature";
    c.caps.UsagePage = input ? 0xFF01 : 0xFF02;
    c.caps.Usage = input ? 0 : 2;
    c.caps.InputReportByteLength = input ? 17 : 0;
    c.caps.FeatureReportByteLength = input ? 0 : 17;
    return c;
}

std::vector<BYTE> Reply(BYTE percent = 85, BYTE charging = 0) {
    return { 0x09, 0x04, 0, 0, 0, 2, percent, charging, 0, 0, 0, 0, 0, 0, 0, 0, 0xF1 };
}

PollContext Context(USHORT pid = 0x1074) {
    opens = sends = reads = 0;
    readDelayMs = 0;
    openOk = featureOpenOk = sendOk = true;
    replies.clear();
    wiredReplies.clear();
    queriedPids.clear();
    PollContext ctx;
    ctx.colls = { Collection(true, pid), Collection(false, pid) };
    return ctx;
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    assert(c.path == L"input");
    c_ = c;
    ++opens;
    return openOk;
}
bool HidDev::OpenFeatureOnly(const HidColl& c) {
    assert(c.path == L"feature");
    c_ = c;
    ++opens;
    return featureOpenOk;
}
void HidDev::Close() {}
void HidDev::Flush() { assert(c_.path == L"input"); }
bool HidDev::SetFeature(const std::vector<BYTE>& request) {
    assert(c_.path == L"feature");
    std::vector<BYTE> expected(17, 0);
    expected[0] = 0x08;
    expected[1] = 0x04;
    expected[16] = 0x49;
    assert(request == expected);
    ++sends;
    queriedPids.push_back(c_.pid);
    return sendOk;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(c_.path == L"input" && !queriedPids.empty() && queriedPids.back() == c_.pid);
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    ++reads;
    if (readDelayMs) Sleep((DWORD)(timeoutMs < readDelayMs ? timeoutMs : readDelayMs));
    auto& pending = c_.pid == 0x1090 ? wiredReplies : replies;
    if (pending.empty()) return -1;
    out = pending.front();
    pending.erase(pending.begin());
    return (int)out.size();
}
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}

int main() {
    auto ctx = Context();
    replies = { Reply() };
    PollDareu(ctx);
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x260D, 0x1074));
    const auto& s = ctx.out[0];
    assert(s.name == L"DAREU EM901X" && s.key == L"dareu:260D:1074");
    assert(s.kind == DeviceKind::Mouse && s.via == L"2.4 GHz receiver");
    assert(s.percent == 85 && !s.charging && s.connected && !s.asleep);
    assert(opens == 2 && sends == 1 && reads == 1);
    PollDareu(ctx);
    assert(sends == 1);  // already claimed

    for (BYTE percent : { BYTE(0), BYTE(100) }) {
        ctx = Context();
        replies = { Reply(percent, 2) };
        PollDareu(ctx);
        assert(ctx.out.size() == 1 && ctx.out[0].percent == percent && ctx.out[0].charging);
    }

    // Ignore unrelated, truncated, failed and out-of-range reports before a valid reply.
    ctx = Context();
    auto wrongId = Reply(), wrongCommand = Reply(), failed = Reply(), shortReply = Reply(), longReply = Reply();
    wrongId[0] = 8;
    wrongCommand[1] = 3;
    failed[2] = 1;
    shortReply.resize(7);
    longReply.push_back(0);
    replies = { wrongId, wrongCommand, failed, shortReply, longReply, Reply(101), Reply(42, 1) };
    PollDareu(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 42 && ctx.out[0].charging);
    assert(sends == 1 && reads == 7);

    ctx = Context();
    replies = { wrongId, failed, Reply(255) };
    PollDareu(ctx);
    assert(ctx.out.empty() && ctx.claimed.empty() && sends == 1);

    ctx = Context();
    replies.assign(33, wrongCommand);
    PollDareu(ctx);
    assert(ctx.out.empty() && reads == 32 && sends == 1);  // bounded even under constant traffic

    ctx = Context();
    replies.assign(32, wrongCommand);
    readDelayMs = 80;
    ULONGLONG started = GetTickCount64();
    PollDareu(ctx);
    ULONGLONG elapsed = GetTickCount64() - started;
    assert(ctx.out.empty() && ctx.claimed.empty() && sends == 1 && reads < 32);
    assert(elapsed >= 900 && elapsed < 1500);  // unrelated reports must not restart the one-second deadline

    ctx = Context();
    PollDareu(ctx);
    assert(ctx.out.empty() && ctx.claimed.empty() && sends == 1);  // silence is not 0%
    ctx = Context();
    sendOk = false;
    PollDareu(ctx);
    assert(ctx.out.empty() && sends == 1 && reads == 0);
    ctx = Context();
    openOk = false;
    PollDareu(ctx);
    assert(ctx.out.empty() && sends == 0);
    ctx = Context();
    featureOpenOk = false;
    PollDareu(ctx);
    assert(ctx.out.empty() && opens == 2 && sends == 0 && reads == 0);

    ctx = Context(0x1090);
    wiredReplies = { Reply(85, 1) };
    PollDareu(ctx);
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x260D, 0x1090));
    assert(ctx.out[0].key == L"dareu:260D:1090" && ctx.out[0].via == L"USB cable");
    assert(ctx.out[0].percent == 85 && ctx.out[0].charging);
    assert(opens == 2 && sends == 1 && reads == 1);
    PollDareu(ctx);
    assert(sends == 1);

    ctx = Context();
    ctx.colls.push_back(Collection(true, 0x1090));
    ctx.colls.push_back(Collection(false, 0x1090));
    wiredReplies = { Reply(85, 1) };
    replies = { Reply(80) };
    PollDareu(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].via == L"USB cable");
    assert(ctx.out[0].percent == 85 && sends == 1 && queriedPids[0] == 0x1090);
    assert(replies.size() == 1);  // do not also query the cached receiver

    ctx = Context();
    ctx.colls.push_back(Collection(true, 0x1090));
    ctx.colls.push_back(Collection(false, 0x1090));
    replies = { Reply(80) };
    PollDareu(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 80 && sends == 2 && opens == 4);
    assert(queriedPids[0] == 0x1090 && queriedPids[1] == 0x1074);

    ctx = Context(0x1090);
    wiredReplies = { wrongId, failed, shortReply, Reply(101), Reply(54) };
    PollDareu(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 54 && !ctx.out[0].charging && reads == 5);

    ctx = Context(0x1090);
    ctx.colls.push_back(ctx.colls[0]);
    PollDareu(ctx);
    assert(ctx.out.empty() && opens == 0 && sends == 0);

    for (int invalid = 0; invalid < 11; invalid++) {
        ctx = Context();
        if (invalid == 0) ctx.colls[0].pid = 0x1090;
        if (invalid == 1) ctx.colls[1].vid = 0x3554;
        if (invalid == 2) ctx.colls[0].caps.UsagePage = 0xFF03;
        if (invalid == 3) ctx.colls[1].caps.Usage = 0;
        if (invalid == 4) ctx.colls[0].caps.InputReportByteLength = 8;
        if (invalid == 5) ctx.colls[1].caps.FeatureReportByteLength = 65;
        if (invalid == 6) ctx.colls[0].iface = 2;
        if (invalid == 7) ctx.colls.push_back(ctx.colls[0]);
        if (invalid == 8) ctx.colls.push_back(ctx.colls[1]);
        if (invalid == 9) ctx.colls.pop_back();
        if (invalid == 10) ctx.colls[0].pid = ctx.colls[1].pid = 0x1091;
        PollDareu(ctx);
        assert(ctx.out.empty() && ctx.claimed.empty() && opens == 0 && sends == 0);
    }
    puts("DAREU driver tests passed");
}
