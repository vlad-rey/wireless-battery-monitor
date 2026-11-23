# Areson 25A7 receiver battery support

## Why the original program missed the mouse

The Redragon M915RGB-WL connects through an Areson/CompX `25A7:FA07` "2.4G Wireless Receiver".
Its battery is not exposed as a standard HID *Battery Strength* usage and Windows has no
Bluetooth battery property for it, because the link is a proprietary 2.4 GHz one. The mouse
answers an active `GetPower` request instead, so neither the generic HID fallback nor the
Bluetooth fallback could see it. There was no driver for the vendor, so the receiver stayed
unclaimed and silent.

## Observed hardware and packets

Tested on Windows 11 with a Redragon M915RGB-WL on 2026-11-17:

| Direction | VID:PID | Interface | Usage page:usage | Report size |
|---|---|---|---|---|
| request (feature) | `25A7:FA07` | 1 | `FF02:0002` | 17 bytes |
| reply (input) | `25A7:FA07` | 1 | `FF01:0000` | 17 bytes |

The reply arrives on a different top-level collection from the one the request is written to.
On Windows these are `MI_01&COL07` (feature) and `MI_01&COL05` (input), which is why the driver
opens both handles.

Captured request (`GetPower`, report id `08`, command `04`, checksum makes the frame sum to `0x55`):

```text
08 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 49
```

Captured reply:

```text
09 04 00 00 00 02 08 00 00 00 00 00 00 00 00 00 3E
```

Offsets below count the report id as byte zero:

| Byte(s) | Meaning | Example |
|---|---|---|
| 0 | Reply report id | `09` |
| 1 | Command echo | `04` |
| 2 | Status, 0 = OK | `00` |
| 3–4 | Address, unused for `GetPower` | `00 00` |
| 5 | Data length (2) | `02` |
| 6 | Battery level, 0–10, in 10% steps | `08` = 80% |
| 7 | Charging flag, any non-zero value | `00` = not charging |
| 16 | Checksum; all 17 bytes sum to `0x55` (mod 256) | `3E` |

The vendor's own software showed 80% for the same frame, which is what establishes the ×10
scale. `GetConnectStatus` (`08 03 …`) answers with a 1-byte payload and reported the mouse as
linked at the same time.

## Other Areson mice

The older `FA7B` / `FA7C` receivers used by the Redragon M693 report the charge directly as a
whole percentage at the same reply offset. Applying this driver's ×10 scale there would show
800%, so those product ids are deliberately not claimed. Only `FA07` and `FA08` are handled;
`FA08` is the alternate identity of the same family and has not been verified on this machine.

## Polling and identity

- The driver only sends the read-only `GetPower` request. It never writes configuration,
  lighting or DPI data.
- Each refresh opens the feature collection with zero access and the input collection for
  reading, sends the request and waits up to 1.8 s for a reply, with two attempts and a queue
  flush between them so a stale report cannot be mistaken for the answer.
- A failed read claims nothing, so a sleeping mouse keeps the last value in the UI. The entry
  uses a 30-minute grace period, matching the other passive drivers.
- The device key is derived from vendor and product id (`areson:25A7:FA07`), so it survives the
  receive/sleep cycle and a receiver replug.

## Validation

The full application was built with MSVC C++17 and the Windows SDK and run against the actual
mouse. `WirelessBattery.exe --list` returned:

```text
Redragon wireless mouse  [2.4 GHz receiver]  80%   (areson:25A7:FA07)
HyperX Cloud II Wireless [2.4 GHz receiver]  14%, 3661 mV
```

The reading matched the percentage displayed by the official Redragon software at the same
moment. The MinGW `build.bat` includes the new source file; the `Makefile` discovers it through
its wildcard.

## Protocol sources

- [Qehbr/m913-ctl](https://github.com/Qehbr/m913-ctl): 17-byte frames, feature/input split and the
  `0x55` checksum rule, recovered from USB captures of the Redragon software.
- [redragon-m913-battery-tray](https://github.com/Doomguy-BFG9000/redragon-m913-battery-tray):
  `GetPower` command and the 10% level scale on `25A7:FA07`.
- [redragon-mouse-linux PROTOCOL.md](https://github.com/rajchauhan28/redragon-mouse-linux/blob/main/PROTOCOL.md):
  reply layout and the different percentage scale on the `FA7B` / `FA7C` family.
