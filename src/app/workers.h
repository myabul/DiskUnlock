#pragma once
#include "../pch.h"
#include "state.h"

namespace app {
namespace workers {

// Every entry point posts to the serial job queue and returns immediately; the UI never
// waits on a device. `core::Jobs()` guarantees exactly one job touches a disk at a time.

void postRefresh();
void postDeepInspect();
// The short, read-only health pass a selection triggers: SMART or the NVMe health log only,
// without TCG discovery or the log round-trips. It skips a drive that already answered.
void postSmartRead();
void postSurfaceScan();
void postStopActiveJob();

// Metadata-only erase: LBA 0..1MB and the trailing backup GPT.
void postWipePartitionTables();
void postOverwrite(hw::erase::Profile profile);
void postHardwarePurge(bool enhanced, const std::wstring& password, bool useMaster);

void postAtaSetPassword(const std::wstring& password, bool useMaster);
void postAtaUnlock(const std::wstring& password, bool useMaster);
void postAtaDisablePassword(const std::wstring& password, bool useMaster);
void postAtaFreeze();
void postTcgDiscover();
void postPsidRevert(const std::wstring& psid);
void postInspectLba0();
void postReadNvmeHealth();

// Partition/volume map of the selected disk and the two reversible mount flags. None of these
// writes a sector, so they stay outside the destructive gate.
void postRelayout();
void postSetDiskReadOnly(bool readOnly);
void postSetDiskOffline(bool offline);

// SMART management. The log read is pure I/O; the rest sends device commands that need raw
// write access, so they can legitimately be refused while the disk's volumes are mounted.
enum class SmartControl : int {
    enableOperations,
    disableOperations,
    autosaveOn,
    autosaveOff,
    selfTestShort,
    selfTestExtended,
    selfTestAbort,
    clearErrorLog,
};

void postSmartLogs();
void postSmartControl(SmartControl op);
// "Clear" for the counters is a re-anchor of the local baseline, never a device write.
void postSmartBaseline(bool take);

// Report writer; returns false when the path could not be opened.
bool writeReport(const std::wstring& path, std::wstring& why);

// The single gate in front of every destructive job. Checks the selection, the disk role,
// the typed confirmation phrase and that every volume really dismounted.
bool ensureSafeToDestroy(std::wstring& why);

bool busy();
std::wstring currentJobName();

// Confirmation phrase the UI must receive verbatim before a destructive job may run.
std::wstring confirmationPhrase(int diskNumber);

}  // namespace workers
}  // namespace app
