#include "brands.h"

#include <cwctype>

namespace {

// `dark`: the official colour; brightened (same hue) only for Xbox, PlayStation and Samsung, whose
// brand blues and greens are too dark on a dark taskbar. `light`: a deeper shade with at least 3:1
// contrast on the light taskbar (#F3F3F3); yellow and silver need the most darkening.
const BrandStyle kBrands[] = {
    { Accent::None,         L"",             { 255, 255, 255 }, {   0,   0,   0 } },
    { Accent::Razer,        L"Razer",        {  68, 214,  44 }, {  22, 140,  20 } },  // #44D62C
    { Accent::Logitech,     L"Logitech",     {   0, 184, 252 }, {   0, 120, 180 } },  // Logitech G #00B8FC
    { Accent::SteelSeries,  L"SteelSeries",  { 255,  82,   0 }, { 204,  65,   0 } },  // #FF5200
    { Accent::Corsair,      L"Corsair",      { 245, 228,  26 }, { 150, 122,   0 } },  // yellow #F5E41A
    { Accent::HyperX,       L"HyperX",       { 232,  35,  63 }, { 200,  16,  46 } },  // #E31837
    { Accent::Rog,          L"ROG",          { 255,  46,  77 }, { 208,   0,  42 } },  // ROG red #FF0029
    { Accent::Zowie,        L"ZOWIE",        { 240,  60,  50 }, { 192,  32,  26 } },  // #E2231A
    { Accent::PlayStation,  L"PlayStation",  {  31, 139, 255 }, {   0,  98, 196 } },  // #0070D1
    { Accent::Xbox,         L"Xbox",         {  34, 168,  34 }, {  16, 124,  16 } },  // #107C10
    { Accent::Galaxy,       L"Samsung",      {  91, 123, 255 }, {  20,  40, 160 } },  // #1428A0
    { Accent::Apple,        L"Apple",        { 199, 199, 204 }, { 110, 110, 115 } },  // silver / space grey
    { Accent::Jabra,        L"Jabra",        { 255, 200,   0 }, { 140, 106,   0 } },  // #FFC800
    { Accent::Jbl,          L"JBL",          { 255,  61,   0 }, { 204,  52,   0 } },  // #FF3D00
    { Accent::CoolerMaster, L"Cooler Master",{ 157, 107, 255 }, { 106,  43, 217 } },  // purple
    { Accent::Steam,        L"Steam",        {  26, 159, 255 }, {  11, 111, 191 } },  // #1A9FFF
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

bool StartsWith(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }

// `w` as a whole word: "rog" matches "ROG Harpe" and "ASUS ROG Delta", not "Progress".
bool HasWord(const std::wstring& n, const std::wstring& w) {
    for (size_t at = n.find(w); at != std::wstring::npos; at = n.find(w, at + 1)) {
        bool startOk = at == 0 || !iswalnum(n[at - 1]);
        size_t end = at + w.size();
        bool endOk = end == n.size() || !iswalnum(n[end]);
        if (startOk && endOk) return true;
    }
    return false;
}

struct KeyRule { const wchar_t* prefix; Accent a; };
const KeyRule kKeys[] = {
    { L"razer:", Accent::Razer }, { L"blackshark:", Accent::Razer }, { L"barracuda:", Accent::Razer },
    { L"logi:", Accent::Logitech }, { L"g435:", Accent::Logitech },
    { L"steelseries:", Accent::SteelSeries },
    { L"corsair:", Accent::Corsair },
    { L"hyperx:", Accent::HyperX },
    { L"rog:", Accent::Rog },
    { L"zowie:", Accent::Zowie },
    { L"ps:", Accent::PlayStation }, { L"inzone:", Accent::PlayStation },
    { L"xbox:", Accent::Xbox }, { L"xinput:", Accent::Xbox },
    { L"galaxybuds:", Accent::Galaxy },
    { L"airpods:", Accent::Apple },
    { L"jabra:", Accent::Jabra },
    { L"jbl:", Accent::Jbl },
    { L"coolermaster:", Accent::CoolerMaster },
    { L"steam:", Accent::Steam },
};

// Vendor ids that only one of these brands uses. Shared ones are left to the name check:
// 03F0 (HP, also HyperX), 0B05 (ASUS, also ROG), 045E (Microsoft: Xbox pads but also Surface mice).
struct VidRule { unsigned short vid; Accent a; };
const VidRule kVids[] = {
    { 0x1532, Accent::Razer }, { 0x068E, Accent::Razer },  // 068E: Razer keyboards over Bluetooth LE
    { 0x046D, Accent::Logitech },
    { 0x1038, Accent::SteelSeries },
    { 0x1B1C, Accent::Corsair },
    { 0x0951, Accent::HyperX },     // Kingston-era HyperX
    { 0x04A5, Accent::Zowie },      // BenQ
    { 0x054C, Accent::PlayStation },
    { 0x04E8, Accent::Galaxy },
    { 0x05AC, Accent::Apple }, { 0x004C, Accent::Apple },  // USB / Bluetooth SIG company id
    { 0x0B0E, Accent::Jabra },
    { 0x0ECB, Accent::Jbl },
    { 0x2516, Accent::CoolerMaster },
    { 0x28DE, Accent::Steam },
};

enum class How { Contains, Word, Prefix, Exact };
struct NameRule { const wchar_t* text; How how; Accent a; };
const NameRule kNames[] = {
    { L"razer", How::Contains, Accent::Razer },
    { L"logitech", How::Contains, Accent::Logitech }, { L"logi ", How::Prefix, Accent::Logitech },
    // Bluetooth names often leave the brand out: "MX Master 3S", "MX Keys S", "LIFT", "POP Mouse", "Pebble M350".
    { L"mx ", How::Prefix, Accent::Logitech }, { L"lift", How::Prefix, Accent::Logitech },
    { L"pop mouse", How::Prefix, Accent::Logitech }, { L"pop keys", How::Prefix, Accent::Logitech },
    { L"pebble", How::Prefix, Accent::Logitech }, { L"ergo k", How::Prefix, Accent::Logitech },
    { L"ergo m", How::Prefix, Accent::Logitech },
    { L"steelseries", How::Contains, Accent::SteelSeries }, { L"arctis", How::Word, Accent::SteelSeries },
    { L"aerox", How::Word, Accent::SteelSeries }, { L"rival ", How::Prefix, Accent::SteelSeries },
    { L"corsair", How::Contains, Accent::Corsair },
    { L"hyperx", How::Contains, Accent::HyperX }, { L"pulsefire", How::Word, Accent::HyperX },
    { L"rog", How::Word, Accent::Rog },
    { L"zowie", How::Contains, Accent::Zowie },
    { L"dualsense", How::Contains, Accent::PlayStation }, { L"dualshock", How::Contains, Accent::PlayStation },
    { L"playstation", How::Contains, Accent::PlayStation }, { L"inzone", How::Word, Accent::PlayStation },
    { L"wireless controller", How::Exact, Accent::PlayStation },  // DualSense / DualShock 4 Bluetooth name
    { L"xbox", How::Word, Accent::Xbox },
    { L"galaxy ", How::Prefix, Accent::Galaxy }, { L"samsung", How::Word, Accent::Galaxy },
    { L"airpods", How::Contains, Accent::Apple }, { L"magic mouse", How::Prefix, Accent::Apple },
    { L"magic keyboard", How::Prefix, Accent::Apple }, { L"magic trackpad", How::Prefix, Accent::Apple },
    { L"apple", How::Word, Accent::Apple }, { L"iphone", How::Prefix, Accent::Apple },
    { L"jabra", How::Word, Accent::Jabra },
    { L"jbl", How::Word, Accent::Jbl },
    { L"cooler master", How::Contains, Accent::CoolerMaster }, { L"coolermaster", How::Contains, Accent::CoolerMaster },
    { L"steam controller", How::Contains, Accent::Steam },
};

bool Matches(const std::wstring& n, const NameRule& r) {
    switch (r.how) {
        case How::Contains: return n.find(r.text) != std::wstring::npos;
        case How::Word:     return HasWord(n, r.text);
        case How::Prefix:   return StartsWith(n, r.text);
        case How::Exact:    return n == r.text;
    }
    return false;
}

// Logitech names that are just a model number: "K380", "M720 Triathlon", "G915", "G502 X".
bool LogitechModel(const std::wstring& n) {
    return n.size() >= 4 && (n[0] == L'g' || n[0] == L'k' || n[0] == L'm') && iswdigit(n[1]) && iswdigit(n[2]) &&
           iswdigit(n[3]) && (n.size() == 4 || !iswalnum(n[4]));
}

// Sony headphones (WH-1000XM5, WF-1000XM4) share PlayStation's vendor id but are not PlayStation gear.
bool SonyAudio(const std::wstring& n) { return StartsWith(n, L"wh-") || StartsWith(n, L"wf-") || HasWord(n, L"linkbuds"); }

}  // namespace

const BrandStyle* AllBrands(int& count) {
    count = (int)(sizeof(kBrands) / sizeof(kBrands[0]));
    return kBrands;
}

const BrandStyle& StyleOf(Accent a) {
    for (const auto& b : kBrands)
        if (b.accent == a) return b;
    return kBrands[0];
}

BrandColor AccentColor(Accent a, bool lightTaskbar) {
    const BrandStyle& b = StyleOf(a);
    return lightTaskbar ? b.light : b.dark;
}

Accent BrandOf(const DeviceStatus& s) {
    for (const auto& k : kKeys)
        if (StartsWith(s.key, k.prefix)) return k.a;
    std::wstring n = Lower(s.name);
    if (s.vid)
        for (const auto& v : kVids)
            if (v.vid == s.vid && !(v.a == Accent::PlayStation && SonyAudio(n))) return v.a;
    if (s.vid == 0x045E && s.kind == DeviceKind::Gamepad) return Accent::Xbox;
    if (SonyAudio(n)) return Accent::None;
    for (const auto& r : kNames)
        if (Matches(n, r)) return r.a;
    return LogitechModel(n) ? Accent::Logitech : Accent::None;
}
