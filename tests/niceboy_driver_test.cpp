// Production Niceboy MW400 poller with a simulated HID transport; replies captured from an MW400
// on its 248A:FA02 receiver.
#include "../src/drivers/drivers.h"

#include <cstdio>

void PollNiceboy(PollContext& ctx);

namespace {
std::vector<BYTE> Report(std::initializer_list<BYTE> head) {
    std::vector<BYTE> r(head);
    r.resize(33, 0);
    return r;
}
// raw 0x03EA = 1002 -> 4133 mV -> 90%; raw 0x03E9 = 1001 -> 4129 mV; raw 0x03EC = 1004 -> 4141 mV.
const std::vector<BYTE> kCaptured = Report({ 0x05, 0x49, 0xFA, 0x48, 0xEA, 0x03, 0x00, 0x00, 0x01, 0xFB, 0x05 });
const std::vector<BYTE> kCaptured2 = Report({ 0x05, 0x48, 0xFA, 0x48, 0xE9, 0x03, 0x00, 0x00, 0x01, 0xFB, 0x05 });
const std::vector<BYTE> kCaptured3 = Report({ 0x05, 0x57, 0xFA, 0x48, 0xEC, 0x03, 0x00, 0x00, 0x01, 0xFB, 0x05 });

int failures = 0, opens = 0, flushes = 0, writes = 0, reads = 0;
bool openOk = true, writeOk = true, repeatReply = false;
std::vector<BYTE> lastWrite;
std::vector<std::vector<BYTE>> replies;
const char* current = "";

void Check(bool result, const char* expression, int line) {
    if (!result) { std::printf("FAIL %s:%d %s\n", current, line, expression); ++failures; }
}
#define CHECK(x) Check((x), #x, __LINE__)

std::vector<BYTE> WithRaw(int raw, BYTE charging = 0) {
    auto r = kCaptured;
    r[4] = (BYTE)(raw & 0xFF);
    r[5] = (BYTE)(raw >> 8);
    r[6] = charging;
    return r;
}

HidColl Collection() {
    HidColl c;
    c.vid = 0x248A;
    c.pid = 0xFA02;
    c.iface = 0;
    c.path = L"receiver-col04";
    c.caps.UsagePage = 0x0001;
    c.caps.Usage = 0x0000;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = 33;
    return c;
}

// The receiver's other collections: mouse, consumer control, system control, keyboard.
std::vector<HidColl> OtherCollections() {
    std::vector<HidColl> v;
    const struct { int iface; USHORT page, usage, in, out; } k[] = {
        { 0, 0x0001, 0x0002, 7, 0 }, { 0, 0x000C, 0x0001, 3, 0 }, { 0, 0x0001, 0x0080, 2, 0 }, { 1, 0x0001, 0x0006, 9, 2 },
    };
    for (auto& e : k) {
        HidColl c = Collection();
        c.iface = e.iface;
        c.caps.UsagePage = e.page;
        c.caps.Usage = e.usage;
        c.caps.InputReportByteLength = e.in;
        c.caps.OutputReportByteLength = e.out;
        c.path = L"other";
        v.push_back(c);
    }
    return v;
}

PollContext Context() {
    opens = flushes = writes = reads = 0;
    openOk = writeOk = true;
    repeatReply = false;
    lastWrite.clear();
    replies.clear();
    PollContext ctx;
    ctx.colls = OtherCollections();
    ctx.colls.push_back(Collection());
    return ctx;
}

void Level(const std::vector<BYTE>& reply, int percent, int millivolts, bool charging) {
    auto ctx = Context();
    replies = { reply };
    PollNiceboy(ctx);
    CHECK(ctx.out.size() == 1);
    CHECK(ctx.IsClaimed(0x248A, 0xFA02));
    CHECK(opens == 1 && writes == 1 && reads == 1);
    if (ctx.out.size() == 1) {
        const auto& s = ctx.out[0];
        if (s.percent != percent || s.millivolts != millivolts)
            std::printf("  got %d%% %d mV, expected %d%% %d mV\n", s.percent, s.millivolts, percent, millivolts);
        CHECK(s.percent == percent && s.millivolts == millivolts);
        CHECK(s.charging == charging && s.connected && !s.asleep);
    }
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    ++opens;
    c_ = c;
    CHECK(c.path == L"receiver-col04");  // never the mouse / keyboard collections
    return openOk;
}
void HidDev::Close() {}
void HidDev::Flush() { ++flushes; }
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    ++writes;
    lastWrite = data;
    return writeOk;
}
int HidDev::Read(std::vector<BYTE>& buf, int timeoutMs) {
    ++reads;
    CHECK(timeoutMs > 0 && timeoutMs <= 500);
    if (replies.empty()) return -1;
    buf = replies.front();
    if (repeatReply) Sleep(1);
    else replies.erase(replies.begin());
    return (int)buf.size();
}
std::wstring Hex4(USHORT v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v);
    return b;
}

int main() {
    current = "captured 90% and identity";
    {
        auto ctx = Context();
        replies = { kCaptured };
        PollNiceboy(ctx);
        CHECK(ctx.out.size() == 1 && flushes == 1);
        CHECK((lastWrite == std::vector<BYTE>{ 0x05, 0x24, 0xFA, 0x48 }));  // the vendor app's read-only query
        if (ctx.out.size() == 1) {
            const auto& s = ctx.out[0];
            CHECK(s.percent == 90 && s.millivolts == 4133 && !s.charging && s.connected && !s.asleep);
            CHECK(s.key == L"niceboy:FA02");
            CHECK(s.name == L"Niceboy MW400");
            CHECK(s.kind == DeviceKind::Mouse);
            CHECK(s.via == L"2.4 GHz receiver");
        }
        auto before = opens;
        PollNiceboy(ctx);  // already claimed: no second query
        CHECK(opens == before && ctx.out.size() == 1);
    }
    current = "other captured replies";
    Level(kCaptured2, 90, 4129, false);
    Level(kCaptured3, 90, 4141, false);

    current = "vendor voltage table boundaries";
    Level(WithRaw(1019), 100, 4203, false);
    Level(WithRaw(1018), 90, 4199, false);
    Level(WithRaw(994), 90, 4100, false);
    Level(WithRaw(993), 80, 4096, false);
    Level(WithRaw(970), 80, 4001, false);
    Level(WithRaw(969), 70, 3997, false);
    Level(WithRaw(946), 70, 3902, false);
    Level(WithRaw(945), 60, 3898, false);
    Level(WithRaw(922), 60, 3803, false);
    Level(WithRaw(921), 50, 3799, false);
    Level(WithRaw(897), 50, 3700, false);
    Level(WithRaw(896), 40, 3696, false);
    Level(WithRaw(849), 40, 3502, false);
    Level(WithRaw(848), 30, 3498, false);
    Level(WithRaw(825), 30, 3403, false);
    Level(WithRaw(824), 20, 3399, false);
    Level(WithRaw(776), 20, 3201, false);
    Level(WithRaw(775), 10, 3196, false);
    Level(WithRaw(1), 10, 4, false);

    current = "charging flag";
    Level(WithRaw(1030, 1), 100, 4248, true);
    Level(WithRaw(900, 0x80), 50, 3712, true);

    current = "mouse asleep or off: receiver answers with zeros";
    {
        auto ctx = Context();
        replies = { WithRaw(0, 0) };
        PollNiceboy(ctx);
        CHECK(ctx.out.size() == 1 && ctx.IsClaimed(0x248A, 0xFA02));
        if (ctx.out.size() == 1) {
            const auto& s = ctx.out[0];
            CHECK(s.asleep && s.connected && s.percent == -1 && s.millivolts == 0 && !s.charging);
            CHECK(s.graceSec > 0);
        }
    }

    current = "unrelated report 05 traffic skipped before the battery reply";
    {
        auto otherType = kCaptured, otherEcho = kCaptured, otherReport = kCaptured;
        otherType[3] = 0x49;
        otherEcho[2] = 0xFB;
        otherReport[0] = 0x04;
        auto ctx = Context();
        replies = { otherType, otherEcho, otherReport, std::vector<BYTE>{ 0x05, 0x49, 0xFA, 0x48 }, kCaptured };
        PollNiceboy(ctx);
        CHECK(ctx.out.size() == 1 && reads == 5);
        if (ctx.out.size() == 1) CHECK(ctx.out[0].percent == 90);
    }

    current = "strict matching: no I/O to other devices or collections";
    for (int field = 0; field < 7; ++field) {
        PollContext ctx = Context();
        auto& c = ctx.colls.back();
        switch (field) {
        case 0: c.vid = 0x249A; break;
        case 1: c.pid = 0xFF4C; break;  // Machenike L7 receiver, same vendor id
        case 2: c.iface = 2; break;     // Ajazz AJ179 layout
        case 3: c.caps.UsagePage = 0xFF00; break;
        case 4: c.caps.Usage = 0x0002; break;
        case 5: c.caps.InputReportByteLength = 32; break;
        case 6: c.caps.OutputReportByteLength = 32; break;
        }
        replies = { kCaptured };
        PollNiceboy(ctx);
        CHECK(ctx.out.empty() && ctx.claimed.empty() && opens == 0 && writes == 0);
    }
    {
        auto ctx = Context();
        ctx.Claim(0x248A, 0xFA02);
        PollNiceboy(ctx);
        CHECK(ctx.out.empty() && opens == 0);
    }

    current = "transport failures";
    {
        auto ctx = Context();
        openOk = false;
        PollNiceboy(ctx);
        CHECK(ctx.out.empty() && writes == 0 && !ctx.IsClaimed(0x248A, 0xFA02));
        ctx = Context();
        writeOk = false;
        replies = { kCaptured };
        PollNiceboy(ctx);
        CHECK(ctx.out.empty() && writes == 1 && reads == 0);
    }

    current = "missing or endlessly unrelated reply has a bounded deadline";
    for (bool unrelated : { false, true }) {
        auto ctx = Context();
        if (unrelated) {
            auto other = kCaptured;
            other[3] = 0x49;
            replies = { other };
            repeatReply = true;
        }
        auto start = GetTickCount64();
        PollNiceboy(ctx);
        CHECK(GetTickCount64() - start < 800);
        CHECK(ctx.out.empty() && writes == 1);
    }

    if (failures) { std::printf("Niceboy: %d failed checks\n", failures); return 1; }
    std::puts("Niceboy driver tests passed (captured replies, voltage table, charging, asleep, filtering, strict matching, deadline).");
}
