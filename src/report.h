#pragma once
// Extra diagnostic sections for `--dump`.
#include <windows.h>

#include <string>

namespace report {

// HID devices that could not be opened, every USB device (including ones without a HID interface, e.g.
// audio-only or WinUSB dongles and serial ports) and the Bluetooth devices Windows knows about.
std::wstring DumpExtras();

}  // namespace report
