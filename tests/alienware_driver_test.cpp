// Production Alienware AW920H poller with a simulated HID transport; replies captured from an AW920H
// on its 413C:A515 dongle (and seen in Alienware Command Center's packet log).
#include "../src/drivers/drivers.h"

#include <cstdio>

void PollAlienware(PollContext& ctx);

namespace {
std::vector<BYTE> Report(std::initializer_list<BYTE> head) {
    std::vector<BYTE> r(head);
    r.resize(62, 0);
    return r;
}
// 95%, checksums 11 and 06 (XOR of bytes 3..12).
const std::vector<BYTE> kCaptured = Report({ 0x07, 0xC0, 0x0A, 0x09, 0x00, 0x01, 0x5F, 0x00, 0x00, 0x0B, 0x10, 0x5F, 0x02, 0x11 });
const std::vector<BYTE> kCaptured2 = Report({ 0x07, 0xC0, 0x0A, 0x09, 0x00, 0x01, 0x5F, 0x00, 0x00, 0x1C, 0x10, 0x5F, 0x02, 0x06 });
const std::vector<BYTE> kBusy = Report({ 0x07, 0x00, 0x80 });  // no answer yet / headset off

int failures = 0, opens = 0, sends = 0, inputReads = 0;
bool openOk = true, getOk = true, repeatReply = false;
int answerOnSend = 1;  // the dongle drops queries before this one: they are only ever answered "busy"
std::vector<BYTE> lastSend;
std::vector<BYTE> interruptReply;  // what SetOutputAndRead returns (empty = read timed out)
std::vector<std::vector<BYTE>> replies;
const char* current = "";

void Check(bool result, const char* expression, int line) {
    if (!result) { std::printf("FAIL %s:%d %s\n", current, line, expression); ++failures; }
}
#define CHECK(x) Check((x), #x, __LINE__)

// A battery reply with a correct checksum, laid out like the captured one.
// [6] level, [7] charging, [8] critical, [11] level used when critical, [12] charge mode.
std::vector<BYTE> Battery(BYTE pct, BYTE charging = 0, BYTE critical = 0, int criticalPct = -1, BYTE mode = 0x02) {
    BYTE pct11 = criticalPct < 0 ? pct : (BYTE)criticalPct;
    auto r = Report({ 0x07, 0xC0, 0x0A, 0x09, 0x00, 0x01, pct, charging, critical, 0x1C, 0x10, pct11, mode });
    BYTE x = 0;
    for (int i = 3; i <= 12; i++) x ^= r[i];
    r[13] = x;
    return r;
}

HidColl Collection() {
    HidColl c;
    c.vid = 0x413C;
    c.pid = 0xA515;
    c.iface = 5;
    c.path = L"dongle-col02";
    c.caps.UsagePage = 0xFF13;
    c.caps.Usage = 0x0001;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = 62;
    c.caps.FeatureReportByteLength = 3;
    return c;
}

// The dongle's consumer-control collection on the same interface.
HidColl ConsumerCollection() {
    HidColl c = Collection();
    c.path = L"dongle-col01";
    c.caps.UsagePage = 0x000C;
    c.caps.InputReportByteLength = 2;
    c.caps.OutputReportByteLength = c.caps.FeatureReportByteLength = 0;
    return c;
}

PollContext Context() {
    opens = sends = inputReads = 0;
    openOk = getOk = true;
    repeatReply = false;
    answerOnSend = 1;
    lastSend.clear();
    interruptReply.clear();
    replies.clear();
    PollContext ctx;
    ctx.colls = { ConsumerCollection(), Collection() };
    return ctx;
}

void Level(std::vector<std::vector<BYTE>> r, int percent, bool charging = false) {
    auto ctx = Context();
    replies = r;
    PollAlienware(ctx);
    CHECK(ctx.out.size() == 1 && ctx.IsClaimed(0x413C, 0xA515));
    CHECK(opens == 1 && sends == 1 && inputReads == (int)r.size());
    if (ctx.out.size() == 1) {
        const auto& s = ctx.out[0];
        if (s.percent != percent || s.charging != charging) std::printf("  got %d%% charging=%d\n", s.percent, (int)s.charging);
        CHECK(s.percent == percent && s.charging == charging && s.connected);
    }
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    ++opens;
    c_ = c;
    CHECK(c.path == L"dongle-col02");  // never the consumer-control collection
    return openOk;
}
void HidDev::Close() {}
int HidDev::SetOutputAndRead(const std::vector<BYTE>& data, std::vector<BYTE>& buf, int timeoutMs) {
    ++sends;
    lastSend = data;
    CHECK(timeoutMs > 0 && timeoutMs <= 100);
    if (interruptReply.empty()) return -1;
    buf = interruptReply;
    return (int)buf.size();
}
bool HidDev::GetInputReport(BYTE reportId, std::vector<BYTE>& out) {
    ++inputReads;
    CHECK(reportId == 0x07);
    if (!getOk) return false;
    if (replies.empty() || sends < answerOnSend) { out = kBusy; return true; }  // the dongle keeps answering "busy"
    out = replies.front();
    if (!repeatReply) replies.erase(replies.begin());
    return true;
}
std::wstring Hex4(USHORT v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v);
    return b;
}

int main() {
    current = "test helper reproduces the captured frame";
    CHECK(Battery(0x5F) == kCaptured2);

    current = "captured 95% after two busy reads, identity";
    {
        auto ctx = Context();
        replies = { kBusy, kBusy, kCaptured };
        PollAlienware(ctx);
        CHECK(ctx.out.size() == 1 && sends == 1 && inputReads == 3);
        auto expected = Report({ 0x06, 0xC0, 0x0A });  // Command Center's read-only battery query
        CHECK(lastSend == expected);
        if (ctx.out.size() == 1) {
            const auto& s = ctx.out[0];
            CHECK(s.percent == 95 && s.connected && !s.charging && !s.asleep);
            CHECK(s.key == L"alienware:A515");
            CHECK(s.name == L"Alienware AW920H");
            CHECK(s.kind == DeviceKind::Headset);
            CHECK(s.via == L"2.4 GHz dongle");
        }
        auto before = opens;
        PollAlienware(ctx);  // already claimed: no second query
        CHECK(opens == before && ctx.out.size() == 1);
    }
    current = "second captured frame";
    Level({ kBusy, kCaptured2 }, 95);
    current = "1% and 100% are valid levels";
    Level({ Battery(1) }, 1);
    Level({ Battery(100) }, 100);
    current = "level 0 is rejected like Command Center does, later valid reply used";
    Level({ Battery(0), Battery(37) }, 37);

    current = "level is byte 6, not the critical-level byte 11";
    Level({ Battery(80, 0, 0, 79) }, 80);
    current = "battery critical: level taken from byte 11";
    Level({ Battery(10, 0, 1, 4) }, 4);

    current = "charging flag, normal and quick charge";
    Level({ Battery(60, 1, 0, -1, 0x00) }, 60, true);
    Level({ Battery(60, 1, 0, -1, 0x01) }, 60, true);
    Level({ Battery(60, 1, 0, -1, 0x02) }, 60, true);
    Level({ Battery(60, 2) }, 60, false);  // only 1 means charging

    current = "reply that also arrives on the interrupt pipe is used directly";
    {
        auto ctx = Context();
        interruptReply = kCaptured;
        PollAlienware(ctx);
        CHECK(ctx.out.size() == 1 && inputReads == 0);
        if (ctx.out.size() == 1) CHECK(ctx.out[0].percent == 95);
        ctx = Context();
        interruptReply = kBusy;  // not an answer: fall back to polling
        replies = { kCaptured };
        PollAlienware(ctx);
        CHECK(ctx.out.size() == 1 && inputReads == 1);
    }

    current = "malformed replies skipped before a valid one";
    {
        auto badChecksum = kCaptured, wrongId = kCaptured, wrongCmd = kCaptured, longLen = kCaptured, shortLen = kCaptured;
        badChecksum[13] ^= 0x01;
        wrongId[0] = 0x08;
        wrongCmd[2] = 0x0B;
        longLen[3] = 0x3A;  // checksum would sit past the end of the report
        shortLen[3] = 0x02;
        auto ctx = Context();
        replies = { badChecksum, wrongId, wrongCmd, longLen, shortLen, Battery(101), std::vector<BYTE>(5), Battery(42) };
        PollAlienware(ctx);
        CHECK(ctx.out.size() == 1 && inputReads == 8);
        if (ctx.out.size() == 1) CHECK(ctx.out[0].percent == 42);
    }

    current = "dropped query: resent, answered on the second or third try";
    for (int answeredBy : { 2, 3 }) {
        auto ctx = Context();
        answerOnSend = answeredBy;
        replies = { kCaptured };
        auto start = GetTickCount64();
        PollAlienware(ctx);
        CHECK(GetTickCount64() - start < 800);
        CHECK(ctx.out.size() == 1 && sends == answeredBy);
        if (ctx.out.size() == 1) CHECK(ctx.out[0].percent == 95 && ctx.out[0].connected);
    }

    current = "headset off: three tries, then left out (tray grace keeps the last level) but claimed";
    for (bool failing : { false, true }) {
        auto ctx = Context();
        getOk = !failing;
        auto start = GetTickCount64();
        PollAlienware(ctx);
        CHECK(GetTickCount64() - start < 800);
        CHECK(ctx.out.empty() && ctx.IsClaimed(0x413C, 0xA515) && sends == 3);
    }
    current = "endless malformed replies also bounded";
    {
        auto ctx = Context();
        auto bad = kCaptured;
        bad[13] ^= 0x01;
        replies = { bad };
        repeatReply = true;
        auto start = GetTickCount64();
        PollAlienware(ctx);
        CHECK(GetTickCount64() - start < 800);
        CHECK(ctx.out.empty() && ctx.IsClaimed(0x413C, 0xA515));
    }
    current = "the graced entry keeps its identity once the headset answers again";
    {
        auto ctx = Context();
        replies = { kCaptured };
        PollAlienware(ctx);
        CHECK(ctx.out.size() == 1 && ctx.out[0].graceSec >= 60);
    }

    current = "strict matching: no I/O to other devices or collections";
    for (int field = 0; field < 7; ++field) {
        auto ctx = Context();
        auto& c = ctx.colls.back();
        switch (field) {
        case 0: c.vid = 0x413D; break;
        case 1: c.pid = 0xA514; break;  // the Bluetooth side, covered by PollBluetooth
        case 2: c.iface = 4; break;
        case 3: c.caps.UsagePage = 0xFF00; break;
        case 4: c.caps.Usage = 0x0002; break;
        case 5: c.caps.InputReportByteLength = 61; break;
        case 6: c.caps.OutputReportByteLength = 61; break;
        }
        replies = { kCaptured };
        PollAlienware(ctx);
        CHECK(ctx.out.empty() && ctx.claimed.empty() && opens == 0 && sends == 0);
    }
    {
        auto ctx = Context();
        ctx.Claim(0x413C, 0xA515);
        PollAlienware(ctx);
        CHECK(ctx.out.empty() && opens == 0);
    }

    current = "open failure: nothing reported or claimed";
    {
        auto ctx = Context();
        openOk = false;
        PollAlienware(ctx);
        CHECK(ctx.out.empty() && sends == 0 && ctx.claimed.empty());
    }

    if (failures) { std::printf("Alienware: %d failed checks\n", failures); return 1; }
    std::puts("Alienware AW920H driver tests passed (captured replies, interrupt path, checksum, charging, critical level, malformed, resend, headset off, strict matching, deadline).");
}
