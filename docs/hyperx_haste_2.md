# HyperX Pulsefire Haste 2 Wireless battery protocol

Read by `HasteWireless` in `src/drivers/hyperx.cpp`. The queries come from
[ngenuity-linux-cli](https://github.com/PMarinov1994/ngenuity-linux-cli) and are the same NGENUITY uses.

Verified on hardware:

- Pulsefire Haste 2 Core Wireless, dongle `03F0:0AB5`.
- Pulsefire Haste 2 Wireless, dongle `03F0:0F98`, by uzumakiprime (PR #312). The level matched HyperX NGENUITY.

The `03F0:0D97` dongle is expected to work the same way but is untested.

## HID collection

| Field | Value (`0F98`) |
|---|---|
| USB interface | `MI_02` (interface 2) |
| Usage page : usage | `FF00:0001` |
| Input / output / feature report length | 64 / 64 / 64 bytes |

The dongle also exposes the standard mouse and consumer-control collections. The driver only opens a vendor
collection (usage page `FF00` or higher) with input reports of at least 8 bytes.

## Queries

Both are output reports: the first two bytes below, zero-padded to the report length. Each reply is an input
report whose first byte is the command plus one. Each query waits at most one second for its reply and skips
unrelated reports (after a reconnect the dongle sends `44 xx`, `45 xx` and `FB xx` reports on its own).

| Request | Reply | Meaning |
|---|---|---|
| `52 01` | `53 01 <linked>` | `linked` = 0 when the mouse is off or out of range |
| `50 02` | `51 02 <percent> <charge> .. .. <mV low> <mV high>` | `percent` 0-100; `charge` 1 = charging, 2 = full on the charger; voltage in mV, little endian (0 on the Core) |

- If the link reply is missing, the battery reply decides.
- If the link reports 0, no battery query is sent and the mouse is shown as switched off.
- For a few seconds after the mouse powers on it reports about 40%. A reading within 6 s of the link coming up is
  ignored, and the tray keeps the last value.

Both requests only read state. Nothing changes settings, lighting or modes.
