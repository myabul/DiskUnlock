#pragma once
#include "../pch.h"
#include "../core/str.h"

namespace hw {

enum class Direction : uint8_t { NoData, DataIn, DataOut };

struct SenseInfo {
    bool valid = false;
    uint8_t senseKey = 0;
    uint8_t asc = 0;
    uint8_t ascq = 0;
    // SAT copies the ATA taskfile into a 0x09 descriptor when CK_COND is set.
    bool ataReturnPresent = false;
    uint8_t ataError = 0, ataSectorCount = 0, ataLbaLow = 0, ataLbaMid = 0, ataLbaHigh = 0;
    uint8_t ataDevice = 0, ataStatus = 0;
};

struct CmdResult {
    bool ioctlOk = false;      // the IOCTL itself reached the port
    bool deviceOk = false;     // GOOD status, and no ATA ERR bit when registers are available
    uint8_t scsiStatus = 0;
    DWORD winError = 0;
    SenseInfo sense;
    std::wstring detail;

    explicit operator bool() const { return deviceOk; }
};

// Media failure vs. transport failure vs. "volume in use" must stay distinguishable,
// otherwise every read error is reported as a dead sector.
enum class FaultKind : uint8_t {
    None,
    MediaError,        // sense key 3h: unrecovered read/write error -> real defect
    HardwareError,     // sense key 4h
    NotReady,          // sense key 2h: spinning up, door closed, no medium
    UnitAttention,     // sense key 6h: media changed / reset
    InvalidCommand,    // sense key 5h: opcode or fields rejected
    DataProtected,     // sense key 7h: locked or write-protected by design
    Aborted,           // sense key Bh: selected/aborted by host or target
    AccessDenied,      // Windows refused the handle or the sector range
    Timeout,
    NoSense,
    Other,
};

FaultKind classify(const CmdResult& r);
std::wstring faultLabel(FaultKind k);
std::wstring describeResult(const CmdResult& r);

// SCSI CDB pass-through. cdbLen must match the CDB's true length: SAT rejects
// a 10-byte opcode sent as a 6-byte CDB and vice versa.
CmdResult scsiCmd(HANDLE hDevice, const uint8_t* cdb, uint8_t cdbLen, Direction dir,
                  void* buffer, uint32_t bufferLen, uint32_t timeoutSec = 20);

}  // namespace hw
