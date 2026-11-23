# Machenike L7 Arctic / L7 Mix

Verified on an L7 Arctic which identifies as **L7 Mix**:

| Connection | VID:PID | Interface | Usage | Input / output / max feature |
| --- | --- | --- | --- | --- |
| 2.4 GHz receiver | `248A:FF4C` | 1 | `008C:0001` | 33 / 33 / 1001 |
| USB cable | `248A:FF4E` | 2 | `008C:0001` | 33 / 33 / 1001 |

The driver also checks the HID product name `L7 Mix`. Other Machenike models,
different receiver IDs, and Bluetooth vendor queries are not enabled.

## Protocol source

Interoperability analysis of the official **Machenike L7 Mix 1.0.0.8** software,
available from the [manufacturer's driver page](https://haieronline.ru/catalog/computer-technology/accessories/l7-arctic/drivers/),
and read-only queries on the actual mouse. `DeviceDriver.exe` SHA-256:
`A73DF7F5199712E754864397630A2EAC198D0D238450A09ADE8851D2DA31B50D`.

In that 32-bit executable (image base `00400000`), function VA `00436750` builds
the status request; `004494F0` sends it through `HidD_SetFeature` and reads the
feature reply. Status parser `00410C20` extracts the fields below and passes the
battery and charge fields to the UI. These are virtual addresses, not file offsets.
No proprietary executable, application code, or resources are included here.

## Read-state query

Send **21 bytes**, report ID `51`, command `06`, all other bytes zero:

```text
51 06 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Read feature report `51`, also using 21 bytes. The collection's maximum feature
size is 1001 for other reports; it is not the length of the status report.
The driver waits 10 ms before reading and uses one 200 ms deadline per product,
including unrelated replies. It sends no setting, mode, macro, or firmware commands.

Indices include the report ID:

| Bytes | Meaning / observed values |
| --- | --- |
| 0–1 | `51 06`, required command echo |
| 4–7 | `8A 24 4E FF`, the verified L7 device signature |
| 10, bits 0–2 | Work mode: 1 on receiver, 0 on cable |
| 10, bit 3 | Wireless mouse connected; clear on the USB cable too |
| 11 | Battery percentage, 0–100 (zero is valid) |
| 12 | Charge flag: 0 discharging, 1 charging |

Other fields and checksum semantics are not established. The driver does not
invent a checksum. It validates the command, length, device signature, mode and
connected-state battery/charge ranges before publishing a reading.

Captured receiver reply, also 64% in the official app:

```text
51 06 01 00 8A 24 4E FF 05 01 09 40 00 00 00 00 00 00 00 00 00
```

Captured USB cable reply while charging (74%):

```text
51 06 01 00 8A 24 4E FF 05 01 00 4A 01 00 00 00 00 00 00 00 00
```

At that time the receiver still returned its cached 64%, but cleared its link bit:

```text
51 06 01 00 8A 24 4E FF 05 01 01 40 01 00 00 00 00 00 00 00 00
```

This disconnected reading must not expose cached battery/charging values. The
cable takes precedence when both transports are present, and the stable device
key is shared across transports. A known L7 is reserved even on query failure so
the later Ajazz driver cannot probe it with a different protocol on the shared VID.

## Validation boundaries

Fixtures and simulated transport tests cover captured readings, charging,
valid 0%/100%, disconnected receivers, malformed/stale replies, strict matching,
transport failure, duplicate suppression and bounded retry loops. Live 0%/100%
and all sleep/firmware variants are not implied by those tests.
