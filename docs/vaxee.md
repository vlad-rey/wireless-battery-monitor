# VAXEE 4K/8K receivers and ZYGEN NP-01S V2

## Scope and hardware validation

This driver supports the VAXEE 4K receiver `3057:1002`, the VAXEE 8K receiver
`3057:2002` and the ZYGEN NP-01S V2 Wireless over USB cable (`3057:1011`), on
their vendor collection `FF05:0001` with a 64-byte feature report. Other VAXEE
receivers, other cable product IDs and ZOWIE devices are deliberately not
probed.

The 8K receiver (`VAXEE 8K Wireless`, paired with an NP-01 Ergo Wireless 8K)
was tested on Windows and reported through the same protocol:

```
VAXEE 8K Wireless  [2.4 GHz receiver]  70%
```

The receiver is shared by all VAXEE 8K mice and does not report which one is
paired, so it is shown by its own name. Charging status on the 8K receiver has not yet been observed on hardware.

A real 4K receiver was tested on Windows:

```
VAXEE collection: page=FF05 usage=0001 featureLength=64
VAXEE battery: 70%; charging: no
```

The integrated application also reported:

```
VAXEE wireless mouse  [2.4 GHz receiver]  70%
```

The same mouse was then connected directly over USB and Windows identified
it as `ZYGEN NP-01S V2 Wireless` (`3057:1011`). The updated production driver
reported:

```
VAXEE battery: 75%; charging: yes
```

The taskbar widget was created successfully in the initial receiver build.
Both on-battery receiver operation and wired charging have been observed;
full-charge transitions and sleep/off behavior have not been tested on
hardware. Charging decoding and invalid responses are covered by simulated
replies.

## Protocol

Source: [stuffz/mouse-battery-monitor, docs/VAXEE.md](https://github.com/stuffz/mouse-battery-monitor/blob/main/docs/VAXEE.md).

A request is a zero-padded 64-byte HID feature report:

| Byte | Value |
|---|---|
| 0 | `0E`, report ID |
| 1 | `A5`, header |
| 2 | `0B` for battery level; `10` for charging status |
| 3 | `01`, read flag (never `02`, write) |
| 4 | `01`, data length |
| 5–63 | `00` |

After sending the request with `HidD_SetFeature`, wait 100 ms and read report
`0E` with `HidD_GetFeature`. Require at least six bytes and the expected report
ID, header, command echo and data length.

For battery, response byte 5 must be 0–20 and is multiplied by five, giving
0–100% in 5% steps. For charging, byte 5 is nonzero when charging. A failed
charging query does not discard an otherwise valid battery reading.

The driver retries the battery request once. There are at most three read
requests per matching collection (two battery attempts and one charging
query), with 300 ms total explicit delay. It sends no settings, pairing,
firmware or lighting commands.

If no valid battery reply is received, the driver reports nothing and leaves
the device unclaimed. While the USB collection remains present, the UI retains
a previously successful reading, dimmed, for up to 30 minutes using its existing
grace-period behavior. If the physical collection disappears, the driver emits
one disconnected status to end that grace period immediately. This prevents
a stale receiver icon lingering when the cable is moved to the mouse.

## Tests

`make test` includes `tests/vaxee_driver_test.cpp`, which links the production
driver against a simulated HID transport. It checks the exact outgoing
read-only requests, percentage conversion, charging decoding, invalid and
missing replies, already-claimed devices and exact collection filtering.
