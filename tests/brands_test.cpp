// Brand recognition: driver key, vendor id, name, and the look-alikes that must stay plain.
#include <cstdio>

#include "../src/brands.h"

static int failures = 0;

static void Expect(const wchar_t* key, const wchar_t* name, unsigned short vid, DeviceKind kind, Accent want) {
    DeviceStatus s;
    s.key = key;
    s.name = name;
    s.vid = vid;
    s.kind = kind;
    Accent got = BrandOf(s);
    if (got != want) {
        failures++;
        fwprintf(stderr, L"FAIL: %ls / %ls / %04X: got '%ls', want '%ls'\n", key, name, vid, StyleOf(got).name,
                 StyleOf(want).name);
    }
}

int main() {
    using K = DeviceKind;
    // Driver keys.
    Expect(L"razer:00b7", L"Razer DeathAdder V3 Pro", 0x1532, K::Mouse, Accent::Razer);
    Expect(L"blackshark:0001", L"BlackShark V2 Pro", 0, K::Headset, Accent::Razer);
    Expect(L"logi:4099", L"G502 X PLUS", 0x046D, K::Mouse, Accent::Logitech);
    Expect(L"steelseries:12e0", L"Arctis Nova 7", 0, K::Headset, Accent::SteelSeries);
    Expect(L"corsair:0a6b", L"HS80 RGB Wireless", 0, K::Headset, Accent::Corsair);
    Expect(L"hyperx:1718", L"Cloud II Wireless", 0, K::Headset, Accent::HyperX);
    Expect(L"rog:1a85", L"Azoth", 0, K::Keyboard, Accent::Rog);
    Expect(L"zowie:0001", L"EC2-CW", 0, K::Mouse, Accent::Zowie);
    Expect(L"ps:0ce6", L"DualSense", 0, K::Gamepad, Accent::PlayStation);
    Expect(L"inzone:0001", L"INZONE H9", 0, K::Headset, Accent::PlayStation);
    Expect(L"xinput:0:1", L"Xbox controller 1", 0, K::Gamepad, Accent::Xbox);
    Expect(L"galaxybuds:x", L"Buds3 Pro", 0, K::Headset, Accent::Galaxy);
    Expect(L"airpods:x", L"Pods", 0, K::Headset, Accent::Apple);
    Expect(L"jabra:x", L"Evolve2 65", 0, K::Headset, Accent::Jabra);
    Expect(L"jbl:x", L"Quantum 910", 0, K::Headset, Accent::Jbl);
    Expect(L"coolermaster:x", L"MM712", 0, K::Mouse, Accent::CoolerMaster);
    Expect(L"steam:x", L"Controller", 0, K::Gamepad, Accent::Steam);
    // Vendor ids (HeadsetControl and plain Bluetooth / HID devices).
    Expect(L"hsc:1038:12e0", L"Arctis Nova 7", 0x1038, K::Headset, Accent::SteelSeries);
    Expect(L"hsc:1b1c:0a6b", L"Void Wireless", 0x1B1C, K::Headset, Accent::Corsair);
    Expect(L"bt:{x}", L"DSV2 Pro", 0x068E, K::Keyboard, Accent::Razer);
    Expect(L"bt:{x}", L"Wireless Controller", 0x054C, K::Gamepad, Accent::PlayStation);
    Expect(L"bt:{x}", L"Magic Mouse", 0x004C, K::Mouse, Accent::Apple);
    Expect(L"bt:{x}", L"Xbox Wireless Controller", 0x045E, K::Gamepad, Accent::Xbox);
    // Names only.
    Expect(L"bt:{x}", L"MX Master 3S", 0, K::Mouse, Accent::Logitech);
    Expect(L"bt:{x}", L"K380", 0, K::Keyboard, Accent::Logitech);
    Expect(L"bt:{x}", L"HyperX Cloud Mix", 0, K::Headset, Accent::HyperX);
    Expect(L"bt:{x}", L"ASUS ROG Keris II Ace", 0, K::Mouse, Accent::Rog);
    Expect(L"bt:{x}", L"Galaxy Buds2 Pro", 0, K::Headset, Accent::Galaxy);
    Expect(L"bt:{x}", L"JBL Tune 770NC", 0, K::Headset, Accent::Jbl);
    Expect(L"bt:{x}", L"Jabra Elite 8 Active", 0, K::Headset, Accent::Jabra);
    Expect(L"bt:{x}", L"Aerox 5 Wireless", 0, K::Mouse, Accent::SteelSeries);
    // Look-alikes that must stay plain.
    Expect(L"bt:{x}", L"HP 430 Multi-Device Mouse", 0x03F0, K::Mouse, Accent::None);
    Expect(L"bt:{x}", L"Surface Precision Mouse", 0x045E, K::Mouse, Accent::None);
    Expect(L"bt:{x}", L"WH-1000XM5", 0x054C, K::Headset, Accent::None);
    Expect(L"bt:{x}", L"WH-1000XM3", 0, K::Headset, Accent::None);
    Expect(L"bt:{x}", L"Progress Mouse", 0, K::Mouse, Accent::None);
    Expect(L"bt:{x}", L"Bose QC Ultra Headphones", 0, K::Headset, Accent::None);
    Expect(L"bt:{x}", L"Pixel Buds Pro", 0, K::Headset, Accent::None);

    if (failures) {
        fwprintf(stderr, L"%d brand test(s) failed\n", failures);
        return 1;
    }
    puts("brands: all passed");
    return 0;
}
