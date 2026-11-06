#pragma once
#include "../pch.h"
#include "scsi.h"

namespace hw {
namespace nvme {

// NVMe admin opcodes are a different namespace from ATA/SCSI; only the ones used here.
enum AdminOpcode : uint8_t {
    opGetLogPage = 0x02,
    opIdentify = 0x06,
    opFormatNvm = 0x80,
    opSanitize = 0x84,
    opDeviceSelfTest = 0x14,
};

// CDW10.SES of FORMAT NVM - the only secure-erase selector ATA-style tools usually get wrong.
enum class SecureErase : uint8_t {
    None = 0,            // block erase with 0x00 pattern
    UserUserData = 1,    // erase all logical blocks of the namespace (user data)
    Cryptographic = 2,   // destroy the encryption key: NIST "Purge" for SED/OPAL SSDs
};

// CDW10.SANACT of SANITIZE; SANITIZE is namespace-independent and covers the whole device.
enum class SanitizeAction : uint8_t {
    Exit = 0,
    BlockErase = 1,
    Overwrite = 2,
    CryptoErase = 4,
};

struct U128 {
    uint64_t value = 0;
    bool overflow = false;  // true when the 16-byte counter does not fit in 64 bits
    static U128 decode(const uint8_t* b);
};

struct HealthInfo {
    bool valid = false;
    uint8_t criticalWarning = 0;
    int32_t compositeTempC = -1;      // converted from Kelvin; -1 when not reported
    uint8_t availableSpare = 0;       // percent
    uint8_t availableSpareThreshold = 0;
    uint8_t percentageUsed = 0;       // can legitimately exceed 100
    U128 dataUnitsRead;               // in units of 1000 logical blocks
    U128 dataUnitsWritten;
    U128 hostReadCommands;
    U128 hostWriteCommands;
    U128 controllerBusyMinutes;
    U128 powerCycles;
    U128 powerOnHours;
    U128 unsafeShutdowns;
    U128 mediaErrors;
    U128 errorLogEntries;
    std::wstring note;

    std::wstring criticalWarningText() const;
};

struct SanitizeStatus {
    bool valid = false;
    uint8_t progressPercent = 0;   // 0-100, or 0xFF when unsupported/not started
    bool inProgress = false;
    uint32_t globalDataEraseStatus = 0;
    std::wstring note;
    std::wstring describe() const;
};

bool readHealth(HANDLE hDevice, HealthInfo& out);
bool readSanitizeStatus(HANDLE hDevice, SanitizeStatus& out);

// --- NVMe health logs ---------------------------------------------------------------
// Device Self-test log (log page 06h): one "what is running now" byte plus 20 finished
// results, newest first. Figure 212/213 of the NVMe Base Spec 2.0a.
struct SelfTestResult {
    uint8_t opCode = 0;        // status byte high nibble: 1 short, 2 extended, E vendor
    uint8_t resultCode = 0;    // low nibble: 0 completed without error, F unused
    uint8_t segment = 0;       // only meaningful when resultCode == 7 (failed segments)
    uint8_t validBits = 0;     // bit0 = NSID valid, bit1 = failing LBA valid
    uint64_t powerOnHours = 0;
    uint64_t firstErrorLba = 0;
    uint32_t nsid = 0;
    uint8_t statusCodeType = 0, statusCode = 0;
    bool valid = false;

    std::wstring opText() const;
    std::wstring resultText() const;
};

struct SelfTestLog {
    bool valid = false;
    uint8_t currentOperation = 0;   // 0 none, 1 short running, 2 extended running, E vendor
    uint8_t currentCompletion = 0;  // bits 6:0 = percent complete of the running test
    std::vector<SelfTestResult> results;   // unused (all-zero / result F) entries dropped
    std::wstring note;

    bool running() const { return (currentOperation & 0x0F) != 0; }
    std::wstring statusText() const;
};

// Error Information log (log page 01h): 384-byte entries, newest first, no sequence number
// beyond the controller's own error count.
struct ErrorInfoEntry {
    uint64_t errorCount = 0;   // monotonically increasing per controller
    uint16_t sqId = 0, cmdId = 0;
    uint16_t statusField = 0;
    uint8_t opcode = 0;
    uint32_t nsid = 0;
    uint64_t lba = 0;
    bool valid = false;

    std::wstring describe() const;
};

bool readSelfTestLog(HANDLE hDevice, SelfTestLog& out);
bool readErrorInfoLog(HANDLE hDevice, std::vector<ErrorInfoEntry>& out, std::wstring& why);

// Device Self-test admin command (opcode 14h). Only the two codes the spec defines as
// non-vendor are accepted: 1 = short, 2 = extended. NVMe has no in-band "abort self-test"
// code, so unlike ATA there is nothing to stop here.
CmdResult deviceSelfTest(HANDLE hDevice, uint8_t selfTestCode, uint32_t nsid,
                         std::wstring& why, uint32_t timeoutSec = 30);

// Reads a log page through the STOR NVMe adapter path. logPageIds are the NVMe spec values
// (0x01 error, 0x02 health, 0x06 self-test, 0x81 sanitize).
CmdResult getLogPage(HANDLE hDevice, uint8_t logPageId, void* buffer, uint32_t bufferLen,
                     uint32_t timeoutSec = 10);

// Raw 64-byte admin submission entry, used by transports that need their own opcodes
// (TCG SECURITY SEND/RECEIVE). `sqe` must be kNvmeCmdSize bytes.
CmdResult rawAdminCommand(HANDLE hDevice, const uint8_t sqe[64], Direction dir, void* data,
                          uint32_t dataLen, uint32_t timeoutSec);

// FORMAT NVM applies to one namespace or, with nsid 0xFFFFFFFF, to all of them.
CmdResult formatNvm(HANDLE hDevice, uint32_t nsid, SecureErase ses, uint32_t timeoutSec);

// SANITIZE operates on the whole controller; no namespace selection exists.
CmdResult sanitize(HANDLE hDevice, SanitizeAction action, uint32_t timeoutSec);

}  // namespace nvme
}  // namespace hw
