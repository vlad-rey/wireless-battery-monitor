// Exercise the real Aerox/Prime driver with a simulated HID transport; no hardware writes.
#include "../src/drivers/drivers.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "Check failed: %s (line %d)\n", expression, line);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

BYTE replyValue = 0, expectedCommand = 0xD2;
int replySize = 2;
bool writeOk = true;

HidColl Collection(USHORT pid) {
    HidColl c;
    c.vid = 0x1038;
    c.pid = pid;
    c.iface = 3;
    c.caps.UsagePage = 0xFFC0;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = 65;
    return c;
}

PollContext Poll(USHORT pid) {
    PollContext ctx;
    ctx.colls = { Collection(pid) };
    PollSteelSeries(ctx);
    return ctx;
}
}  // namespace

bool HidDev::Open(const HidColl& c) { c_ = c; h_ = reinterpret_cast<HANDLE>(1); return true; }
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    CHECK(data == std::vector<BYTE>({ 0x00, expectedCommand }));
    return writeOk;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    CHECK(timeoutMs > 0 && timeoutMs <= 300);
    out = { expectedCommand, replyValue };
    out.resize(replySize);
    return replySize ? replySize : -1;
}
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
int MapTo100(int v, int lo, int hi) { return Clamp100((v - lo) * 100 / (hi - lo)); }

int main() {
    struct Model { USHORT pid; bool wired; };
    const Model models[] = { { 0x1838, false }, { 0x183A, true }, { 0x1852, false }, { 0x1854, true },
                             { 0x1858, false }, { 0x185A, true }, { 0x1840, false }, { 0x1842, true } };
    struct Sample { BYTE reply; int percent; bool charging, connected; };
    const Sample samples[] = {
        { 0x01, 0, false, true }, { 0x13, 90, false, true }, { 0x15, 100, false, true },
        { 0x81, 0, true, true }, { 0x93, 90, true, true }, { 0x95, 100, true, true },
        // Out-of-range levels must stay unknown and never invent a charging state.
        { 0xFF, -1, false, true }, { 0x7F, -1, false, true },
        { 0x16, -1, false, true }, { 0x96, -1, false, true },
        // Zero is the existing off / out-of-range indication, even with the charging bit set.
        { 0x00, -1, false, false }, { 0x80, -1, false, false },
    };
    for (const auto& model : models) {
        expectedCommand = model.wired ? 0x92 : 0xD2;
        for (const auto& sample : samples) {
            replyValue = sample.reply;
            auto ctx = Poll(model.pid);
            CHECK(ctx.out.size() == 1 && ctx.IsClaimed(0x1038, model.pid));
            const auto& status = ctx.out.front();
            if (status.percent != sample.percent || status.charging != sample.charging || status.connected != sample.connected) {
                std::fprintf(stderr, "PID %04X reply %02X: got percent=%d charging=%d connected=%d; expected %d %d %d\n",
                             model.pid, sample.reply, status.percent, status.charging, status.connected,
                             sample.percent, sample.charging, sample.connected);
                return EXIT_FAILURE;
            }
            CHECK(status.kind == DeviceKind::Mouse);
            CHECK(status.via == (model.wired ? L"USB cable" : L"2.4 GHz receiver"));
        }
    }

    expectedCommand = 0xD2;
    writeOk = false;
    CHECK(Poll(0x1838).out.empty());
    writeOk = true;
    for (int size : { 0, 1 }) {
        replySize = size;
        auto ctx = Poll(0x1838);
        CHECK(ctx.out.empty() && ctx.claimed.empty());
    }
    std::puts("SteelSeries mouse tests passed (eight models, USB/wireless, battery bounds, charging, offline, I/O failures).");
}
