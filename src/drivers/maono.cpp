// MAONO PD200W battery reporting over its 2.4 GHz receiver.
//
// Hardware-tested by the device owner:
// VID:PID 352F:040E, interface 3, usage page/usage 000C:0001,
// input/output reports 64 bytes, no feature reports.
// Battery register 0x0042 tracks the vendor software's battery level
// as the battery discharges. No device is reported when the mic is off.
//
// PD200W transport reference (documented for a different PID, 352F:0411):
// https://github.com/iamsh4/maomao/blob/main/protocol-pd200w.md
//
// Battery register reference from the related PD100W:
// https://github.com/shahriyardx/maono/blob/main/src/mic.rs
//
// Only sends GET (0x04), never SET (0x03).
// Charging status is not implemented.

#include "drivers.h"

namespace
{

    constexpr USHORT kVid = 0x352F;
    constexpr USHORT kPid = 0x040E;
    constexpr BYTE kReportId = 0xC4;
    constexpr BYTE kGet = 0x04;

    // Candidate inherited from PD100W. Unconfirmed on PD200W.
    constexpr USHORT kBatteryRegister = 0x0042;

    constexpr int kInterface = 3;
    constexpr int kReportLength = 64;
    constexpr int kTimeoutMs = 1000;

    USHORT ReadLe16(const std::vector<BYTE> &data, size_t offset)
    {
        return static_cast<USHORT>(
            static_cast<unsigned>(data[offset]) |
            (static_cast<unsigned>(data[offset + 1]) << 8));
    }

    USHORT Checksum(const std::vector<BYTE> &data, size_t count)
    {
        unsigned sum = 0;
        for (size_t i = 0; i < count; ++i)
            sum += data[i];
        return static_cast<USHORT>((0x10000u - (sum & 0xFFFFu)) & 0xFFFFu);
    }

    std::vector<BYTE> BatteryRequest()
    {
        // Candidate GET frame:
        // C4 09 00 00 04 42 00 ED FE
        //
        // C4 is the HID report ID, so no extra leading zero is added.
        // Remaining bytes are padding.
        std::vector<BYTE> request(kReportLength, 0);
        request[0] = kReportId;
        request[1] = 9;
        request[4] = kGet;
        request[5] = static_cast<BYTE>(kBatteryRegister & 0xFF);
        request[6] = static_cast<BYTE>(kBatteryRegister >> 8);

        const USHORT checksum = Checksum(request, 7);
        request[7] = static_cast<BYTE>(checksum & 0xFF);
        request[8] = static_cast<BYTE>(checksum >> 8);
        return request;
    }

    bool ParseBattery(const std::vector<BYTE> &reply, int &percent)
    {
        // Expected response body: one or more little-endian
        // (register, value) pairs. This interpretation is based on the
        // related PD100W implementation and needs PD200W testing.
        if (reply.size() < 11 || reply[0] != kReportId)
            return false;

        const size_t length = reply[1];
        if (length < 11 || length > reply.size())
            return false;
        if (reply[2] != 0 || reply[3] != 0 || reply[4] != kGet)
            return false;

        const size_t bodyEnd = length - 2;
        if ((bodyEnd - 5) % 4 != 0)
            return false;
        if (ReadLe16(reply, bodyEnd) != Checksum(reply, bodyEnd))
            return false;

        for (size_t offset = 5; offset < bodyEnd; offset += 4)
        {
            if (ReadLe16(reply, offset) != kBatteryRegister)
                continue;

            const unsigned value = ReadLe16(reply, offset + 2);
            if (value > 100)
                return false;

            percent = static_cast<int>(value);
            return true;
        }

        return false;
    }

    bool ReadBattery(HidDev &device, int &percent)
    {
        // One absolute deadline shared by sending and receiving.
        const auto deadline = GetTickCount64() + kTimeoutMs;
        const auto request = BatteryRequest();

        device.Flush();

        const auto beforeWrite = GetTickCount64();
        if (beforeWrite >= deadline)
            return false;

        const int remainingWrite = static_cast<int>(deadline - beforeWrite);
        const int writeTimeout = remainingWrite < 250 ? remainingWrite : 250;

        // USB output transfer carrying a register GET.
        // This does not send a settings-changing SET command.
        if (!device.Write(request, writeTimeout))
            return false;

        // Ignore unrelated reports without restarting the deadline.
        for (int reports = 0; reports < 128; ++reports)
        {
            const auto now = GetTickCount64();
            if (now >= deadline)
                break;

            std::vector<BYTE> reply;
            const int remaining = static_cast<int>(deadline - now);
            if (device.Read(reply, remaining) < 0)
                break;

            if (GetTickCount64() >= deadline)
                break;
            if (ParseBattery(reply, percent))
                return true;
        }

        return false;
    }

} // namespace

void PollMaono(PollContext &ctx)
{
    if (ctx.IsClaimed(kVid, kPid))
        return;

    const auto collections = ctx.Find(kVid, kPid, [](const HidColl &c)
                                      { return c.iface == kInterface &&
                                               c.page() == 0x000C &&
                                               c.usage() == 0x0001 &&
                                               c.inLen() == kReportLength &&
                                               c.outLen() == kReportLength &&
                                               c.featLen() == 0; });

    for (const HidColl *collection : collections)
    {
        HidDev device;
        if (!device.Open(*collection))
            continue;

        int percent = -1;
        if (!ReadBattery(device, percent))
            continue;

        DeviceStatus status;
        status.key = L"maono:pd200w:" + collection->path;
        status.name = L"MAONO PD200W";
        status.kind = DeviceKind::Other;
        status.via = L"2.4 GHz receiver";
        status.percent = percent;
        // status.detail = L"experimental battery register 0x0042";

        // Charging and link-state registers are not established.
        // Do not infer either state from the battery percentage.
        ctx.Add(status, kVid, kPid);
        return;
    }
}
