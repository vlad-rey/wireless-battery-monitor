// Exercise the Finalmouse driver with a simulated HID transport; no hardware access.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <set>

namespace {
// Scripted dongle: writing wake/all on a collection queues that collection's status burst.
std::map<std::wstring, std::vector<std::vector<BYTE>>> bursts;
std::vector<std::vector<BYTE>> pending;
std::vector<std::pair<std::wstring, std::vector<BYTE>>> writes;
std::set<std::wstring> rejectWrites;  // collections without the output report the driver uses
int opens = 0;
std::wstring openPath;

HidColl Collection(USHORT pid, const wchar_t* path, USHORT usage = 1, USHORT page = 0xFF00) {
    HidColl c;
    c.path = path;
    c.vid = 0x361D;
    c.pid = pid;
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.InputReportByteLength = 64;
    c.caps.OutputReportByteLength = 64;
    return c;
}

// Input report as Read() hands it over: [report id][2 + n][command][n][payload], zero-padded.
std::vector<BYTE> Reply(BYTE id, BYTE command, std::vector<BYTE> payload) {
    std::vector<BYTE> r(64, 0);
    r[0] = id;
    r[1] = (BYTE)(2 + payload.size());
    r[2] = command;
    r[3] = (BYTE)payload.size();
    for (size_t i = 0; i < payload.size(); ++i) r[4 + i] = payload[i];
    return r;
}
std::vector<BYTE> Ulx(BYTE command, std::vector<BYTE> payload) { return Reply(0x05, command, payload); }
std::vector<BYTE> Slx(BYTE command, std::vector<BYTE> payload) { return Reply(0x02, command, payload); }
std::vector<BYTE> Millivolts(int mv) { return { (BYTE)(mv & 0xFF), (BYTE)(mv >> 8) }; }

PollContext Poll(std::vector<HidColl> colls, std::map<std::wstring, std::vector<std::vector<BYTE>>> data) {
    bursts = std::move(data);
    pending.clear();
    writes.clear();
    PollContext ctx;
    ctx.colls = std::move(colls);
    PollFinalmouse(ctx);
    return ctx;
}

// The only requests the driver may send: empty-payload GETs, zero-padded by Write().
std::vector<BYTE> Get(BYTE id, BYTE command) { return { id, 2, (BYTE)(0x80 | command), 0 }; }
}  // namespace

// Stand-ins for the HID transport. The production driver is linked unchanged.
bool HidDev::Open(const HidColl& c) {
    c_ = c;
#ifdef _WIN32
    h_ = reinterpret_cast<HANDLE>(1);
    overlapped_ = true;
#else
    fd_ = 1;
#endif
    openPath = c.path;
    pending.clear();
    ++opens;
    return true;
}
void HidDev::Close() {
#ifdef _WIN32
    h_ = INVALID_HANDLE_VALUE;
#else
    fd_ = -1;
#endif
}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    assert(data.size() == 4 && data[1] == 2 && (data[2] & 0x80) && data[3] == 0);  // GET, no payload
    writes.push_back({ openPath, data });
    if (rejectWrites.count(openPath)) return false;
    if (data[2] == (0x80 | 96)) {
        auto it = bursts.find(openPath);
        if (it != bursts.end()) pending = it->second;
    }
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
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
    // ULX: xpanel's first read (lift-off, 21) and wake/all (96) on output report 04; the reply burst
    // on input report 05 carries the voltage (3880 mV = 50% on xpanel's curve) and the charging state.
    auto ulx = Collection(0x0100, L"ulx");
    auto ctx = Poll({ ulx }, { { L"ulx", {
        Reply(0x03, 10, { '1', '.', '0', 0 }),  // dongle firmware on report 03: ignored
        Ulx(3, Millivolts(1600)),               // DPI
        Ulx(5, Millivolts(3880)),
        Ulx(37, { 1 }),
    } } });
    assert(writes.size() == 2 && writes[0].second == Get(0x04, 21) && writes[1].second == Get(0x04, 96));
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x361D, 0x0100));
    const auto& s = ctx.out[0];
    assert(s.percent == 50 && s.millivolts == 3880 && s.charging && !s.asleep && s.connected);
    assert(s.name == L"Finalmouse UltralightX" && s.key == L"finalmouse:0100");
    assert(s.kind == DeviceKind::Mouse && s.via == L"2.4 GHz receiver" && s.graceSec == 1800);
    int before = opens;
    PollFinalmouse(ctx);  // claimed devices are not queried again
    assert(opens == before && ctx.out.size() == 1);

    // Same calibration points as xpanel (and OpenMouse's tests); the charging state may come first.
    const int curve[][2] = { { 2950, 0 }, { 3000, 0 }, { 3640, 8 }, { 3880, 50 }, { 4276, 88 }, { 4380, 100 }, { 4450, 100 } };
    for (const auto& point : curve) {
        ctx = Poll({ ulx }, { { L"ulx", { Ulx(37, { 0 }), Ulx(5, Millivolts(point[0])) } } });
        assert(ctx.out.size() == 1 && ctx.out[0].percent == point[1] && !ctx.out[0].charging);
    }
    // A level without a charging report is still shown (the driver waits briefly, then gives up).
    ctx = Poll({ ulx }, { { L"ulx", { Ulx(5, Millivolts(4170)) } } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 75 && !ctx.out[0].charging);
    // A state of charge (38) wins over the voltage curve when the dongle sends one.
    ctx = Poll({ ulx }, { { L"ulx", { Ulx(5, Millivolts(3880)), Ulx(38, { 64, 0x2C, 0x0F }), Ulx(37, { 0 }) } } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 64 && ctx.out[0].millivolts == 3884);

    // Link down (36 = 0) and no level: the mouse is asleep or off; keep the last value.
    ctx = Poll({ ulx }, { { L"ulx", { Ulx(36, { 0 }), Ulx(5, Millivolts(0)) } } });
    assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent == -1 && ctx.out[0].millivolts == 0);
    assert(ctx.IsClaimed(0x361D, 0x0100));
    // Link up but no level, malformed or out-of-range frames, or silence: nothing is reported or claimed.
    for (auto burst : std::vector<std::vector<std::vector<BYTE>>>{
             { Ulx(36, { 1 }) },
             { Ulx(5, { 0x28 }) },                           // voltage frame without its second byte
             { Ulx(5, Millivolts(6000)) },                   // implausible voltage
             { Ulx(38, { 101, 0x2C, 0x0F }) },               // percentage above 100
             { Slx(5, Millivolts(3880)), Slx(37, { 1 }) },   // SLX report id on the ULX dongle
             { Reply(0x05, 5, {}) },                         // empty payload
             {} }) {
        ctx = Poll({ ulx }, { { L"ulx", burst } });
        assert(ctx.out.empty() && ctx.claimed.empty());
    }
    std::vector<BYTE> truncated = Ulx(5, Millivolts(3880));
    truncated[1] = 3;  // inner length too short for the 2-byte payload
    ctx = Poll({ ulx }, { { L"ulx", { truncated } } });
    assert(ctx.out.empty());

    // SLX: first read is the profile roster (57), everything on output report 01, replies on 02.
    // The state of charge from 38 is used; a voltage alone does not end the wait early.
    auto slx = Collection(0x0300, L"slx", 9);
    ctx = Poll({ slx }, { { L"slx", {
        Ulx(5, Millivolts(4380)),  // ULX report id: ignored on SLX
        Slx(16, Millivolts(1600)),
        Slx(5, Millivolts(3880)),
        Slx(38, { 76, 0x44, 0x0F }),
        Slx(37, { 0 }),
    } } });
    assert(writes.size() == 2 && writes[0].second == Get(0x01, 57) && writes[1].second == Get(0x01, 96));
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 76 && ctx.out[0].millivolts == 0x0F44);
    assert(ctx.out[0].name == L"Finalmouse Starlight X" && ctx.out[0].key == L"finalmouse:0300");
    assert(!ctx.out[0].charging && ctx.IsClaimed(0x361D, 0x0300));
    // Voltage only: fall back to xpanel's curve once the burst is over.
    ctx = Poll({ Collection(0x0301, L"slx2", 1) }, { { L"slx2", { Slx(37, { 1 }), Slx(5, Millivolts(4276)) } } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 88 && ctx.out[0].charging && ctx.out[0].key == L"finalmouse:0301");

    // SLX: the usage is not pinned down. Usage 1 is tried first; a collection that rejects
    // output report 01 or stays silent is skipped and the next one is used.
    rejectWrites = { L"slx-a" };
    ctx = Poll({ Collection(0x0300, L"slx-b", 2), Collection(0x0300, L"slx-c", 3), Collection(0x0300, L"slx-a", 1) },
               { { L"slx-c", { Slx(38, { 40, 0, 0 }) } } });
    assert(writes.size() == 1 + 2 + 2 && writes[0].first == L"slx-a" && writes[1].first == L"slx-b");
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 40 && ctx.out[0].millivolts == 0);
    rejectWrites.clear();

    // Only the known dongles and their vendor collection are touched: not other vendors, the mice on
    // their cable, the ULX dongle's other collections, or another usage page.
    before = opens;
    ctx = Poll({ Collection(0x0102, L"ulx-cable"), Collection(0x0310, L"slx-cable"), Collection(0x0100, L"ulx-u2", 2),
                 Collection(0x0300, L"slx-ff01", 1, 0xFF01), Collection(0x0100, L"ulx-mouse", 2, 0x0001) },
               {});
    auto otherVendor = Collection(0x0100, L"other");
    otherVendor.vid = 0x361E;
    auto noOutput = Collection(0x0100, L"no-output");
    noOutput.caps.OutputReportByteLength = 0;
    ctx = Poll({ otherVendor, noOutput }, {});
    assert(opens == before && writes.empty() && ctx.out.empty());

    puts("Finalmouse driver tests passed");
}
