// Simulated HID transport exercising PollArbiter alongside PollLamzu on shared vendor id 373E.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {

std::vector<std::vector<BYTE>> replies;
int opens = 0, sends = 0, reads = 0;
bool openOk = true, sendOk = true;

HidColl MakeColl(USHORT vid, USHORT pid, USHORT page = 0xFFFF, USHORT usage = 0, USHORT featLen = 65) {
    HidColl c;
    c.vid = vid;
    c.pid = pid;
    c.iface = 2;
    c.path = L"test";
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.FeatureReportByteLength = featLen;
    return c;
}

std::vector<BYTE> MakeReply(BYTE status, BYTE charging, BYTE percent, bool withReportId = true) {
    std::vector<BYTE> r(65, 0);
    if (withReportId) {
        r[0] = 0x00;
        r[1] = status;
        r[3] = 0x02;
        r[4] = 0x02;
        r[6] = 0x83;
        r[7] = charging;
        r[8] = percent;
    } else {
        r[0] = status;
        r[2] = 0x02;
        r[3] = 0x02;
        r[5] = 0x83;
        r[6] = charging;
        r[7] = percent;
    }
    return r;
}

void Reset() {
    replies.clear();
    opens = sends = reads = 0;
    openOk = sendOk = true;
}

}  // namespace

bool HidDev::OpenFeatureOnly(const HidColl& c) {
    ++opens;
    if (!openOk) return false;
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    return true;
}

void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }

bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    // Both PollArbiter and PollLamzu must only ever send the read-only 0x83 battery status query.
    std::vector<BYTE> expected(65, 0);
    expected[3] = 0x02;
    expected[4] = 0x02;
    expected[6] = 0x83;
    assert(data == expected);
    ++sends;
    return sendOk;
}

bool HidDev::GetFeature(BYTE reportId, std::vector<BYTE>& out) {
    assert(reportId == 0x00);
    ++reads;
    if (replies.empty()) return false;
    out = replies.front();
    replies.erase(replies.begin());
    return true;
}

std::wstring Hex4(USHORT v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v);
    return b;
}

int main() {
    // 1. Akitsu Medium 8K receiver (373E:0067) must be claimed by PollArbiter, not PollLamzu.
    Reset();
    replies = { MakeReply(0xA1, 0, 84) };
    PollContext ctx;
    ctx.colls = { MakeColl(0x373E, 0x0067) };
    PollArbiter(ctx);
    PollLamzu(ctx);
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].key == L"arbiter:373E:0067");
    assert(ctx.out[0].name == L"Arbiter Studio Akitsu Medium");
    assert(ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 84 && !ctx.out[0].charging && ctx.out[0].connected && !ctx.out[0].asleep);
    assert(ctx.out[0].graceSec == 1800);
    assert(sends == 1 && reads == 1);

    // 2. All three Akitsu models (3395, 3950, Medium) on receiver and cable, including headerless reply.
    struct Case { USHORT pid; const wchar_t* name; const wchar_t* via; bool headerless; };
    const Case cases[] = {
        { 0x000A, L"Arbiter Studio Akitsu", L"2.4 GHz receiver", false },
        { 0x0009, L"Arbiter Studio Akitsu", L"USB cable", true },
        { 0x0039, L"Arbiter Studio Akitsu 3950", L"2.4 GHz receiver", false },
        { 0x0038, L"Arbiter Studio Akitsu 3950", L"USB cable", false },
        { 0x0066, L"Arbiter Studio Akitsu Medium", L"USB cable", true },
    };
    for (const auto& tc : cases) {
        Reset();
        replies = { MakeReply(0xA1, 1, 95, !tc.headerless) };
        PollContext c;
        c.colls = { MakeColl(0x373E, tc.pid) };
        PollArbiter(c);
        PollLamzu(c);
        assert(c.out.size() == 1);
        assert(c.out[0].name == tc.name && c.out[0].via == tc.via);
        assert(c.out[0].percent == 95 && c.out[0].charging);
    }

    // 3. Cable + 8K receiver plugged in simultaneously: cable wins, suppresses receiver duplicate,
    //    and shares the same key as the receiver so the running UI updates in place (no graceSec ghost).
    Reset();
    replies = { MakeReply(0xA1, 1, 62) };
    ctx = {};
    ctx.colls = { MakeColl(0x373E, 0x0067), MakeColl(0x373E, 0x0066) };
    PollArbiter(ctx);
    PollLamzu(ctx);
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].key == L"arbiter:373E:0067" && ctx.out[0].via == L"USB cable");
    assert(ctx.out[0].graceSec == 1800);
    assert(ctx.IsClaimed(0x373E, 0x0066) && ctx.IsClaimed(0x373E, 0x0067));
    assert(opens == 1);

    // 4. Asleep status (0xA0) and switched-off dongle (percent 0).
    Reset();
    replies = { MakeReply(0xA0, 0, 0) };
    ctx = {};
    ctx.colls = { MakeColl(0x373E, 0x0067) };
    PollArbiter(ctx);
    PollLamzu(ctx);
    assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent == -1);

    Reset();
    replies = { MakeReply(0xA1, 0, 0) };
    ctx = {};
    ctx.colls = { MakeColl(0x373E, 0x0067) };
    PollArbiter(ctx);
    PollLamzu(ctx);
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && ctx.out[0].percent == -1);

    // 5. Silent / echoed Akitsu dongle must finish under 1s and STILL prevent PollLamzu mislabeling.
    Reset();
    std::vector<BYTE> echo(65, 0);
    echo[3] = 0x02; echo[4] = 0x02; echo[6] = 0x83;
    replies.assign(6, echo);
    ctx = {};
    ctx.colls = { MakeColl(0x373E, 0x0067) };
    ULONGLONG t0 = GetTickCount64();
    PollArbiter(ctx);
    ULONGLONG elapsed = GetTickCount64() - t0;
    PollLamzu(ctx);
    assert(ctx.out.empty() && ctx.IsClaimed(0x373E, 0x0067));
    assert(sends == 2 && reads == 6 && elapsed < 1000);

    // 6. LAMZU coexistence: LAMZU Maya X on 373E:001E, Maya X V2 on 373E:0016, and 37B0 mice
    //    alongside an Akitsu 373E:0067 all report with their proper names and drivers.
    Reset();
    replies = {
        MakeReply(0xA1, 0, 84),  // Akitsu Medium 373E:0067
        MakeReply(0xA1, 0, 71),  // LAMZU Maya X V2 373E:0016
        MakeReply(0xA1, 1, 50),  // LAMZU Maya X 373E:001E
        MakeReply(0xA1, 0, 90),  // LAMZU Thorn 37B0:0019
    };
    ctx = {};
    ctx.colls = {
        MakeColl(0x373E, 0x0067),
        MakeColl(0x373E, 0x0016),
        MakeColl(0x373E, 0x001E),
        MakeColl(0x37B0, 0x0019),
    };
    PollArbiter(ctx);
    PollLamzu(ctx);
    assert(ctx.out.size() == 4);
    assert(ctx.out[0].name == L"Arbiter Studio Akitsu Medium" && ctx.out[0].percent == 84);
    assert(ctx.out[1].name == L"LAMZU Maya X V2" && ctx.out[1].percent == 71);
    assert(ctx.out[2].name == L"LAMZU Maya X" && ctx.out[2].percent == 50 && ctx.out[2].charging);
    assert(ctx.out[3].name == L"LAMZU Thorn" && ctx.out[3].percent == 90);

    // 7. Strict exact matching: PollArbiter never opens unrelated collections or non-Akitsu PIDs.
    Reset();
    ctx = {};
    ctx.colls = {
        MakeColl(0x1234, 0x0067),                  // wrong VID
        MakeColl(0x373E, 0x001E),                  // LAMZU PID, not Akitsu
        MakeColl(0x373E, 0x0067, 0x0001, 2, 65),   // standard mouse page, not FFFF
        MakeColl(0x373E, 0x0067, 0xFFFF, 0, 64),   // wrong feature report length
    };
    PollArbiter(ctx);
    assert(opens == 0 && ctx.out.empty() && ctx.claimed.empty());

    puts("Arbiter Studio Akitsu + LAMZU coexistence tests passed");
}
