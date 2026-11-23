// Includes the backend directly so the anonymous-namespace parser can be exercised without
// real hardware. Run with: ./build-linux.sh test
#include "../src/linux/hid_linux.cpp"

#include <cassert>
#include <cstdio>

int main() {
    // Two application collections: a mouse with a 2-byte input report and a vendor collection
    // with a 7-byte feature report.
    const unsigned char descriptor[] = {
        0x05, 0x01,        // Usage Page (Generic Desktop)
        0x09, 0x02,        // Usage (Mouse)
        0xA1, 0x01,        // Collection (Application)
        0x75, 0x08,        //   Report Size (8)
        0x95, 0x02,        //   Report Count (2)
        0x81, 0x02,        //   Input (Data,Var,Abs)
        0xC0,              // End Collection
        0x06, 0x00, 0xFF,  // Usage Page (FF00)
        0x09, 0x01,        // Usage (1)
        0xA1, 0x01,        // Collection (Application)
        0x75, 0x08,        //   Report Size (8)
        0x95, 0x07,        //   Report Count (7)
        0xB1, 0x02,        //   Feature (Data,Var,Abs)
        0xC0,              // End Collection
    };
    auto infos = ParseReportDescriptor(descriptor, sizeof(descriptor));
    assert(infos.size() == 2);
    assert(infos[0].usagePage == 0x01 && infos[0].usage == 0x02);
    assert(infos[0].in == 2 && infos[0].feat == 0);
    assert(infos[1].usagePage == 0xFF00 && infos[1].usage == 0x01);
    assert(infos[1].feat == 7 && infos[1].in == 0);

    // Report IDs add one prefix byte to every report length.
    const unsigned char numbered[] = {
        0x06, 0x00, 0xFF, 0xA1, 0x01,
        0x85, 0x10,        // Report ID (16)
        0x75, 0x08, 0x95, 0x20, 0xB1, 0x02,  // 32-byte feature report
        0xC0,
    };
    auto numberedInfos = ParseReportDescriptor(numbered, sizeof(numbered));
    assert(numberedInfos.size() == 1);
    assert(numberedInfos[0].feat == 33);

    printf("linux HID report descriptor tests passed\n");
    return 0;
}
