#pragma once
#include "../pch.h"
#include "../core/device.h"
#include "../hw/ata.h"
#include "../hw/nvme.h"
#include "../hw/tcg.h"
#include "../hw/erase.h"

namespace app {

// One full surface pass, bucketed by the latency the read actually took. `scannedDisk` keeps
// the measurement attached to the drive it was taken on, so a scan of disk 0 cannot be
// reported as the performance of disk 2.
struct SurfaceStats {
    uint32_t fast = 0, normal = 0, mid = 0, slow = 0, bad = 0;
    uint32_t maxLatencyMs = 0;
    uint64_t blocksScanned = 0;
    int scannedDisk = -1;
};

// The counters as they stood when the operator last anchored them. Attribute values, power-on
// hours and wear are cumulative inside the drive and NO ATA/NVMe command can reset them, so
// this is the only honest "clear" this tool offers: a reference point the UI diffs against.
struct HealthBaseline {
    bool valid = false;
    std::wstring takenAt;                                // local time text of the snapshot
    std::vector<std::pair<uint8_t, uint64_t>> ataRaw;    // SMART attribute id -> raw value
    int nvmePercentageUsed = -1;
    uint64_t nvmeMediaErrors = 0;
    uint64_t nvmePowerOnHours = 0;

    bool ataRawOf(uint8_t id, uint64_t& out) const;
};

// A percentage the tool can defend. Every field it is built from is one the drive itself
// reported, `basis` names the derivation, and `known == false` makes the UI print
// "bilinmiyor" with the reason instead of a healthy-looking number out of nothing.
struct Score {
    bool known = false;
    int percent = -1;
    std::wstring basis;
    std::wstring whyUnknown;

    std::wstring valueText() const;
};

// One physical disk as the tool actually knows it. Every field is either measured or
// explicitly unknown: the first prototype filled temperature, health and bus type with
// guesses, which is what made the whole report untrustworthy.
struct DiskRecord {
    int number = -1;
    std::wstring path;              // "\\\\.\\PhysicalDrive3"
    std::wstring model, serial, firmware;
    core::BusKind bus = core::BusKind::Unknown;
    core::MediaKind media = core::MediaKind::Unknown;
    core::DiskRole role = core::DiskRole::Unknown;
    std::wstring busLabel, mediaLabel, roleLabel;

    uint64_t sizeBytes = 0;
    uint64_t totalSectors = 0;
    uint32_t logicalSector = 0;
    uint32_t physicalSector = 0;
    bool geometryKnown = false;
    bool identityFromDescriptor = false;  // STORAGE_DESCRIPTOR/INQUIRY, not ATA IDENTIFY

    // --- ATA identity, filled only when IDENTIFY DEVICE actually answered -------------
    bool ataIdentifyOk = false;
    // When it did not answer, this carries the measured refusal (IOCTL / winerror / SCSI
    // status / sense bytes) so "SMART yok" is stated as evidence rather than as a guess about
    // the bridge.
    std::wstring ataIdentifyFail;
    bool ataSmartSupported = false, ataSmartEnabled = false;
    bool ataSecuritySupported = false, ataSecurityEnabled = false;
    bool ataLocked = false, ataFrozen = false, ataCountExpired = false;
    bool ataEnhancedErase = false, ataSanitizeBit = false;
    bool ataEraseTimeValid = false;
    uint32_t ataEraseMinutesNormal = 0, ataEraseMinutesEnhanced = 0;
    hw::ata::Identify identify;

    // --- layout: what actually sits on this disk ---------------------------------------
    core::DiskLayout layout;
    std::vector<core::VolumeOnDisk> volumes;
    core::DiskAttributes attrs;

    // --- TCG ---------------------------------------------------------------------------
    bool tcgProbed = false;
    hw::tcg::DiscoveryInfo tcg;

    // --- health, per technology; both stay empty until read ---------------------------
    bool smartRead = false;
    hw::ata::SmartSnapshot smart;
    bool nvmeHealthRead = false;
    hw::nvme::HealthInfo nvmeHealth;

    // SMART/NVMe logs are a separate, on-demand read: they are cheap to refuse and expensive
    // to fake, so an empty log block must never be shown as "no errors".
    bool smartLogsRead = false;
    hw::ata::SmartLogs smartLogs;
    bool nvmeSelfTestRead = false;
    hw::nvme::SelfTestLog nvmeSelfTest;
    bool nvmeErrorsRead = false;
    std::vector<hw::nvme::ErrorInfoEntry> nvmeErrors;

    HealthBaseline baseline;

    // Reported by the device only; -1 means "the device did not say".
    int temperatureC = -1;
    std::wstring temperatureSource;
    int wearPercent = -1;
    std::wstring wearSource;         // "NVMe percentage_used" or "SMART id 231"
    int64_t reallocatedSectors = -1;
    int64_t powerOnHours = -1;

    // --- purge plan ------------------------------------------------------------------
    bool planBuilt = false;
    hw::erase::PurgeMethod purge = hw::erase::PurgeMethod::None;
    std::wstring purgeDetail;

    // Non-empty = destructive actions are refused for this disk, with the reason.
    std::wstring blockedBecause;

    bool isSystemCritical() const {
        return role == core::DiskRole::OsVolume || role == core::DiskRole::PageFile ||
               role == core::DiskRole::SystemReserved;
    }
    std::wstring sizeText() const;
    std::wstring sectorText() const;
    std::wstring securityText() const;
    std::wstring healthText() const;
    std::wstring mountStateText() const;   // offline / read-only as the disk itself reports it
    std::wstring layoutSummary() const;    // "GPT, 3 bolum" or the refusal reason
    std::wstring smartWhyText() const;     // why the attribute grid is empty, when it is
    std::wstring selfTestActivityText() const;   // running/last self-test, per technology
    std::wstring smartLogsSummary() const;       // one honest line about the log reads
    std::wstring baselineSummary() const;        // what the delta column is measured against
    Score healthScore() const;
    // The scan is a measurement of one disk, so the stats only score the disk they were taken
    // on; every other disk stays "bilinmiyor".
    Score performanceScore(const SurfaceStats& s) const;
};

// Cross-thread state. Workers write under mutex_ or through the atomics; the UI thread
// reads on its timer and never blocks a job.
class State {
public:
    std::vector<DiskRecord> disks;   // guarded by mutex_
    std::mutex mutex_;

    int selectedIndex() const { return selected_.load(); }
    void selectIndex(int i) { selected_.store(i); }
    void clearSelection() { selected_.store(-1); }

    void setStatus(const std::wstring& s);
    std::wstring status();

    // Progress channels polled by the UI timer; no window handles cross threads.
    std::atomic<bool> scanRunning{false};
    std::atomic<uint32_t> scanPercent{0};
    std::atomic<double> scanSpeedMiBs{0.0};
    std::atomic<bool> wipeRunning{false};
    std::atomic<uint32_t> wipePercent{0};
    std::atomic<double> wipeSpeedMiBs{0.0};
    std::atomic<uint32_t> wipeStep{0};
    std::atomic<uint32_t> wipeStepCount{0};
    std::wstring wipeStepLabel;      // guarded by progressMutex_
    std::mutex progressMutex_;

    SurfaceStats surface;            // guarded by mutex_

    // Bumped after any write to `disks`. The UI polls it instead of trusting a busy->idle
    // edge, because a job that finishes between two repaints would never be shown.
    std::atomic<uint64_t> disksRevision{0};
    void touchDisks() { disksRevision.fetch_add(1); }

    // Set by the UI when a destructive job should stop.
    std::wstring destructiveOverride;  // the typed confirmation phrase, empty = never confirmed

private:
    std::atomic<int> selected_{-1};
    std::wstring status_;
    std::mutex statusMutex_;
};

State& The();

// Snapshot copy for the UI (records are cheap enough to clone on a repaint).
std::vector<DiskRecord> snapshotDisks();

// Returns the record for the current selection, or nullptr.
const DiskRecord* selectedDisk();

}  // namespace app
