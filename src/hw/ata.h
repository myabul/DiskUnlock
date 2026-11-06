#pragma once
#include "../pch.h"
#include "scsi.h"

namespace hw {
namespace ata {

// ATA/ACS-6 command opcodes used by this tool. Getting a byte wrong here is invisible in
// the UI and fatal in the result, so each value is listed with its spec name.
enum Opcode : uint8_t {
    opSmart = 0xB0,
    opIdentifyDevice = 0xEC,
    opSecuritySetPassword = 0xF1,
    opSecurityUnlock = 0xF2,
    opSecurityErasePrepare = 0xF3,
    opSecurityEraseUnit = 0xF4,
    opSecurityFreezeLock = 0xF5,
    opSecurityDisablePassword = 0xF6,
    opFlushCache = 0xE7,
    opFlushCacheExt = 0xEA,
};

// SMART sub-commands travel in the Features register while the command byte stays 0xB0.
// Names follow the ACS-6 table; smartmontools' ata.h uses the same byte values.
enum SmartFeature : uint8_t {
    featSmartReadData = 0xD0,
    featSmartReadThresholds = 0xD1,
    featSmartAutoSave = 0xD2,      // sector count selects enable(0xF1)/disable(0xF2) autosave
    featSmartSave = 0xD3,
    featSmartExecuteOffline = 0xD4, // LBA low carries the self-test code
    featSmartReadLog = 0xD5,       // LBA low carries the log address
    featSmartWriteLog = 0xD6,
    featSmartWriteThresholds = 0xD7,
    featSmartEnableOperations = 0xD8,
    featSmartDisableOperations = 0xD9,
    featSmartReturnStatus = 0xDA,
};

// Self-test codes for SMART EXECUTE OFF-LINE IMMEDIATE; they land in the LBA low register.
enum SelfTestCode : uint8_t {
    stAutoSaveOffline = 0x00,
    stShort = 0x01,
    stExtended = 0x02,
    stConveyance = 0x03,
    stSelective = 0x04,
    stAbort = 0x7F,
};

// SMART READ LOG addresses (ACS "Log Addresses" table). 00h is the log directory itself.
enum SmartLogAddress : uint8_t {
    logDirAddress = 0x00,
    logErrorSummary = 0x01,
    logErrorComprehensive = 0x02,
    logSelfTest = 0x06,
    logSelfTestExtended = 0x07,
    logSelectiveSelfTest = 0x09,
};

// Every 0xB0 request must present LBA mid=0x4F / LBA high=0xC2; SMART RETURN STATUS echoes
// 4F/C2 for "passed" and F4/2C for "failed" in the same two registers.
constexpr uint8_t kSmartLbaMid = 0x4F;
constexpr uint8_t kSmartLbaHigh = 0xC2;
constexpr uint8_t kSmartStatusFailedMid = 0xF4;
constexpr uint8_t kSmartStatusFailedHigh = 0x2C;
constexpr uint8_t kSmartAutosaveOnCount = 0xF1;   // sector count for featSmartAutoSave
constexpr uint8_t kSmartAutosaveOffCount = 0xF2;


// The 28-bit taskfile is what SAT actually transports; the *Hi fields carry the 48-bit
// (EXT) halves and are only placed in the CDB when lba48/count48 is set.
struct Taskfile {
    uint8_t features = 0, featuresHi = 0;
    uint8_t count = 0, countHi = 0;
    uint8_t lbaLow = 0, lbaLowHi = 0;
    uint8_t lbaMid = 0, lbaMidHi = 0;
    uint8_t lbaHigh = 0, lbaHighHi = 0;
    uint8_t device = 0xA0;  // bit 5 = LBA mode; cdb device byte also carries bit 7=1
    uint8_t command = 0;
    bool lba48 = false;
};

// Executes ATA PASS-THROUGH (16). CK_COND is implied for no-data and register-read commands
// because the only way to see the taskfile back is the SAT ATA Return Descriptor.
CmdResult command(HANDLE hDevice, const Taskfile& tf, Direction dir, void* buffer,
                  uint32_t bufferLen, uint32_t timeoutSec = 20);

// --- IDENTIFY DEVICE ---------------------------------------------------------------
struct Identify {
    uint16_t w[256] = {0};
    bool valid = false;
    // Set only when the mirrored words 85/86/87 agree with 82/83/84, per spec.
    bool capabilityWordsValid = false;

    bool smartSupported() const { return (w[82] & 0x0001) != 0; }
    bool smartEnabled() const { return (w[85] & 0x0001) != 0; }
    bool securitySupported() const { return (w[82] & 0x0002) != 0; }
    bool securityEnabled() const { return (w[85] & 0x0002) != 0; }
    bool selfTestSupported() const { return (w[84] & 0x0002) != 0; }
    bool errorLogSupported() const { return (w[84] & 0x0001) != 0; }
    bool address48Supported() const { return (w[83] & 0x0400) != 0; }
    bool flushCacheExtSupported() const { return (w[83] & 0x2000) != 0; }
    bool writeCacheEnabled() const { return (w[85] & 0x0020) != 0; }

    // Sanitize Device feature set (ACS-5/6) - a far better tool than legacy ERASE UNIT
    // when present, because it reports progress and covers the whole address space.
    bool sanitizeSupported() const { return (w[59] & 0x1000) != 0; }
    bool cryptoScrambleExt() const { return (w[59] & 0x2000) != 0; }
    bool overwriteExt() const { return (w[59] & 0x4000) != 0; }
    bool blockEraseExt() const { return (w[59] & 0x8000) != 0; }
    bool antiFreezeLockExt() const { return (w[59] & 0x0400) != 0; }

    // ATA SECURITY ERASE UNIT wall-clock estimates; words hold multiples of 2 minutes.
    uint32_t eraseTimeMinutesNormal() const { return uint32_t(w[89] & 0x7FFF) * 2u; }
    uint32_t eraseTimeMinutesEnhanced() const { return uint32_t(w[90] & 0x7FFF) * 2u; }
    bool eraseTimeValidNormal() const { return (w[89] & 0x8000) != 0; }
    bool eraseTimeValidEnhanced() const { return (w[90] & 0x8000) != 0; }

    std::wstring serial() const;    // words 10-19
    std::wstring firmware() const;  // words 23-26
    std::wstring model() const;     // words 27-46
    std::wstring modelTrusted() const;  // words 170-175 (ATA/ACS-6 field)

    // Capacity, in logical sectors. 100-103 is authoritative when 48-bit addressing exists;
    // 60-61 is the 28-bit view and saturates at 137 GiB.
    uint64_t userAddressableSectors() const;

    struct SectorSizes {
        uint32_t logical = 0;
        uint32_t physical = 0;
        bool longerThan512 = false;
        bool multiplePerPhysical = false;
        bool valid = false;
    };
    SectorSizes sectorSizes() const;  // word 106 (+117-118 when logical > 512 bytes)
};

struct SecurityState {
    bool supported = false;
    bool enabled = false;
    bool locked = false;
    bool frozen = false;
    bool countExpired = false;         // attempts exhausted; needs a power cycle
    bool enhancedEraseSupported = false;
    bool masterIsMaximum = false;      // false = High, true = Maximum
    bool valid = false;

    std::wstring describe() const;
};

// `failDetail` receives hw::describeResult() when the SAT command itself was refused, so the
// caller can name the IOCTL, the Win32 error and the sense bytes instead of guessing what the
// bridge does not support.
bool readIdentify(HANDLE hDevice, Identify& out, std::wstring* failDetail = nullptr);
SecurityState securityStateOf(const Identify& id);

// --- SMART -------------------------------------------------------------------------
struct SmartAttribute {
    uint8_t id = 0;
    uint16_t flags = 0;
    uint8_t value = 0;
    uint8_t worst = 0;
    uint64_t raw = 0;
    uint8_t threshold = 0;
    bool thresholdValid = false;

    bool preFailure() const { return (flags & 0x01) != 0; }
    bool onlineUpdated() const { return (flags & 0x02) != 0; }
    std::wstring label() const;
};

enum class Health : uint8_t {
    Unknown,     // SMART disabled, unsupported, or the registers did not come back
    Passed,      // SMART RETURN STATUS echoed LBA mid/high = 0x4F/0xC2
    Failed,      // ... or 0xF4/0x2C, the inverted echo the spec reserves for "failing"
    Indeterminate,
};

struct SmartSnapshot {
    bool smartEnabledByDevice = false;
    Health health = Health::Unknown;
    uint8_t offlineDataCollectionStatus = 0;
    uint8_t selfTestStatus = 0xFF;
    std::vector<SmartAttribute> attributes;
    std::wstring note;

    // Remaining SMART READ DATA bytes (362..376). Zero means "the byte said zero", which is a
    // real answer; unknowns never reach this struct because the whole read failed.
    uint16_t revision = 0;
    uint8_t offlineDataCollectionCapability = 0;
    uint8_t errorLogCapability = 0;
    uint16_t totalTimeToCompleteOfflineMin = 0;
    uint16_t shortTestMinutes = 0;
    uint16_t extendedTestMinutes = 0;
    uint16_t conveyanceTestMinutes = 0;

    bool completed() const { return !attributes.empty(); }
    bool selfTestRunning() const { return (selfTestStatus >> 4) == 0x0F; }
    uint32_t selfTestPercentRemaining() const { return uint32_t(selfTestStatus & 0x0F) * 10u; }
    std::wstring offlineStatusText() const;
    std::wstring selfTestStatusText() const;
};

const wchar_t* healthLabel(Health h);
std::wstring selfTestCodeLabel(uint8_t code);

bool readSmart(HANDLE hDevice, const Identify& id, SmartSnapshot& out);

// --- SMART logs --------------------------------------------------------------------
// One entry of the summary SMART error log (log address 01h, 512 bytes, 5 x 90-byte
// structures in a circular buffer). The registers are the 28-bit view the device stored.
struct ErrorLogEntry {
    uint32_t index = 0;         // 1 = newest
    uint8_t errorRegister = 0;
    uint8_t statusRegister = 0;
    uint8_t featuresRegister = 0;
    uint8_t sectorCount = 0;
    uint8_t deviceHead = 0;
    uint8_t commandRegister = 0;
    uint32_t lba = 0;           // assembled from the four legacy registers (28-bit)
    uint16_t powerOnHours = 0;
    uint8_t state = 0;          // 0 unknown / 1 sleeping / 2 standby / 3 active / 4 self-test
    bool valid = false;

    std::wstring stateText() const;
};

// One entry of the SMART self-test log (log address 06h, 21 x 24-byte structures).
struct SelfTestLogEntry {
    uint32_t index = 0;         // 1 = newest
    uint8_t typeCode = 0;       // 01 short / 02 extended / 04 selective / 7F abort ...
    uint8_t statusByte = 0;     // high nibble = outcome, low nibble = 10% units remaining
    uint16_t powerOnHours = 0;
    uint32_t firstErrorLba = 0; // 0xFFFFFFFF = the device did not report one
    bool valid = false;

    uint32_t percentRemaining() const { return uint32_t(statusByte & 0x0F) * 10u; }
    std::wstring statusText() const;
    bool failed() const;
};

// Both standard logs of one drive, kept together because the UI shows them in one grid.
// `errorSupported`/`selfTestSupported` are separate from "read": a refusal is an answer too.
struct SmartLogs {
    bool errorRead = false;
    bool selfTestRead = false;
    uint8_t errorLogVersion = 0;
    uint16_t selfTestRevision = 0;
    uint16_t ataErrorCount = 0;   // total errors the device counted, not just what it keeps
    std::vector<ErrorLogEntry> errors;
    std::vector<SelfTestLogEntry> selfTests;
    std::wstring errorNote;
    std::wstring selfTestNote;

    bool anything() const { return errorRead || selfTestRead; }
    std::wstring summary() const;
};

// Reads log address 01h and 06h. Read-only, so it works on a mounted disk.
bool readSmartLogs(HANDLE hDevice, SmartLogs& out);

// 0xD8 / 0xD9. Needs GENERIC_WRITE, which Windows only grants while no volume is mounted.
CmdResult smartSetOperations(HANDLE hDevice, bool enable);
// 0xD2 with sector count 0xF1/0xF2 - autosave, not the operations switch above.
CmdResult smartSetAutoSave(HANDLE hDevice, bool enable);
// 0xD4 with the code in LBA low: 01 short, 02 extended, 7F abort.
CmdResult smartExecuteSelfTest(HANDLE hDevice, uint8_t code);

// 0xD6 WRITE LOG on address 01h with a zeroed sector: the only standard way to reset the
// error log. Attribute counters (reallocated sectors, power-on hours, wear) are NOT clearable
// by any ATA command, and this function does not pretend otherwise.
CmdResult smartClearErrorLog(HANDLE hDevice);

// --- ATA Security Feature Set ------------------------------------------------------
// A password is a printable ASCII string of 1..32 bytes; the rest of the 32-byte field is
// NUL padded. Longer input is rejected instead of silently truncated.
bool normalizePassword(const std::wstring& in, std::string& out, std::wstring& why);

// Builds the 512-byte Security status data structure: byte 0 holds the user/master and
// ERASE SPECIFIED bits, the password starts at byte 2 (hdparm: data[0]=master, memcpy(data+2)).
CmdResult securitySetPassword(HANDLE hDevice, bool master, const std::string& password,
                              bool eraseLockingMedia, uint16_t masterRevisionCode = 0);
CmdResult securityUnlock(HANDLE hDevice, bool master, const std::string& password);
CmdResult securityDisablePassword(HANDLE hDevice, bool master, const std::string& password);
CmdResult securityFreezeLock(HANDLE hDevice);

// SECURITY ERASE UNIT always requires a preceding ERASE PREPARE and is the slowest command
// in the ATA spec, so the timeout is a caller-supplied, IDENTIFY-derived value.
CmdResult securityEraseUnit(HANDLE hDevice, bool master, bool enhanced,
                            const std::string& password, uint32_t timeoutSec);

}  // namespace ata
}  // namespace hw
