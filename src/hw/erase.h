#pragma once
#include "../pch.h"
#include "../core/device.h"
#include "../core/jobs.h"
#include "ata.h"
#include "nvme.h"

namespace hw {
namespace erase {

// --- overwrite engine ---------------------------------------------------------------
enum class Pattern : uint8_t { Zero, Ones, Pseudorandom };

struct Pass {
    Pattern pattern;
    const wchar_t* label;
};

// Each profile is named after what it actually writes. Nothing here claims Gutmann or
// DoD compliance it cannot deliver: a profile's label is the pass list itself.
enum class Profile : uint8_t {
    NistClearOnePass,   // 1 x 0x00
    SinglePassRandom,   // 1 x pseudorandom
    DoDStyleThreePass,  // 0x00, 0xFF, pseudorandom
};

const wchar_t* profileLabel(Profile p);
const std::vector<Pass>& passesFor(Profile p);
uint32_t verificationPassCount(Profile p);  // 1 when the last pass is read back

struct Progress {
    uint64_t totalBytes = 0;
    uint64_t doneBytes = 0;
    uint32_t passIndex = 0;      // 0-based, counting the verification pass as a pass
    uint32_t totalSteps = 0;     // write passes + verification
    double mibPerSec = 0.0;
    std::wstring stepLabel;
};
using ProgressFn = std::function<void(const Progress&)>;

struct Result {
    bool completed = false;
    bool cancelled = false;
    uint64_t bytesWritten = 0;
    uint32_t stepsDone = 0;
    uint64_t verifyMismatchSectors = 0;  // sectors whose read-back did not match the pattern
    std::wstring error;                  // non-empty = aborted; the reason
    std::wstring note;

    bool ok() const { return completed && error.empty(); }
};

// Writes every logical sector of the device, pass after pass. The 10 GiB cap and the
// "one random chunk reused as pass 3" behaviour of the first prototype are gone: the
// whole address space is covered and pseudorandom bytes are generated per chunk.
Result overwrite(core::Device& dev, Profile profile, core::CancelToken& cancel,
                 const ProgressFn& onProgress = {}, bool verifyLastPass = true);

// --- metadata-only erase ------------------------------------------------------------
// Zeroes LBA 0..(frontSectors-1) and the last (backSectors) sectors, which removes the
// protective MBR, the primary GPT header + entries and the *backup* GPT that the
// first version of this tool never touched.
struct TableWipeResult {
    bool completed = false;
    uint64_t frontSectors = 0;
    uint64_t backSectors = 0;
    std::wstring error;
    std::wstring note;
};
TableWipeResult wipePartitionTables(core::Device& dev, std::wstring& why);

// Lets Windows re-enumerate the disk after the table is gone.
bool rereadPartitionTable(core::Device& dev, std::wstring& why);

// --- hardware purge ---------------------------------------------------------------
enum class PurgeMethod : uint8_t {
    None,                  // no in-device purge; overwrite is the only option
    AtaSecureErase,        // SECURITY ERASE PREPARE + ERASE UNIT
    AtaSecureEraseEnh,     // enhanced variant (vendor-specific coverage)
    NvmeSanitize,          // admin SANITIZE
    NvmeFormatCrypto,      // FORMAT NVM SES=2
    NvmeFormatUser,        // FORMAT NVM SES=1
};

const wchar_t* purgeMethodLabel(PurgeMethod m);

// Honest capability report: what the device claims, what the bus allows, what is blocked
// (e.g. a frozen ATA security state) and how long the device itself estimates it will take.
struct PurgePlan {
    core::BusKind bus = core::BusKind::Unknown;
    PurgeMethod best = PurgeMethod::None;
    bool ataSecurityFrozen = false;
    bool ataSecurityLocked = false;
    bool ataSecurityCountExpired = false;
    bool needsPassword = false;         // ATA SECURITY ERASE UNIT requires the password present
    uint32_t estimatedMinutesNormal = 0;
    uint32_t estimatedMinutesEnhanced = 0;
    bool estimateValid = false;
    std::wstring detail;                // why this method, and what was rejected
};

PurgePlan buildPlan(core::Device& dev, const ata::Identify& id);

CmdResult runPurge(core::Device& dev, PurgeMethod method, const std::string& ataPassword,
                   bool useMaster, uint32_t timeoutSec);

}  // namespace erase
}  // namespace hw
