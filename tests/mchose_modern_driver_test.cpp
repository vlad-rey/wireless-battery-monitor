// Exercise the production MCHOSE "new" driver with simulated replies; no hardware access.
#include "../src/drivers/drivers.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> pending;
int opens = 0;
int writes = 0;

HidColl Collection(USHORT pid = 0x1014, USHORT page = 0xFF01, USHORT outLen = 64, USHORT inLen = 64) {
    HidColl c;
    c.path = L"mchose";
    c.vid = 0x3837;
    c.pid = pid;
    c.caps.UsagePage = page;
    c.caps.Usage = 0x01;
    c.caps.InputReportByteLength = inLen;
    c.caps.OutputReportByteLength = outLen;
    return c;
}

// Device-info reply, laid out the way the receiver sends it: header, 16 data bytes, checksum.
std::vector<BYTE> Reply(BYTE link, BYTE charge, BYTE percent) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x4D;
    r[1] = 0x01;
    r[2] = 0x01;  // read
    r[3] = 16;    // data length
    r[5] = 0x09;  // command 0x0900, device info
    r[8] = 0x37;
    r[9] = 0x38;  // vendor id 3837
    r[10] = 0x26;
    r[11] = 0x40;  // paired model id
    r[18] = link;
    r[19] = charge;
    r[20] = percent;
    for (int i = 2; i <= 7 + 16; i++) r[8 + 16] ^= r[i];
    return r;
}

// What the receiver answered on real hardware while the mouse was switched off and on again.
std::vector<BYTE> NotLinked() {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x4D;
    r[1] = 0x01;
    r[2] = 0x01;
    r[3] = 0x01;
    r[5] = 0x09;
    r[8] = 0xFF;
    return r;
}

PollContext Poll(std::vector<std::vector<BYTE>> replies, const HidColl& c = Collection()) {
    pending = std::move(replies);
    writes = 0;
    PollContext ctx;
    ctx.colls = { c };
    PollMchoseModern(ctx);
    return ctx;
}
}  // namespace

// Stand-ins for the Win32 HID transport. The production driver is linked unchanged.
bool HidDev::Open(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    ++opens;
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
// The scripted replies stand for what the receiver will answer next, so there is nothing stale to drop.
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    // The only frame this driver may send: the read-only device-info request.
    const std::vector<BYTE> expected = { 0x4D, 0x01, 0x01, 0x00, 0x00, 0x09, 0x00, 0x00, 0x08 };
    assert(data.size() == 64);
    assert(std::equal(expected.begin(), expected.end(), data.begin()));
    assert(std::find_if(data.begin() + 9, data.end(), [](BYTE b) { return b != 0; }) == data.end());
    ++writes;
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    if (pending.empty()) return -1;
    out = pending.front();
    pending.erase(pending.begin());
    return (int)out.size();
}
std::wstring HidProduct(const HidColl&) { return L"MCHOSE K7 V2 Ultra+"; }
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

int main() {
    auto ctx = Poll({ Reply(2, 0, 29) });
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x3837, 0x1014));
    const auto& s = ctx.out[0];
    assert(s.percent == 29 && s.connected && !s.charging && !s.asleep);
    assert(s.name == L"MCHOSE K7 V2 Ultra+" && s.key == L"mchose:3837:1014");
    assert(s.kind == DeviceKind::Mouse && s.via == L"2.4 GHz receiver" && s.graceSec == 1800);
    assert(writes == 1);  // one reply is enough, no retries

    ctx = Poll({ Reply(2, 1, 100) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].charging);

    // Mouse switched off: the receiver answers, but without battery data.
    ctx = Poll({ NotLinked() });
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x3837, 0x1014));
    assert(ctx.out[0].asleep && !ctx.out[0].charging && ctx.out[0].percent == -1);
    assert(writes == 1);

    // The 0x13 echo frame the receiver emits after an unrelated write carries no battery either.
    std::vector<BYTE> echo(64, 0);
    echo[0] = 0x4D;
    echo[1] = 0x01;
    echo[2] = 0x01;
    echo[3] = 0x01;
    echo[8] = 0xFF;
    ctx = Poll({ echo, Reply(2, 0, 31) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 31);

    // Silence: three attempts, then nothing is reported or claimed.
    ctx = Poll({});
    assert(ctx.out.empty() && ctx.claimed.empty() && writes == 3);

    // Corrupted frames are not trusted.
    auto fixChecksum = [](std::vector<BYTE>& r) {
        r[8 + 16] = 0;
        for (int i = 2; i <= 7 + 16; i++) r[8 + 16] ^= r[i];
    };
    auto badChecksum = Reply(2, 0, 29);
    auto badPercent = Reply(2, 0, 101);
    auto badVendor = Reply(2, 0, 29);
    auto badReport = Reply(2, 0, 29);
    auto shortData = Reply(2, 0, 29);
    badChecksum[24] ^= 0xFF;
    badVendor[8] = 0x34;
    fixChecksum(badVendor);
    badReport[0] = 0x4E;
    shortData[3] = 12;
    fixChecksum(shortData);
    ctx = Poll({ badChecksum, badPercent, badVendor, badReport });
    assert(ctx.out.empty() && ctx.claimed.empty() && writes == 3);

    // A frame too short to hold the 14-byte payload means "not linked", like the 0xFF answer.
    ctx = Poll({ shortData });
    assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent == -1);

    // Only the vendor collections of the documented receivers are opened.
    int before = opens;
    ctx = Poll({}, Collection(0x1012, 0xFF00));   // MagDock: a different product
    assert(ctx.out.empty() && opens == before);
    ctx = Poll({}, Collection(0x1014, 0x0001));   // mouse collection, not the vendor one
    assert(ctx.out.empty() && opens == before);
    ctx = Poll({}, Collection(0x1014, 0xFF01, 63, 64));  // too short to hold the frame
    assert(ctx.out.empty() && opens == before);
    ctx = Poll({}, Collection(0x1014, 0xFF01, 64, 20));  // reply cannot be read
    assert(ctx.out.empty() && opens == before);

    // The other receivers M HUB groups with this one, and the FF0B collection, are accepted too.
    ctx = Poll({ Reply(2, 0, 45) }, Collection(0x1018, 0xFF0B));
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 45 && ctx.out[0].key == L"mchose:3837:1018");
    ctx = Poll({ Reply(2, 0, 45) }, Collection(0x102D, 0xFF0B));
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"mchose:3837:102D");

    puts("MCHOSE modern driver tests passed");
}
