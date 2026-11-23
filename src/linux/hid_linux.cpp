// Linux HID backend for the shared vendor drivers.
//
// Uses the kernel hidraw interface directly, so no third-party HID library is required. The
// report-buffer conventions are deliberately identical to the Windows backend (and to hidapi):
//   * Write/SetFeature: data[0] is the report ID (0 for unnumbered), zero-padded to report length.
//   * Read: a leading zero report ID is stripped.
// That lets every portable vendor driver in src/drivers/ compile and run unchanged.

#ifndef __linux__
// Intentionally empty on other platforms.
#else

#include "../hid.h"

#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

struct ReportInfo {
    int in = 0, out = 0, feat = 0;
    USHORT usagePage = 0, usage = 0;
};
using ReportBits = std::map<int, std::array<int, 3>>;  // report id -> {input, output, feature} bits

void FinalizeCollection(ReportInfo& r, const ReportBits& bits, bool hasIds) {
    for (const auto& [id, arr] : bits) {
        (void)id;
        r.in = std::max(r.in, (arr[0] + 7) / 8 + (hasIds ? 1 : 0));
        r.out = std::max(r.out, (arr[1] + 7) / 8 + (hasIds ? 1 : 0));
        r.feat = std::max(r.feat, (arr[2] + 7) / 8 + (hasIds ? 1 : 0));
    }
}

void TrimUtf16(std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) a++;
    while (b > a && std::iswspace(s[b - 1])) b--;
    s = s.substr(a, b - a);
}

unsigned ItemData(const unsigned char* p, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= ((unsigned)p[i]) << (8 * i);
    return v;
}

// hidraw exposes one node per USB interface, but Windows HIDP_CAPS is per top-level collection.
// Split the report descriptor into application collections so the vendor drivers' page/usage
// filters see the same collections they do on Windows.
std::vector<ReportInfo> ParseReportDescriptor(const unsigned char* d, size_t n) {
    std::vector<ReportInfo> result;
    ReportInfo cur;
    ReportBits bits;
    int reportSize = 0, reportCount = 0, reportId = 0, depth = 0;
    USHORT usagePage = 0, usage = 0;
    bool hasIds = false;
    size_t i = 0;
    auto flush = [&]() {
        FinalizeCollection(cur, bits, hasIds);
        result.push_back(cur);
        cur = ReportInfo();
        bits.clear();
    };
    while (i < n) {
        unsigned char prefix = d[i++];
        int size = prefix & 3;
        if (size == 3) size = 4;
        if (i + (size_t)size > n) break;
        unsigned data = ItemData(d + i, size);
        i += (size_t)size;
        int type = (prefix >> 2) & 3;  // 0 main, 1 global, 2 local
        int tag = (prefix >> 4) & 0x0F;
        if (type == 1) {  // global
            if (tag == 0) usagePage = (USHORT)data;
            else if (tag == 7) reportSize = (int)data;
            else if (tag == 9) reportCount = (int)data;
            else if (tag == 8) { reportId = (int)data; if (reportId) hasIds = true; }
        } else if (type == 2) {  // local
            if (tag == 0 && !usage) usage = (USHORT)data;
        } else if (type == 0) {  // main
            if (tag == 0x0A) {  // Collection
                if (data == 1 && depth == 0 && !bits.empty()) flush();  // Application
                if (data == 1 && depth == 0) { cur.usagePage = usagePage; cur.usage = usage; }
                depth++;
                usage = 0;
            } else if (tag == 0x0C) {  // End Collection
                depth--;
                if (depth <= 0 && !bits.empty()) { flush(); depth = 0; }
                usage = 0;
            } else if (tag == 8 || tag == 9 || tag == 0x0B) {  // Input / Output / Feature
                int idx = tag == 8 ? 0 : (tag == 9 ? 1 : 2);
                bits[reportId][idx] += reportSize * reportCount;
                usage = 0;
            }
        }
    }
    if (!bits.empty()) flush();
    if (result.empty()) result.push_back(cur);
    return result;
}

std::map<std::string, std::string> ReadUevent(const fs::path& dir) {
    std::map<std::string, std::string> m;
    std::ifstream f(dir / "uevent");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

// USB interface number from the resolved sysfs path, e.g. ".../1-1:1.0/0003:..." -> 0.
int InterfaceOf(const fs::path& sysPath) {
    std::error_code ec;
    fs::path real = fs::canonical(sysPath, ec);
    if (ec) return -1;
    std::string parent = real.parent_path().filename().string();
    size_t colon = parent.find(':');
    size_t dot = parent.find('.', colon == std::string::npos ? 0 : colon + 1);
    if (colon == std::string::npos || dot == std::string::npos || dot <= colon + 1) return -1;
    try { return std::stoi(parent.substr(colon + 1, dot - colon - 1)); }
    catch (...) { return -1; }
}

}  // namespace

std::vector<HidColl> EnumHid() {
    std::vector<HidColl> out;
    std::error_code ec;
    fs::directory_iterator it("/sys/bus/hid/devices", ec), end;
    if (ec) return out;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        fs::path dev = it->path();
        auto ev = ReadUevent(dev);
        auto id = ev.find("HID_ID");
        if (id == ev.end()) continue;
        // HID_ID=bus:vid:pid (all hex).
        unsigned bus = 0, vid = 0, pid = 0;
        if (sscanf(id->second.c_str(), "%x:%x:%x", &bus, &vid, &pid) != 3) continue;

        // Find the hidraw node for this collection.
        std::string node;
        std::error_code ec2;
        fs::directory_iterator hit(dev / "hidraw", ec2), hend;
        if (!ec2) for (; hit != hend; hit.increment(ec2)) {
            if (ec2) break;
            std::string name = hit->path().filename().string();
            if (name.rfind("hidraw", 0) == 0) { node = "/dev/" + name; break; }
        }
        if (node.empty()) continue;

        int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = ::open(node.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        HidColl c;
        c.path = Utf8ToWide(node);
        c.vid = (USHORT)vid;
        c.pid = (USHORT)pid;
        c.iface = InterfaceOf(dev);
        auto nm = ev.find("HID_NAME");
        if (nm != ev.end()) {
            c.product = Utf8ToWide(nm->second);
            TrimUtf16(c.product);
        }

        int descSize = 0;
        std::vector<ReportInfo> infos;
        if (ioctl(fd, HIDIOCGRDESCSIZE, &descSize) == 0 && descSize > 0) {
            hidraw_report_descriptor rd{};
            rd.size = (unsigned)descSize;
            if (ioctl(fd, HIDIOCGRDESC, &rd) == 0)
                infos = ParseReportDescriptor(rd.value, rd.size);
        }
        ::close(fd);
        if (infos.empty()) {
            out.push_back(std::move(c));
            continue;
        }
        for (const auto& info : infos) {
            HidColl col = c;
            col.caps.UsagePage = info.usagePage;
            col.caps.Usage = info.usage;
            col.caps.InputReportByteLength = (USHORT)info.in;
            col.caps.OutputReportByteLength = (USHORT)info.out;
            col.caps.FeatureReportByteLength = (USHORT)info.feat;
            out.push_back(std::move(col));
        }
    }
    return out;
}

// ---- HidDev -----------------------------------------------------------------------------------

bool HidDev::Open(const HidColl& c) {
    Close();
    c_ = c;
    std::string path = WideToUtf8(c.path);
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    return ok();
}

bool HidDev::OpenReadOnly(const HidColl& c) {
    Close();
    c_ = c;
    std::string path = WideToUtf8(c.path);
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    return ok();
}

bool HidDev::OpenFeatureOnly(const HidColl& c) {
    // hidraw has no "zero access" open; feature ioctls need read/write on the fd.
    return Open(c);
}

void HidDev::Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool HidDev::Write(const std::vector<BYTE>& data, int timeoutMs) {
    (void)timeoutMs;
    if (!ok() || !c_.outLen() || data.empty()) return false;
    std::vector<BYTE> buf(c_.outLen(), 0);
    memcpy(buf.data(), data.data(), std::min(buf.size(), data.size()));
    return ::write(fd_, buf.data(), buf.size()) == (ssize_t)buf.size();
}

int HidDev::Read(std::vector<BYTE>& buf, int timeoutMs) {
    if (!ok() || !c_.inLen()) return -1;
    struct pollfd pfd{ fd_, POLLIN, 0 };
    int pr = ::poll(&pfd, 1, timeoutMs > 0 ? timeoutMs : 0);
    if (pr <= 0 || !(pfd.revents & POLLIN)) return -1;
    std::vector<BYTE> tmp(c_.inLen(), 0);
    ssize_t got = ::read(fd_, tmp.data(), tmp.size());
    if (got <= 0) return -1;
    size_t off = tmp[0] == 0 ? 1 : 0;
    buf.assign(tmp.begin() + off, tmp.begin() + got);
    return (int)buf.size();
}

int HidDev::SetOutputAndRead(const std::vector<BYTE>& data, std::vector<BYTE>& buf, int timeoutMs) {
    if (!ok() || !c_.inLen() || !c_.outLen() || data.empty()) return -1;
    std::vector<BYTE> out(c_.outLen(), 0);
    memcpy(out.data(), data.data(), std::min(out.size(), data.size()));
    if (ioctl(fd_, HIDIOCSOUTPUT(out.size()), out.data()) < 0) return -1;
    return Read(buf, timeoutMs);
}

void HidDev::Flush() {
    if (!ok()) return;
    struct pollfd pfd{ fd_, POLLIN, 0 };
    std::vector<BYTE> tmp(c_.inLen() ? c_.inLen() : 64);
    while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
        if (::read(fd_, tmp.data(), tmp.size()) <= 0) break;
}

bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    if (!ok() || !c_.featLen() || data.empty()) return false;
    std::vector<BYTE> buf(c_.featLen(), 0);
    memcpy(buf.data(), data.data(), std::min(buf.size(), data.size()));
    return ioctl(fd_, HIDIOCSFEATURE(buf.size()), buf.data()) >= 0;
}

bool HidDev::GetFeature(BYTE reportId, std::vector<BYTE>& out) {
    if (!ok() || !c_.featLen()) return false;
    out.assign(c_.featLen(), 0);
    out[0] = reportId;
    return ioctl(fd_, HIDIOCGFEATURE(out.size()), out.data()) >= 0;
}

bool HidDev::GetFeatureByte(BYTE reportId, BYTE& value) {
    if (!ok() || !reportId || c_.featLen() < 2) return false;
    BYTE report[] = {reportId, 0xff};
    if (ioctl(fd_, HIDIOCGFEATURE(sizeof(report)), report) != sizeof(report) || report[0] != reportId)
        return false;
    value = report[1];
    return true;
}

// ---- helpers (same behaviour as the Windows backend) ------------------------------------------

std::wstring HidProduct(const HidColl& c) { return c.product; }

std::wstring Hex4(USHORT v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v);
    return b;
}

int Clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

int MapTo100(int v, int lo, int hi) {
    if (hi <= lo) return Clamp100(v);
    return Clamp100((v - lo) * 100 / (hi - lo));
}

int Interpolate(int mv, const int* mvTable, const int* pctTable, int n) {
    if (mv >= mvTable[0]) return pctTable[0];
    for (int i = 1; i < n; i++) {
        if (mv >= mvTable[i]) {
            int span = mvTable[i - 1] - mvTable[i];
            return pctTable[i] + (pctTable[i - 1] - pctTable[i]) * (mv - mvTable[i]) / (span ? span : 1);
        }
    }
    return pctTable[n - 1];
}

static bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

DeviceKind GuessKind(const std::wstring& name) {
    std::wstring n;
    for (wchar_t ch : name) n.push_back((wchar_t)towlower((wint_t)ch));
    if (Contains(n, L"mouse") || Contains(n, L"mice") || Contains(n, L"trackball")) return DeviceKind::Mouse;
    if (Contains(n, L"keyboard") || Contains(n, L"keys")) return DeviceKind::Keyboard;
    if (Contains(n, L"controller") || Contains(n, L"gamepad") || Contains(n, L"xbox") ||
        Contains(n, L"dualsense") || Contains(n, L"dualshock") || Contains(n, L"joy-con"))
        return DeviceKind::Gamepad;
    if (Contains(n, L"head") || Contains(n, L"buds") || Contains(n, L"pods") || Contains(n, L"ear") ||
        Contains(n, L"wh-") || Contains(n, L"wf-") || Contains(n, L"audio") || Contains(n, L"speaker") ||
        Contains(n, L"arctis") || Contains(n, L"cloud") || Contains(n, L"void") || Contains(n, L"virtuoso"))
        return DeviceKind::Headset;
    return DeviceKind::Other;
}

DeviceKind KindFromCollections(const std::vector<HidColl>& all, USHORT vid, USHORT pid) {
    bool mouse = false, pad = false, kbd = false;
    for (const auto& c : all) {
        if (c.vid != vid || c.pid != pid || (c.caps.UsagePage && c.caps.UsagePage != 0x01)) continue;
        if (!c.caps.Usage) continue;
        mouse |= c.caps.Usage == 0x02;
        pad |= c.caps.Usage == 0x04 || c.caps.Usage == 0x05;
        kbd |= c.caps.Usage == 0x06;
    }
    if (mouse) return DeviceKind::Mouse;
    if (pad) return DeviceKind::Gamepad;
    if (kbd) return DeviceKind::Keyboard;
    return DeviceKind::Other;
}

#endif  // __linux__
