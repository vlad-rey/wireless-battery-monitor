// Checks the short captions used to tell apart devices of the same kind in the taskbar widget.
#include <cstdio>

#include "../src/labels.h"

static int failures = 0;

static void Expect(const char* what, const std::vector<LabelInput>& in, const std::vector<std::wstring>& want, size_t maxChars = 3) {
    auto got = ShortLabels(in, maxChars);
    if (got != want) {
        failures++;
        printf("FAIL %s\n", what);
        for (size_t i = 0; i < got.size(); i++) printf("  [%u] got \"%ls\"%s\n", (unsigned)i, got[i].c_str(), i < want.size() && got[i] != want[i] ? "  <-- different" : "");
    }
}

int main() {
    using K = DeviceKind;
    Expect("single device gets no caption", { { K::Mouse, L"MX Vertical Advanced Ergonomic Mouse" } }, { L"" });
    Expect("two mice: first significant word",
           { { K::Mouse, L"MX Vertical Advanced Ergonomic Mouse" }, { K::Mouse, L"Logitech PRO X Wireless" } }, { L"MX", L"PRO" });
    Expect("different kinds are not mixed up",
           { { K::Mouse, L"MX Vertical" }, { K::Headset, L"Logitech G PRO X 2 LIGHTSPEED" } }, { L"", L"" });
    Expect("only the duplicated kind is captioned",
           { { K::Headset, L"G PRO X 2" }, { K::Mouse, L"MX Vertical" }, { K::Mouse, L"PRO X Wireless" } }, { L"", L"MX", L"PRO" });
    Expect("long words are cut to the limit",
           { { K::Mouse, L"Razer DeathAdder V3 Pro" }, { K::Mouse, L"Razer Viper V3 Pro" } }, { L"Dea", L"Vip" });
    Expect("limit is respected", { { K::Mouse, L"Razer DeathAdder V3 Pro" }, { K::Mouse, L"Razer Viper V3 Pro" } }, { L"De", L"Vi" }, 2);
    Expect("maker is skipped, the model tells them apart",
           { { K::Mouse, L"Keychron M4" }, { K::Mouse, L"Keychron M3" } }, { L"M4", L"M3" });
    Expect("more words when the first one is shared and the limit allows it",
           { { K::Mouse, L"PRO X Wireless" }, { K::Mouse, L"PRO 2 Mini" } }, { L"PRO X", L"PRO 2" }, 8);
    Expect("the limit can cut the extra words, then they are numbered",
           { { K::Mouse, L"PRO X Wireless" }, { K::Mouse, L"PRO 2 Mini" } }, { L"1", L"2" });
    Expect("trailing number when the words are the same",
           { { K::Gamepad, L"Xbox controller 2" }, { K::Gamepad, L"Xbox controller 4" } }, { L"2", L"4" });
    Expect("identical names are numbered",
           { { K::Gamepad, L"DualSense" }, { K::Gamepad, L"DualSense" } }, { L"1", L"2" });
    Expect("three identical names",
           { { K::Gamepad, L"DualSense" }, { K::Gamepad, L"DualSense" }, { K::Gamepad, L"DualSense" } }, { L"1", L"2", L"3" });
    Expect("a name made only of skipped words still gets a caption",
           { { K::Mouse, L"Wireless Mouse" }, { K::Mouse, L"Gaming Mouse" } }, { L"Wir", L"Gam" });
    Expect("maker is skipped case-insensitively", { { K::Mouse, L"LOGITECH Pebble" }, { K::Mouse, L"logitech Lift" } }, { L"Peb", L"Lif" });
    Expect("nothing to caption", {}, {});
    if (!failures) printf("labels_test: all passed\n");
    return failures ? 1 : 0;
}
