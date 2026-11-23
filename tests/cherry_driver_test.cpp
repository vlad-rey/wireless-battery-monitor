// Exercise the production driver with simulated replies; no hardware access.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<std::vector<BYTE>>> script;  // replies queued per Write call
std::vector<std::vector<BYTE>> writes, pending;
int opens = 0;
bool openOk = true;

HidColl Collection(USHORT pid = 0x00EA, USHORT page = 0xFF1C, USHORT usage = 0x0092, USHORT len = 64) {
    HidColl c;
    c.path = L"test-dongle";
    c.vid = 0x046A;
    c.pid = pid;
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.InputReportByteLength = len;
    c.caps.OutputReportByteLength = len;
    return c;
}

std::vector<BYTE> Reply(BYTE percent, BYTE usb = 0, BYTE type = 0x1A) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x04;
    r[3] = type;
    r[8] = percent;
    r[9] = usb;
    return r;
}

PollContext Poll(std::vector<std::vector<std::vector<BYTE>>> perWrite, std::vector<HidColl> colls = { Collection() }) {
    script = std::move(perWrite);
    writes.clear();
    pending.clear();
    opens = 0;
    PollContext ctx;
    ctx.colls = std::move(colls);
    PollCherry(ctx);
    return ctx;
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    ++opens;
    if (!openOk) return false;
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    overlapped_ = true;
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
void HidDev::Flush() { pending.clear(); }
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    writes.push_back(data);
    size_t i = writes.size() - 1;
    if (i < script.size()) pending = script[i];
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 400);
    if (pending.empty()) return -1;
    out = pending.front();
    pending.erase(pending.begin());
    return (int)out.size();
}
std::wstring Hex4(USHORT v) {
    wchar_t s[8];
    swprintf(s, 8, L"%04X", v);
    return s;
}

int main() {
    // The only frame ever sent: report 4, checksum 0x0072 (= 0x1A + 3 + 0x55), type 0x1A, payload 03 00 00 55.
    std::vector<BYTE> expected(64, 0);
    expected[0] = 0x04;
    expected[1] = 0x72;
    expected[2] = 0x00;
    expected[3] = 0x1A;
    expected[4] = 0x03;
    expected[7] = 0x55;

    // Normal reply: 73%, on battery.
    auto ctx = Poll({ { Reply(73) } });
    assert(writes.size() == 1 && writes[0] == expected);
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].percent == 73 && !ctx.out[0].charging && !ctx.out[0].asleep);
    assert(ctx.out[0].kind == DeviceKind::Keyboard);
    assert(ctx.out[0].name == L"CHERRY MX 3.0S Wireless");
    assert(ctx.out[0].key == L"cherry:mx30s");
    assert(ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].graceSec > 0);
    assert(ctx.IsClaimed(0x046A, 0x00EA));

    // USB powered flag is shown as charging.
    ctx = Poll({ { Reply(100, 1) } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].charging);

    // Unrelated or malformed reports are skipped until the real status reply arrives.
    std::vector<BYTE> shortReply = { 0x04, 0, 0, 0x1A };
    ctx = Poll({ { Reply(50, 0, 0x2B), shortReply, Reply(42) } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 42);

    // No answer on the first try: the query is repeated once, and a late answer is accepted.
    ctx = Poll({ {}, { Reply(64) } });
    assert(writes.size() == 2 && writes[0] == expected && writes[1] == expected);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 64);

    // Keyboard off or asleep: the dongle stays silent, so nothing is reported (two tries, then give up).
    ctx = Poll({});
    assert(writes.size() == 2 && ctx.out.empty() && !ctx.IsClaimed(0x046A, 0x00EA));

    // Answer without a usable level: shown as asleep with an unknown percentage.
    ctx = Poll({ { Reply(0xFF) } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == -1 && ctx.out[0].asleep);

    // Exact matching: other product ids, other usages and the 9-byte FF1C collection are never opened.
    ctx = Poll({ { Reply(80) } }, { Collection(0x01E4) });
    assert(opens == 0 && writes.empty() && ctx.out.empty());
    ctx = Poll({ { Reply(80) } }, { Collection(0x00EA, 0xFF1C, 0x0092, 9) });
    assert(opens == 0 && writes.empty() && ctx.out.empty());
    ctx = Poll({ { Reply(80) } }, { Collection(0x00EA, 0xFF00, 0x0092) });
    assert(opens == 0 && writes.empty() && ctx.out.empty());
    ctx = Poll({ { Reply(80) } }, { Collection(0x00EA, 0xFF1C, 0x0001) });
    assert(opens == 0 && writes.empty() && ctx.out.empty());

    // With the 9-byte collection listed first, only the 64-byte one is used.
    ctx = Poll({ { Reply(55) } }, { Collection(0x00EA, 0xFF1C, 0x0092, 9), Collection() });
    assert(opens == 1 && ctx.out.size() == 1 && ctx.out[0].percent == 55);

    // USB cable (046A:00E3): same query, reported as the same keyboard entry.
    ctx = Poll({ { Reply(91, 1) } }, { Collection(0x00E3) });
    assert(writes.size() == 1 && writes[0] == expected);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 91 && ctx.out[0].charging);
    assert(ctx.out[0].via == L"USB cable" && ctx.out[0].key == L"cherry:mx30s");
    assert(ctx.IsClaimed(0x046A, 0x00E3));

    // Cable and dongle both attached: only one entry, and the cable is preferred.
    ctx = Poll({ { Reply(90, 1) } }, { Collection(0x00EA), Collection(0x00E3) });
    assert(opens == 1 && ctx.out.size() == 1 && ctx.out[0].via == L"USB cable" && ctx.out[0].percent == 90);

    // Cable present but silent: fall back to the dongle.
    ctx = Poll({ {}, {}, { Reply(60) } }, { Collection(0x00E3), Collection(0x00EA) });
    assert(writes.size() == 3 && ctx.out.size() == 1 && ctx.out[0].via == L"2.4 GHz receiver" && ctx.out[0].percent == 60);

    // The 9-byte collection of the cable is never used either.
    ctx = Poll({ { Reply(80) } }, { Collection(0x00E3, 0xFF1C, 0x0092, 9) });
    assert(opens == 0 && writes.empty() && ctx.out.empty());

    // Already claimed by another driver: untouched.
    {
        script = { { Reply(80) } };
        writes.clear();
        opens = 0;
        PollContext claimed;
        claimed.colls = { Collection() };
        claimed.Claim(0x046A, 0x00EA);
        PollCherry(claimed);
        assert(opens == 0 && writes.empty() && claimed.out.empty());
    }

    // Open failure (another program holds the channel): no device, no crash.
    openOk = false;
    ctx = Poll({ { Reply(80) } });
    assert(ctx.out.empty() && writes.empty());
    openOk = true;

    std::puts("cherry driver test passed");
    return 0;
}
