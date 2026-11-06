#include "workers.h"
#include "../core/log.h"
#include "../core/jobs.h"
#include "../core/str.h"
#include "../hw/scsi.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace app {
namespace workers {

namespace {

constexpr int kMaxPhysicalDrives = 64;
constexpr uint32_t kSurfaceBlockBytes = 256u * 1024u;

// Merges a freshly probed record with the deep data already collected for the same disk,
// so a plain refresh does not throw away SMART/TCG results.
void carryOver(DiskRecord& fresh, const DiskRecord& old) {
    if (old.number != fresh.number) return;
    fresh.smartRead = old.smartRead;
    fresh.smart = old.smart;
    fresh.nvmeHealthRead = old.nvmeHealthRead;
    fresh.nvmeHealth = old.nvmeHealth;
    fresh.temperatureC = old.temperatureC;
    fresh.temperatureSource = old.temperatureSource;
    fresh.wearPercent = old.wearPercent;
    fresh.wearSource = old.wearSource;
    fresh.reallocatedSectors = old.reallocatedSectors;
    fresh.powerOnHours = old.powerOnHours;
    fresh.tcgProbed = old.tcgProbed;
    fresh.tcg = old.tcg;
    fresh.planBuilt = old.planBuilt;
    fresh.purge = old.purge;
    fresh.purgeDetail = old.purgeDetail;
    fresh.smartLogsRead = old.smartLogsRead;
    fresh.smartLogs = old.smartLogs;
    fresh.nvmeSelfTestRead = old.nvmeSelfTestRead;
    fresh.nvmeSelfTest = old.nvmeSelfTest;
    fresh.nvmeErrorsRead = old.nvmeErrorsRead;
    fresh.nvmeErrors = old.nvmeErrors;
    // An operator-anchored baseline survives every refresh; only "baseline al/sifirla" moves it.
    fresh.baseline = old.baseline;
}

void updateRecord(int number, const std::function<void(DiskRecord&)>& fn) {
    State& st = The();
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        for (auto& d : st.disks) {
            if (d.number == number) { fn(d); break; }
        }
    }
    st.touchDisks();
}

// Re-reads the IDENTIFY words that carry the lock/freeze bits, so the UI cannot keep showing
// a state that the last command just changed.
void refreshSelectionIdentity(int number);

// Takes the current selection as a private copy, refusing out loud when nothing is selected.
bool selectedSnapshot(DiskRecord& rec) {
    State& st = The();
    std::lock_guard<std::mutex> lk(st.mutex_);
    const int idx = st.selectedIndex();
    if (idx < 0 || idx >= (int)st.disks.size()) {
        st.setStatus(L"Once bir disk secin");
        return false;
    }
    rec = st.disks[idx];
    return true;
}

int64_t attrRaw(const hw::ata::SmartSnapshot& s, uint8_t id) {
    for (const auto& a : s.attributes)
        if (a.id == id) return (int64_t)a.raw;
    return -1;
}

int64_t attrCurrent(const hw::ata::SmartSnapshot& s, uint8_t id) {
    for (const auto& a : s.attributes)
        if (a.id == id) return a.value;
    return -1;
}

// Vendor attributes are reported as what they are: an id plus the device's own number.
// The first prototype printed a made-up "health %" from these.
void deriveAtaHealthFacts(DiskRecord& rec) {
    const auto& s = rec.smart;
    const int64_t t194 = attrRaw(s, 194);
    const int64_t t190 = attrRaw(s, 190);
    if (t194 >= 0 && (t194 & 0xFFFF) > 0 && (t194 & 0xFFFF) < 130) {
        rec.temperatureC = (int)(t194 & 0xFFFF);
        rec.temperatureSource = L"SMART id 194 (ham dusuk 16 bit)";
    } else if (t190 >= 0 && (t190 & 0xFF) > 0 && (t190 & 0xFF) < 130) {
        rec.temperatureC = (int)(t190 & 0xFF);
        rec.temperatureSource = L"SMART id 190";
    }
    rec.reallocatedSectors = attrRaw(s, 5) >= 0 ? (attrRaw(s, 5) & 0xFFFF) : -1;
    const int64_t poh = attrRaw(s, 9);
    if (poh >= 0) rec.powerOnHours = poh;
    for (uint8_t id : {uint8_t(231), uint8_t(177), uint8_t(233), uint8_t(173), uint8_t(202)}) {
        const int64_t cur = attrCurrent(s, id);
        if (cur >= 0) {
            rec.wearPercent = (int)cur;
            rec.wearSource = L"SMART id " + core::formatU64(id) +
                             L" current degeri (anlamlandirma ureticiye aittir)";
            break;
        }
    }
}

// Purge planning needs no write access, so it belongs in the probe.
void applyPlan(core::Device& dev, DiskRecord& rec) {
    const hw::erase::PurgePlan plan = hw::erase::buildPlan(dev, rec.identify);
    rec.purge = plan.best;
    rec.purgeDetail = plan.detail;
    rec.planBuilt = true;
}

// Fills everything that only needs a read handle.
DiskRecord probeOne(int number, const DiskRecord* previous) {
    DiskRecord rec;
    rec.number = number;
    rec.path = L"\\\\.\\PhysicalDrive" + core::formatU64((uint64_t)number);

    core::Device dev(number);
    std::wstring why;
    if (!dev.openForRead()) {
        const DWORD e = GetLastError();
        // "No such drive" is a normal gap in the numbering; anything else is a real disk the
        // user should see, with the reason why it could not be opened.
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_SUCH_DEVICE) {
            rec.number = -1;
            return rec;
        }
        rec.blockedBecause = L"disk acilamadi: " + core::winErrorText(e);
        return rec;
    }

    const core::DiskGeometry& g = dev.geometry();
    rec.geometryKnown = g.valid;
    rec.sizeBytes = g.sizeBytes;
    rec.totalSectors = g.logicalSectorCount;
    rec.logicalSector = g.logicalSectorSize;
    rec.physicalSector = g.physicalSectorSize;

    rec.bus = core::diskinfo::queryBusKind(dev.handle());
    rec.busLabel = core::diskinfo::busLabel(rec.bus);

    std::wstring model = core::diskinfo::queryModel(dev);
    std::wstring serial = core::diskinfo::querySerial(dev);
    std::wstring firmware = core::diskinfo::queryFirmware(dev);
    rec.identityFromDescriptor = !model.empty() || !serial.empty();
    rec.media = core::diskinfo::queryMediaKind(dev, rec.bus, model);
    rec.mediaLabel = core::diskinfo::mediaLabel(rec.media);

    // IDENTIFY DEVICE is a SAT command: NVMe and pure-SCSI targets answer differently or
    // not at all, and that is recorded instead of guessed.
    if (rec.bus != core::BusKind::Nvme) {
        std::wstring failDetail;
        if (hw::ata::readIdentify(dev.handle(), rec.identify, &failDetail)) {
            rec.ataIdentifyOk = true;
            rec.ataIdentifyFail.clear();
            rec.ataSmartSupported = rec.identify.smartSupported();
            rec.ataSmartEnabled = rec.identify.smartEnabled();
            rec.ataSecuritySupported = rec.identify.securitySupported();
            rec.ataSecurityEnabled = rec.identify.securityEnabled();
            // Word 128 is the authoritative status field, so every lock bit comes from there.
            const hw::ata::SecurityState stt = hw::ata::securityStateOf(rec.identify);
            rec.ataEnhancedErase = stt.enhancedEraseSupported;
            rec.ataSanitizeBit = rec.identify.sanitizeSupported();
            rec.ataEraseTimeValid = rec.identify.eraseTimeValidNormal();
            rec.ataEraseMinutesNormal = rec.identify.eraseTimeMinutesNormal();
            rec.ataEraseMinutesEnhanced = rec.identify.eraseTimeMinutesEnhanced();
            rec.ataLocked = stt.locked;
            rec.ataFrozen = stt.frozen;
            rec.ataCountExpired = stt.countExpired;
            // IDENTIFY strings beat the storage descriptor when present.
            if (auto m = core::trimCopy(rec.identify.model()); !m.empty()) model = m;
            if (auto sn = core::trimCopy(rec.identify.serial()); !sn.empty()) serial = sn;
            if (auto fw = core::trimCopy(rec.identify.firmware()); !fw.empty()) firmware = fw;
        } else {
            // Recorded, not inferred: the exact layer that refused is what the UI then says.
            rec.ataIdentifyFail = failDetail;
            core::LogInfo(L"[kimlik] IDENTIFY DEVICE yanitlamadi: " +
                          (failDetail.empty() ? core::lastErrorText() : failDetail));
        }
    }
    rec.model = model.empty() ? L"(bildirilmedi)" : model;
    rec.serial = serial.empty() ? L"(bildirilmedi)" : serial;
    rec.firmware = firmware.empty() ? L"(bildirilmedi)" : firmware;

    rec.volumes = core::diskinfo::volumesOnDisk(number);
    rec.role = core::diskinfo::classifyDiskRole(number, rec.volumes);
    rec.roleLabel = core::diskinfo::roleLabel(rec.role);
    rec.layout = core::diskinfo::readDiskLayout(dev, rec.volumes);
    rec.attrs = core::diskinfo::queryDiskAttributes(dev);

    // Purge planning needs no write access, so it belongs in the probe.
    applyPlan(dev, rec);

    if (previous) carryOver(rec, *previous);
    dev.close();
    return rec;
}

void refreshJob(core::CancelToken& cancel) {
    core::LogInfo(L"[tarama] fiziksel diskler listeleniyor...");
    std::vector<DiskRecord> old;
    {
        State& st = The();
        std::lock_guard<std::mutex> lk(st.mutex_);
        old = st.disks;
    }
    std::vector<DiskRecord> found;
    for (int n = 0; n < kMaxPhysicalDrives && !cancel.cancelled(); ++n) {
        const DiskRecord* prev = nullptr;
        for (const auto& o : old) if (o.number == n) prev = &o;
        DiskRecord rec = probeOne(n, prev);
        if (rec.number < 0) continue;  // numbering gap, not a disk
        found.push_back(std::move(rec));
    }

    State& st = The();
    int selected = st.selectedIndex();
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        st.disks = std::move(found);
        for (auto& d : st.disks) {
            if (d.isSystemCritical() && d.blockedBecause.empty())
                d.blockedBecause = L"Isletim sistemi / sayfa dosyasi / EFI rezerve alani bu diskte: "
                                   L"yikici islemler onay deyimi olmadan reddedilir.";
        }
        if (selected >= (int)st.disks.size()) selected = -1;
    }
    st.selectIndex(selected);
    st.touchDisks();
    core::LogOk(L"[tarama] " + core::formatU64((uint64_t)st.disks.size()) + L" fiziksel disk bulundu");
    st.setStatus(L"Disk listesi guncel");
}

// The health reads, shared by "Derin teshis" and the automatic read that follows a selection.
// `withLogs` costs two extra commands on the ATA path, so only the deep inspection asks for
// them; everything here is a read, which is why no confirmation gates it.
bool readHealthFacts(core::Device& dev, DiskRecord& updated, bool withLogs, const wchar_t* tag) {
    bool any = false;
    if (updated.bus == core::BusKind::Nvme) {
        if (hw::nvme::readHealth(dev.handle(), updated.nvmeHealth)) {
            updated.nvmeHealthRead = updated.nvmeHealth.valid;
            if (updated.nvmeHealth.compositeTempC >= 0) {
                updated.temperatureC = updated.nvmeHealth.compositeTempC;
                updated.temperatureSource = L"NVMe CompositeTemperature";
            }
            updated.wearPercent = updated.nvmeHealth.percentageUsed;
            updated.wearSource = L"NVMe percentage_used";
            updated.powerOnHours = (int64_t)updated.nvmeHealth.powerOnHours.value;
            updated.reallocatedSectors = (int64_t)updated.nvmeHealth.mediaErrors.value;
            core::LogOk(L"[" + std::wstring(tag) + L"] NVMe saglik sayfalari okundu");
            any = true;
        } else {
            core::LogWarn(L"[" + std::wstring(tag) + L"] NVMe health log okunamadi: " +
                          updated.nvmeHealth.note);
        }
    } else if (updated.ataIdentifyOk) {
        if (!updated.ataSmartSupported) {
            core::LogWarn(L"[" + std::wstring(tag) +
                          L"] sürücü SMART desteklemiyor (word 82 bit 0 = 0)");
        } else if (!updated.ataSmartEnabled) {
            core::LogWarn(L"[" + std::wstring(tag) +
                          L"] SMART surucude kapali; bu yuzden degerler raporlanamiyor. "
                          L"SMART sekmesindeki 'SMART ac' ham yazma erisimi (bolumleri cikarma) "
                          L"gerektirir.");
            updated.smart.note = L"SMART kapali (surucu bildirimi)";
            updated.smartRead = true;
            any = true;
        } else {
            hw::ata::SmartSnapshot snap;
            if (hw::ata::readSmart(dev.handle(), updated.identify, snap)) {
                updated.smart = snap;
                updated.smartRead = true;
                deriveAtaHealthFacts(updated);
                core::LogOk(L"[" + std::wstring(tag) + L"] SMART okundu: " +
                            core::formatU64((uint64_t)snap.attributes.size()) +
                            L" atr, saglik=" + std::wstring(hw::ata::healthLabel(snap.health)) +
                            L" | self-test: " + snap.selfTestStatusText() +
                            (snap.note.empty() ? L"" : L" | " + snap.note));
                any = true;
                if (withLogs) {
                    hw::ata::SmartLogs logs;
                    updated.smartLogsRead = true;
                    if (hw::ata::readSmartLogs(dev.handle(), logs)) {
                        updated.smartLogs = logs;
                        core::LogOk(L"[smart-log] " + logs.summary());
                    } else {
                        updated.smartLogs = logs;
                        core::LogWarn(L"[smart-log] " + updated.smartLogsSummary());
                    }
                }
            } else {
                core::LogFail(L"[" + std::wstring(tag) + L"] SMART verisi alinamadi: " + snap.note);
            }
        }
    } else {
        core::LogWarn(L"[" + std::wstring(tag) +
                      L"] kimlik komutu bu yolda desteklenmiyor; SMART/NVMe verisi yok" +
                      (updated.ataIdentifyFail.empty() ? std::wstring()
                                                       : L" | " + updated.ataIdentifyFail));
    }
    return any;
}

void deepInspectJob(core::CancelToken& cancel) {
    State& st = The();
    const int idx = st.selectedIndex();
    DiskRecord rec;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        if (idx < 0 || idx >= (int)st.disks.size()) { st.setStatus(L"Once bir disk secin"); return; }
        rec = st.disks[idx];
    }
    core::Device dev(rec.number);
    if (!dev.openForRead()) {
        core::LogFail(L"[inceleme] disk okunamadi: " + core::lastErrorText());
        return;
    }

    DiskRecord updated = rec;
    bool any = readHealthFacts(dev, updated, true, L"inceleme");

    if (cancel.cancelled()) return;

    hw::tcg::DiscoveryInfo disco;
    if (hw::tcg::discoverLevel0(dev, disco)) {
        updated.tcg = disco;
        updated.tcgProbed = true;
        core::LogOk(L"[tcg] " + disco.summary());
        any = true;
    } else {
        updated.tcg = disco;
        updated.tcgProbed = true;
        core::LogInfo(L"[tcg] " + disco.summary());
    }

    dev.close();

    if (any || updated.tcgProbed) {
        {
            std::lock_guard<std::mutex> lk(st.mutex_);
            for (auto& d : st.disks)
                if (d.number == updated.number) { d = updated; break; }
        }
        st.touchDisks();
    }
    st.setStatus(updated.healthText().empty() ? L"Inceleme tamamlandi" : updated.healthText());
}

// The read that follows a selection: only the facts the gauges are built from, so clicking a
// drive fills the summary strip without waiting for "Derin teshis". No TCG discovery, no log
// round-trips, no writes - the whole job is the same two or three commands the deep inspection
// already issues.
void smartReadJob(core::CancelToken& cancel) {
    State& st = The();
    const int idx = st.selectedIndex();
    DiskRecord rec;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        if (idx < 0 || idx >= (int)st.disks.size()) return;
        rec = st.disks[idx];
    }
    // Skip when there is nothing to gain: the drive either already answered, or the SAT path is
    // already known to be refused for it - re-asking would only spam the log with the same no.
    const bool done = (rec.bus == core::BusKind::Nvme)
                          ? rec.nvmeHealthRead
                          : (rec.smartRead || !rec.ataIdentifyOk);
    if (done) return;
    if (cancel.cancelled()) return;

    core::Device dev(rec.number);
    if (!dev.openForRead()) {
        core::LogFail(L"[saglik] disk okunamadi: " + core::lastErrorText());
        return;
    }
    DiskRecord updated = rec;
    const bool any = readHealthFacts(dev, updated, false, L"saglik");
    dev.close();
    if (!any) return;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        for (auto& d : st.disks)
            if (d.number == updated.number) { d = updated; break; }
    }
    st.touchDisks();
}

void surfaceScanJob(core::CancelToken& cancel) {
    State& st = The();
    const int idx = st.selectedIndex();
    DiskRecord rec;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        if (idx < 0 || idx >= (int)st.disks.size()) { st.setStatus(L"Once bir disk secin"); return; }
        rec = st.disks[idx];
    }
    if (!rec.geometryKnown) { core::LogFail(L"[yuzey] geometri bilinmiyor"); return; }

    core::Device dev(rec.number);
    if (!dev.openForUnbufferedRead()) {
        core::LogFail(L"[yuzey] FILE_FLAG_NO_BUFFERING ile acilamadi: " + core::lastErrorText() +
                      L" (bazi birimler mount edilmişken reddeder)");
        return;
    }
    const uint32_t sec = rec.logicalSector ? rec.logicalSector : 512u;
    const uint32_t blockSectors = (std::max)(uint64_t(kSurfaceBlockBytes / sec), uint64_t(1));
    core::AlignedBuffer buf(size_t(blockSectors) * sec);
    if (!buf) { core::LogFail(L"[yuzey] tampon ayiramadi"); return; }

    st.scanRunning = true;
    st.scanPercent = 0;
    SurfaceStats stats{};
    stats.scannedDisk = rec.number;
    {
        // Publish the ownership before the first block is timed, so a scan that is stopped
        // immediately cannot leave the previous drive's numbers attached to this one.
        std::lock_guard<std::mutex> lk(st.mutex_);
        st.surface = stats;
    }
    const uint64_t total = rec.totalSectors;
    const auto started = std::chrono::steady_clock::now();
    core::LogInfo(L"[yuzey] " + rec.sizeText() + L" taranmaya basladi (" +
                  core::formatU64(blockSectors) + L" sektor/blok)");

    for (uint64_t lba = 0; lba < total; lba += blockSectors) {
        if (cancel.cancelled()) break;
        const uint64_t n = (std::min)(uint64_t(blockSectors), total - lba);
        const auto t0 = std::chrono::steady_clock::now();
        std::wstring err;
        const bool ok = dev.readSectors(lba, buf.data(), n, err);
        const uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
        if (!ok) stats.bad++;
        else if (ms < 5) stats.fast++;
        else if (ms < 20) stats.normal++;
        else if (ms < 60) stats.mid++;
        else if (ms < 150) stats.slow++;
        else stats.bad++;
        stats.maxLatencyMs = (std::max)(stats.maxLatencyMs, ms);
        stats.blocksScanned++;
        st.scanPercent = (uint32_t)((lba + n) * 100 / total);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (el > 0) st.scanSpeedMiBs = double((lba + n) * sec) / (1024.0 * 1024.0) / el;
        if ((stats.blocksScanned & 0x3F) == 0) {
            std::lock_guard<std::mutex> lk(st.mutex_);
            st.surface = stats;
        }
    }
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        st.surface = stats;
    }
    dev.close();
    st.scanRunning = false;
    core::LogInfo(L"[yuzey] bitti: hizli=" + core::formatU64(stats.fast) + L" normal=" +
                  core::formatU64(stats.normal) + L" orta=" + core::formatU64(stats.mid) +
                  L" yavas=" + core::formatU64(stats.slow) + L" hata=" + core::formatU64(stats.bad) +
                  L" | maks " + core::formatU64(stats.maxLatencyMs) + L"ms");
    st.setStatus(L"Yuzey taramasi tamamlandi");
}

void inspectLba0Job(core::CancelToken&) {
    State& st = The();
    const int idx = st.selectedIndex();
    DiskRecord rec;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        if (idx < 0 || idx >= (int)st.disks.size()) { st.setStatus(L"Once bir disk secin"); return; }
        rec = st.disks[idx];
    }
    core::Device dev(rec.number);
    if (!dev.openForUnbufferedRead()) {
        core::LogFail(L"[lba0] disk okunamadi: " + core::lastErrorText());
        return;
    }
    const uint32_t sec = rec.logicalSector ? rec.logicalSector : 512u;
    core::AlignedBuffer buf(sec);
    std::wstring err;
    if (!dev.readSectors(0, buf.data(), 1, err)) {
        core::LogFail(L"[lba0] LBA 0 okunamadi: " + err);
        return;
    }
    core::LogCmd(L"[lba0] PhysicalDrive" + core::formatU64((uint64_t)rec.number) + L" ilk sektor:");
    for (uint32_t off = 0; off < sec; off += 16) {
        wchar_t line[128];
        int pos = swprintf(line, 128, L"  %04X  ", (unsigned)off);
        for (uint32_t i = 0; i < 16 && off + i < sec; ++i)
            pos += swprintf(line + pos, 128 - pos, L"%02X ", buf.data()[off + i]);
        core::LogInfo(line);
    }
    const uint8_t* b = buf.data();
    std::wstring verdict;
    if (b[510] == 0x55 && b[511] == 0xAA) verdict = L"MBR imzasi (55AA) var";
    else verdict = L"MBR imzasi yok";
    if (sec >= 512 && memcmp(b + 512 - 2, "\x55\xAA", 2) == 0 && b[3] == 'E' && b[5] == 'F' &&
        b[7] == 'I' && b[9] == 'G' && b[11] == 'A')
        verdict += L" | ayrica GPT header (EFI PART)";
    core::LogInfo(L"[lba0] degerlendirme: " + verdict);
    dev.close();
}

// Opens the disk for raw writes; every destructive job starts here.
bool openForDestruction(core::Device& dev, std::wstring& why) {
    if (!dev.openForRawWrite()) {
        why = L"Ham yazma erisimi alinamadi: " + core::lastErrorText() +
              L" (yonetici gerekir / bolum mount edilmiş olabilir)";
        return false;
    }
    return true;
}

void finishDestructive(int number, const std::wstring& statusLine) {
    State& st = The();
    std::wstring err;
    core::Device dev(number);
    if (dev.openForRawWrite()) {
        std::wstring why;
        hw::erase::rereadPartitionTable(dev, why);
        dev.close();
    }
    std::vector<std::wstring> failed;
    core::diskinfo::remountVolumes(number, failed);
    for (const auto& f : failed) core::LogWarn(L"[montaj] " + f + L" tekrar baglanamadi");
    st.setStatus(statusLine);
    core::Device probe(number);
    if (probe.openForRead()) {
        DiskRecord fresh = probeOne(number, nullptr);
        {
            std::lock_guard<std::mutex> lk(st.mutex_);
            for (auto& d : st.disks)
                if (d.number == number) { carryOver(fresh, d); d = fresh; break; }
        }
        st.touchDisks();
    }
}

void wipeTablesJob(core::CancelToken&) {
    State& st = The();
    std::wstring why;
    if (!ensureSafeToDestroy(why)) { core::LogFail(L"[silme] " + why); st.setStatus(why); return; }
    const DiskRecord rec = *selectedDisk();
    core::Device dev(rec.number);
    if (!openForDestruction(dev, why)) { core::LogFail(L"[silme] " + why); return; }

    hw::erase::TableWipeResult r = hw::erase::wipePartitionTables(dev, why);
    if (!r.completed) {
        core::LogFail(L"[bolum tablosu] " + r.error);
        dev.close();
        return;
    }
    std::wstring err;
    dev.flush(err);
    hw::erase::rereadPartitionTable(dev, err);
    dev.close();
    core::LogOk(L"[bolum tablosu] on " + core::formatU64(r.frontSectors) + L" sektor + son " +
                core::formatU64(r.backSectors) + L" sektor sifirlandi");
    if (!r.note.empty()) core::LogWarn(L"[bolum tablosu] " + r.note);
    finishDestructive(rec.number, L"Bolumleme tablosu silindi");
}

void overwriteJob(core::CancelToken& cancel, hw::erase::Profile profile) {
    State& st = The();
    std::wstring why;
    if (!ensureSafeToDestroy(why)) { core::LogFail(L"[ezme] " + why); st.setStatus(why); return; }
    const DiskRecord rec = *selectedDisk();

    core::Device dev(rec.number);
    if (!openForDestruction(dev, why)) { core::LogFail(L"[ezme] " + why); return; }

    st.wipeRunning = true;
    st.wipeStepCount = hw::erase::passesFor(profile).size() + hw::erase::verificationPassCount(profile);
    core::LogCmd(L"[ezme] " + std::wstring(hw::erase::profileLabel(profile)) + L" | hedef PhysicalDrive" +
                 core::formatU64((uint64_t)rec.number) + L" (" + rec.sizeText() + L")");

    hw::erase::Result r = hw::erase::overwrite(
        dev, profile, cancel,
        [&](const hw::erase::Progress& p) {
            st.wipePercent = p.totalBytes ? (uint32_t)(p.doneBytes * 100 / p.totalBytes) : 0;
            st.wipeSpeedMiBs = p.mibPerSec;
            st.wipeStep = p.passIndex;
            {
                std::lock_guard<std::mutex> lk(st.progressMutex_);
                st.wipeStepLabel = p.stepLabel;
            }
            st.setStatus(p.stepLabel + L" %" + core::formatU64(st.wipePercent));
        },
        true);

    st.wipeRunning = false;
    if (!r.error.empty()) {
        (r.cancelled ? core::LogWarn(L"[ezme] " + r.error) : core::LogFail(L"[ezme] " + r.error));
    }
    if (r.completed) {
        core::LogOk(L"[ezme] tamamlandi: " + core::formatBytes(r.bytesWritten) + L" yazildi, " +
                    core::formatU64(r.stepsDone) + L" adim");
        if (r.verifyMismatchSectors)
            core::LogWarn(L"[ezme] dogrulama: " + core::formatU64(r.verifyMismatchSectors) +
                          L" sektor beklenen deseni taşımıyor");
    }
    if (!r.note.empty()) core::LogInfo(L"[ezme not] " + r.note);
    dev.close();
    if (r.completed) finishDestructive(rec.number, L"Ezme tamamlandi");
    else st.setStatus(r.cancelled ? L"Ezme iptal edildi" : L"Ezme basarisiz");
}

void purgeJob(core::CancelToken&, bool enhanced, std::wstring password, bool useMaster) {
    State& st = The();
    std::wstring why;
    if (!ensureSafeToDestroy(why)) { core::LogFail(L"[purge] " + why); st.setStatus(why); return; }
    const DiskRecord rec = *selectedDisk();
    if (rec.purge == hw::erase::PurgeMethod::None) {
        core::LogFail(L"[purge] sürücü donanım purge desteklemiyor ya da kilitli: " + rec.purgeDetail);
        st.setStatus(L"Donanim purge mevcut degil");
        return;
    }
    std::string pw;
    if (rec.bus != core::BusKind::Nvme && !password.empty()) {
        std::wstring pwWhy;
        if (!hw::ata::normalizePassword(password, pw, pwWhy)) {
            core::LogFail(L"[purge] parola: " + pwWhy);
            return;
        }
    }

    core::Device dev(rec.number);
    if (!openForDestruction(dev, why)) { core::LogFail(L"[purge] " + why); return; }

    uint32_t timeoutSec = 600;
    if (rec.ataEraseTimeValid)
        timeoutSec = (std::max)(600u, (enhanced ? rec.ataEraseMinutesEnhanced : rec.ataEraseMinutesNormal) * 60u + 300u);
    const hw::erase::PurgeMethod method =
        enhanced && rec.ataEnhancedErase ? hw::erase::PurgeMethod::AtaSecureEraseEnh : rec.purge;

    core::LogCmd(L"[purge] " + std::wstring(hw::erase::purgeMethodLabel(method)) + L" gonderiliyor (" +
                 core::formatU64(timeoutSec) + L"s zaman asimi)");
    hw::CmdResult r = hw::erase::runPurge(dev, method, pw, useMaster, timeoutSec);
    dev.close();
    if (r.deviceOk) {
        core::LogOk(L"[purge] surucu komutu kabul etti: " + hw::describeResult(r));
        finishDestructive(rec.number, L"Donanim purge tamamlandi");
    } else {
        core::LogFail(L"[purge] basarisiz: " + hw::describeResult(r));
        st.setStatus(L"Donanim purge basarisiz - ezme kullanin");
    }
}

void ataSecurityJob(core::CancelToken&, int op, std::wstring password, bool useMaster) {
    State& st = The();
    const DiskRecord* rec = selectedDisk();
    if (!rec) { st.setStatus(L"Once bir disk secin"); return; }
    if (rec->bus == core::BusKind::Nvme) {
        core::LogFail(L"[ata-guvenlik] NVMe surucusunde ATA Security komutlari yoktur; "
                      L"TCG/Opal ya da NVMe Format yolu kullanin.");
        return;
    }
    if (!rec->ataIdentifyOk) {
        core::LogFail(L"[ata-guvenlik] IDENTIFY DEVICE yanit vermedi; bu yolda (USB kopru?) "
                      L"ATA Security komutlari taşınamıyor.");
        return;
    }
    std::string pw;
    std::wstring why;
    if (op != 2 /* freeze */ && !hw::ata::normalizePassword(password, pw, why)) {
        core::LogFail(L"[ata-guvenlik] " + why);
        st.setStatus(why);
        return;
    }

    // ATA Security is a write-side feature: Windows only hands it out with write access,
    // which in turn needs the volumes offline. Destructive only for erase, so use a plain
    // raw-write open and keep the volume state visible in the log.
    core::Device dev(rec->number);
    if (!dev.openForRawWrite()) {
        const auto dm = core::diskinfo::dismountVolumes(rec->number);
        if (!dm.stillBusy.empty()) {
            std::wstring list;
            for (auto& s : dm.stillBusy) list += s + L", ";
            core::LogFail(L"[ata-guvenlik] bolumler mesgul: " + list +
                          L"- komut gonderilmedi.");
            st.setStatus(L"Bolumler kapatilamadi");
            return;
        }
        if (!dev.openForRawWrite()) {
            core::LogFail(L"[ata-guvenlik] yazma erisimi yok: " + core::lastErrorText());
            st.setStatus(L"Erisim reddedildi");
            return;
        }
    }

    hw::CmdResult r;
    const wchar_t* name = L"";
    switch (op) {
    case 0:
        name = L"SECURITY UNLOCK";
        r = hw::ata::securityUnlock(dev.handle(), useMaster, pw);
        break;
    case 1:
        name = L"SECURITY DISABLE PASSWORD";
        r = hw::ata::securityDisablePassword(dev.handle(), useMaster, pw);
        break;
    case 2:
        name = L"SECURITY FREEZE LOCK";
        r = hw::ata::securityFreezeLock(dev.handle());
        break;
    case 3:
        name = L"SECURITY SET PASSWORD";
        r = hw::ata::securitySetPassword(dev.handle(), useMaster, pw, false, 0);
        break;
    default: break;
    }
    dev.close();
    if (r.deviceOk) core::LogOk(L"[ata-guvenlik] " + std::wstring(name) + L": " + hw::describeResult(r));
    else core::LogFail(L"[ata-guvenlik] " + std::wstring(name) + L": " + hw::describeResult(r));
    if (op == 2) core::LogWarn(L"[ata-guvenlik] FREEZE tek yolludur: parola komutlari power "
                               L"cycle olmadan reddedilir.");
    st.setStatus(std::wstring(name) + (r.deviceOk ? L" basarili" : L" basarisiz"));

    // The lock/freeze bits live in IDENTIFY; re-read them so the UI cannot show a stale state.
    refreshSelectionIdentity(rec->number);
}

void tcgDiscoverJob(core::CancelToken&) {
    State& st = The();
    const DiskRecord* recPtr = selectedDisk();
    if (!recPtr) { st.setStatus(L"Once bir disk secin"); return; }
    const int number = recPtr->number;
    core::Device dev(number);
    if (!dev.openForRead()) {
        core::LogFail(L"[tcg] disk okunamadi: " + core::lastErrorText());
        return;
    }
    hw::tcg::DiscoveryInfo info;
    const bool ok = hw::tcg::discoverLevel0(dev, info);
    updateRecord(number, [&](DiskRecord& d) {
        d.tcg = info;
        d.tcgProbed = true;
    });
    dev.close();
    core::Log().write(ok ? core::LogLevel::Ok : core::LogLevel::Warn, L"[tcg] " + info.summary());
    st.setStatus(info.summary());
}

void psidRevertJob(core::CancelToken&, std::wstring psid) {
    State& st = The();
    std::wstring why;
    if (!ensureSafeToDestroy(why)) { core::LogFail(L"[psid] " + why); st.setStatus(why); return; }
    const DiskRecord* recPtr = selectedDisk();
    if (!recPtr) return;
    const int number = recPtr->number;
    if (!recPtr->tcgProbed || (!recPtr->tcg.hasOpal && !recPtr->tcg.hasTPer)) {
        core::LogWarn(L"[psid] Level 0 Discovery OPAL/TCG gostermedi; yine de denenebilir "
                      L"(bazi Suruculer discovery'yi kilitli durumda kismen yanitlar).");
    }
    core::Device dev(number);
    if (!dev.openForRawWrite()) {
        core::LogFail(L"[psid] TRUSTED SEND icin ham yazma erisimi gerekli: " + core::lastErrorText());
        st.setStatus(L"Erisim reddedildi");
        return;
    }
    st.setStatus(L"PSID Revert calisiyor...");
    const hw::tcg::RevertResult r = hw::tcg::psidRevert(dev, psid);
    dev.close();
    if (r.revertAccepted) {
        core::LogOk(L"[psid] " + r.detail);
        finishDestructive(number, L"PSID Revert tamamlandi");
    } else {
        core::LogFail(L"[psid] " + r.detail);
        st.setStatus(L"PSID Revert basarisiz");
    }
}

void refreshSelectionIdentity(int number) {
    State& st = The();
    core::Device dev(number);
    if (!dev.openForRead()) return;
    hw::ata::Identify id;
    if (!hw::ata::readIdentify(dev.handle(), id)) return;
    const hw::ata::SecurityState s = hw::ata::securityStateOf(id);
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        for (auto& d : st.disks) {
            if (d.number != number) continue;
            d.identify = id;
            d.ataIdentifyOk = true;
            d.ataSmartSupported = id.smartSupported();
            d.ataSmartEnabled = id.smartEnabled();
            d.ataLocked = s.locked;
            d.ataFrozen = s.frozen;
            d.ataCountExpired = s.countExpired;
            d.ataSecurityEnabled = s.enabled;
            d.ataEnhancedErase = s.enhancedEraseSupported;
            break;
        }
    }
    st.touchDisks();
}

// Re-reads one disk through the same read-only probe the first scan uses. carryOver keeps the
// deep-inspection results, so pressing this after plugging something in costs an IDENTIFY and
// nothing else.
void relayoutJob(core::CancelToken& cancel) {
    State& st = The();
    DiskRecord prev;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        const int idx = st.selectedIndex();
        if (idx < 0 || idx >= (int)st.disks.size()) {
            st.setStatus(L"Once bir disk secin");
            return;
        }
        prev = st.disks[idx];
    }
    if (cancel.cancelled()) return;
    DiskRecord fresh = probeOne(prev.number, &prev);
    if (fresh.number < 0) {
        core::LogWarn(L"[harita] PhysicalDrive" + core::formatU64((uint64_t)prev.number) +
                      L" artik listede yok");
        return;
    }
    updateRecord(fresh.number, [&](DiskRecord& d) { d = fresh; });
    if (fresh.layout.read)
        core::LogOk(L"[harita] " + core::formatU64((uint64_t)fresh.layout.parts.size()) +
                    L" bolum kaydi | " + fresh.layoutSummary());
    else
        core::LogFail(L"[harita] bolum tablosu okunamadi: " + fresh.layout.errorText);
    st.setStatus(fresh.layoutSummary());
}

// Read-only and offline are the two mount flags the disk attribute IOCTL can actually write.
// They are reversible and destroy no data, but a system disk's mount state is refused: taking
// the volume that holds %SystemRoot% offline is a way to break a running machine by accident.
void diskAttrJob(core::CancelToken& cancel, uint64_t setBits, uint64_t clearBits,
                 std::wstring label) {
    State& st = The();
    int number = -1;
    bool critical = false;
    {
        std::lock_guard<std::mutex> lk(st.mutex_);
        const int idx = st.selectedIndex();
        if (idx < 0 || idx >= (int)st.disks.size()) {
            st.setStatus(L"Once bir disk secin");
            return;
        }
        number = st.disks[idx].number;
        critical = st.disks[idx].isSystemCritical();
    }
    if (critical) {
        core::LogFail(L"[bayrak] " + label +
                      L" reddedildi: disk isletim sistemi / pagefile / EFI rezerve alani tasiyor.");
        st.setStatus(L"Sistem diskining mount durumu degistirilmedi");
        return;
    }
    std::wstring why;
    if (!core::diskinfo::setDiskAttributeFlags(number, setBits, clearBits, why)) {
        core::LogFail(L"[bayrak] " + label + L" uygulanamadi: " + why);
        st.setStatus(L"Bayrak degistirilemedi - ayrinti logda");
        return;
    }
    core::LogOk(L"[bayrak] PhysicalDrive" + core::formatU64((uint64_t)number) + L": " + label +
                L" uygulandi");
    relayoutJob(cancel);
}

// --- SMART management (logs, self-test, enable/disable, baseline) -------------------

std::wstring nowText() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t buf[40];
    swprintf(buf, 40, L"%04u-%02u-%02u %02u:%02u:%02u", (unsigned)t.wYear, (unsigned)t.wMonth,
             (unsigned)t.wDay, (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond);
    return buf;
}

// Health jobs own a well-defined subset of the record; committing only those fields keeps a
// concurrent mount/layout refresh from being overwritten with a stale copy.
void commitSmartFields(int number, const DiskRecord& updated) {
    updateRecord(number, [&](DiskRecord& d) {
        d.smartRead = updated.smartRead;
        d.smart = updated.smart;
        d.smartLogsRead = updated.smartLogsRead;
        d.smartLogs = updated.smartLogs;
        d.nvmeHealthRead = updated.nvmeHealthRead;
        d.nvmeHealth = updated.nvmeHealth;
        d.nvmeSelfTestRead = updated.nvmeSelfTestRead;
        d.nvmeSelfTest = updated.nvmeSelfTest;
        d.nvmeErrorsRead = updated.nvmeErrorsRead;
        d.nvmeErrors = updated.nvmeErrors;
        d.baseline = updated.baseline;
        d.temperatureC = updated.temperatureC;
        d.temperatureSource = updated.temperatureSource;
        d.wearPercent = updated.wearPercent;
        d.wearSource = updated.wearSource;
        d.reallocatedSectors = updated.reallocatedSectors;
        d.powerOnHours = updated.powerOnHours;
    });
}

// Reads whatever the disk's own technology exposes: SMART READ DATA (+ logs when asked) for
// ATA, the NVMe health/self-test/error pages for NVMe.
bool readSmartInto(core::Device& dev, DiskRecord& rec, bool withLogs) {
    State& st = The();
    if (rec.bus == core::BusKind::Nvme) {
        bool any = false;
        if (hw::nvme::readHealth(dev.handle(), rec.nvmeHealth)) {
            rec.nvmeHealthRead = rec.nvmeHealth.valid;
            if (rec.nvmeHealth.compositeTempC >= 0) {
                rec.temperatureC = rec.nvmeHealth.compositeTempC;
                rec.temperatureSource = L"NVMe CompositeTemperature";
            }
            rec.wearPercent = rec.nvmeHealth.percentageUsed;
            rec.wearSource = L"NVMe percentage_used";
            rec.powerOnHours = (int64_t)rec.nvmeHealth.powerOnHours.value;
            rec.reallocatedSectors = (int64_t)rec.nvmeHealth.mediaErrors.value;
            core::LogOk(L"[smart] NVMe saglik sayiasi okundu");
            any = true;
        } else {
            core::LogWarn(L"[smart] NVMe saglik sayiasi: " + rec.nvmeHealth.note);
        }
        if (withLogs) {
            if (hw::nvme::readSelfTestLog(dev.handle(), rec.nvmeSelfTest)) {
                rec.nvmeSelfTestRead = true;
                core::LogOk(L"[smart] NVMe self-test logu: " + rec.nvmeSelfTest.statusText() +
                            L" | " + core::formatU64((uint64_t)rec.nvmeSelfTest.results.size()) +
                            L" kayit");
                any = true;
            } else {
                core::LogWarn(L"[smart] NVMe self-test logu: " + rec.nvmeSelfTest.note);
            }
            std::wstring why;
            if (hw::nvme::readErrorInfoLog(dev.handle(), rec.nvmeErrors, why)) {
                rec.nvmeErrorsRead = true;
                core::LogOk(L"[smart] NVMe hata logu: " +
                            core::formatU64((uint64_t)rec.nvmeErrors.size()) + L" kayit");
                any = true;
            } else {
                core::LogWarn(L"[smart] NVMe hata logu: " + why);
            }
        }
        st.setStatus(rec.nvmeHealth.valid ? rec.nvmeHealth.criticalWarningText()
                                          : L"NVMe saglik logu alinamadi");
        return any;
    }

    if (!rec.ataIdentifyOk || !rec.ataSmartSupported) {
        core::LogWarn(L"[smart] " + rec.smartWhyText());
        st.setStatus(L"SMART yok - sebep logda");
        return false;
    }
    hw::ata::SmartSnapshot snap;
    const bool ok = hw::ata::readSmart(dev.handle(), rec.identify, snap);
    rec.smart = snap;
    rec.smartRead = true;
    if (ok) {
        deriveAtaHealthFacts(rec);
        core::LogOk(L"[smart] " + core::formatU64((uint64_t)snap.attributes.size()) +
                    L" attribute | saglik=" + std::wstring(hw::ata::healthLabel(snap.health)) +
                    L" | " + snap.offlineStatusText() + L" | self-test: " +
                    snap.selfTestStatusText() + (snap.note.empty() ? L"" : L" | " + snap.note));
    } else {
        core::LogFail(L"[smart] READ DATA basarisiz: " + snap.note);
    }
    if (withLogs) {
        hw::ata::SmartLogs logs;
        rec.smartLogsRead = true;  // a refusal is an answer the grid has to show
        if (hw::ata::readSmartLogs(dev.handle(), logs)) {
            rec.smartLogs = logs;
            core::LogOk(L"[smart-log] " + logs.summary());
        } else {
            rec.smartLogs = logs;
            core::LogWarn(L"[smart-log] " + rec.smartLogsSummary());
        }
    }
    st.setStatus(ok ? std::wstring(hw::ata::healthLabel(snap.health)) : L"SMART okunamadi");
    return ok;
}

// A SMART log read is a pure read: it needs no write access, never needs the destructive gate
// and works on a mounted, in-use disk.
void smartLogsJob(core::CancelToken& cancel) {
    DiskRecord rec;
    if (!selectedSnapshot(rec)) return;
    core::Device dev(rec.number);
    if (!dev.openForRead()) {
        core::LogFail(L"[smart] disk okunamadi: " + core::lastErrorText());
        return;
    }
    if (cancel.cancelled()) return;
    readSmartInto(dev, rec, true);
    dev.close();
    commitSmartFields(rec.number, rec);
}

// SMART write-side operations. Windows only hands GENERIC_WRITE on a PhysicalDrive while no
// volume of it is mounted, so each of these either reports "command accepted" or names the
// exact gate that stopped it - never a silent failure.
void smartCtlJob(core::CancelToken& cancel, SmartControl op, std::wstring label) {
    State& st = The();
    DiskRecord rec;
    if (!selectedSnapshot(rec)) return;
    if (rec.bus == core::BusKind::Nvme && op != SmartControl::selfTestShort &&
        op != SmartControl::selfTestExtended) {
        std::wstring why;
        if (op == SmartControl::enableOperations || op == SmartControl::disableOperations)
            why = L"NVMe surucusunde SMART ac/kapat komutu yoktur; saglik logu her zaman aciktir.";
        else if (op == SmartControl::clearErrorLog || op == SmartControl::autosaveOn ||
                 op == SmartControl::autosaveOff)
            why = L"NVMe hata logu (page 01h) sadece-okunurdur; temizleme veya autosave "
                  L"karsiligi yoktur.";
        else
            why = L"NVMe'de self-testi durduran bant-ustu bir komut yoktur; abort yalnizca "
                  L"ATA'nin 7Fh kodundadir.";
        core::LogFail(L"[smart] " + why);
        st.setStatus(why);
        return;
    }

    core::Device dev(rec.number);
    if (!dev.openForRawWrite()) {
        // Never dismount an in-use system volume to make a health command fit: the command is
        // worth less than the session the user is running.
        if (rec.isSystemCritical()) {
            core::LogFail(L"[smart] " + label +
                          L" reddedildi: disk isletim sistemi/pagefile/EFI tasiyor ve Windows "
                          L"monteli bolum uzerinde ham yazma vermiyor. Ayni komut icin surucuyu "
                          L"baska bir makinede (veya on-yuklemede) calistirin.");
            st.setStatus(L"Yazma erisimi yok - ayrinti logda");
            return;
        }
        const auto dm = core::diskinfo::dismountVolumes(rec.number);
        if (!dm.stillBusy.empty()) {
            std::wstring list;
            for (const auto& s : dm.stillBusy) list += s + L", ";
            core::LogFail(L"[smart] " + label + L" icin bolumler mesgul: " + list +
                          L"- komut gonderilmedi.");
            st.setStatus(L"Bolumler kapatilamadi - ayrinti logda");
            return;
        }
        if (!dev.openForRawWrite()) {
            core::LogFail(L"[smart] " + label + L" icin ham yazma erisimi alinamadi: " +
                          core::lastErrorText());
            st.setStatus(L"Erisim reddedildi - ayrinti logda");
            return;
        }
    }
    if (cancel.cancelled()) return;

    hw::CmdResult r;
    if (rec.bus == core::BusKind::Nvme) {
        // `label` carries the ATA opcode names because that is what most of the buttons are;
        // on NVMe the log has to say which command was actually put on the wire.
        label += op == SmartControl::selfTestShort ? L" -> NVMe Device Self-test (14h, STC=1)"
                                                   : L" -> NVMe Device Self-test (14h, STC=2)";
        std::wstring why;
        r = hw::nvme::deviceSelfTest(dev.handle(), uint8_t(op == SmartControl::selfTestShort ? 1 : 2),
                                     0xFFFFFFFFu, why);
        if (!r.deviceOk && r.detail.empty()) r.detail = why;
    } else {
        switch (op) {
        case SmartControl::enableOperations:   r = hw::ata::smartSetOperations(dev.handle(), true); break;
        case SmartControl::disableOperations:  r = hw::ata::smartSetOperations(dev.handle(), false); break;
        case SmartControl::autosaveOn:    r = hw::ata::smartSetAutoSave(dev.handle(), true); break;
        case SmartControl::autosaveOff:   r = hw::ata::smartSetAutoSave(dev.handle(), false); break;
        case SmartControl::selfTestShort:     r = hw::ata::smartExecuteSelfTest(dev.handle(), hw::ata::stShort); break;
        case SmartControl::selfTestExtended:  r = hw::ata::smartExecuteSelfTest(dev.handle(), hw::ata::stExtended); break;
        case SmartControl::selfTestAbort:     r = hw::ata::smartExecuteSelfTest(dev.handle(), hw::ata::stAbort); break;
        case SmartControl::clearErrorLog: r = hw::ata::smartClearErrorLog(dev.handle()); break;
        default: break;
        }
    }
    dev.close();

    if (r.deviceOk) {
        core::LogOk(L"[smart] " + label + L": komut kabul edildi (" + hw::describeResult(r) + L")");
        if (op == SmartControl::selfTestShort || op == SmartControl::selfTestExtended) {
            const uint16_t est =
                rec.bus == core::BusKind::Nvme
                    ? 0u
                    : uint16_t(op == SmartControl::selfTestShort ? rec.smart.shortTestMinutes
                                                 : rec.smart.extendedTestMinutes);
            core::LogInfo(L"[smart] test suruceye girdi; tahmini sure " +
                          (est ? core::formatU64(est) + L" dk" : L"suruucu tahmin vermedi") +
                          L". Sure dolunca 'Loglari oku' ile sonucu okutun - sonuc logdan gelir, "
                          L"uydurulmaz.");
        }
    } else {
        core::LogFail(L"[smart] " + label + L" reddedildi: " + hw::describeResult(r));
        st.setStatus(label + L" basarisiz - ayrinti logda");
    }

    // Whatever happened, the visible state comes back from the device rather than from hope:
    // an accepted ENABLE must show up in word 85, a rejected clear must still show the old log.
    core::Device probe(rec.number);
    if (probe.openForRead()) {
        readSmartInto(probe, rec, r.deviceOk);
        probe.close();
        if (op == SmartControl::enableOperations || op == SmartControl::disableOperations) refreshSelectionIdentity(rec.number);
    }
    commitSmartFields(rec.number, rec);
    if (r.deviceOk) st.setStatus(label + L" uygulanildi");
}

// "SMART temizleme" has exactly one honest meaning left after the command layer: re-anchor the
// baseline. The drive's own counters are cumulative and no standard command resets them.
void smartBaselineJob(core::CancelToken& cancel, bool take) {
    State& st = The();
    DiskRecord rec;
    if (!selectedSnapshot(rec)) return;
    if (!take) {
        rec.baseline = HealthBaseline{};
        commitSmartFields(rec.number, rec);
        core::LogInfo(L"[baseline] PhysicalDrive" + core::formatU64((uint64_t)rec.number) +
                      L" icin baseline silindi; delta sutunu tekrar bos.");
        st.setStatus(L"Baseline silindi");
        return;
    }
    core::Device dev(rec.number);
    if (!dev.openForRead()) {
        core::LogFail(L"[baseline] disk okunamadi: " + core::lastErrorText());
        return;
    }
    if (!cancel.cancelled()) readSmartInto(dev, rec, false);
    dev.close();

    HealthBaseline b;
    b.takenAt = nowText();
    if (rec.bus == core::BusKind::Nvme) {
        if (!rec.nvmeHealthRead || !rec.nvmeHealth.valid) {
            core::LogFail(L"[baseline] NVMe saglik sayiasi okunamadi; karsilastirilacak deger yok.");
            st.setStatus(L"Baseline alinamadi");
            return;
        }
        b.valid = true;
        b.nvmePercentageUsed = int(rec.nvmeHealth.percentageUsed);
        b.nvmeMediaErrors = rec.nvmeHealth.mediaErrors.value;
        b.nvmePowerOnHours = rec.nvmeHealth.powerOnHours.value;
    } else {
        if (!rec.smartRead || rec.smart.attributes.empty()) {
            core::LogFail(L"[baseline] SMART attribute yok: " + rec.smartWhyText());
            st.setStatus(L"Baseline alinamadi - ayrinti logda");
            return;
        }
        b.valid = true;
        for (const auto& a : rec.smart.attributes) b.ataRaw.push_back({a.id, a.raw});
    }
    rec.baseline = b;
    commitSmartFields(rec.number, rec);
    core::LogOk(L"[baseline] PhysicalDrive" + core::formatU64((uint64_t)rec.number) + L" | " +
                b.takenAt + L" | " + core::formatU64((uint64_t)b.ataRaw.size()) +
                L" SMART sayaci referans alindi. Delta sutunu sonraki okumalarda artisi gosterir; "
                L"surucunun kendi sayaclari sifirlanmaz.");
    st.setStatus(L"Baseline alindi: " + b.takenAt);
}

}  // namespace

bool busy() { return core::Jobs().busy(); }
std::wstring currentJobName() { return core::Jobs().currentJobName(); }
void postStopActiveJob() {
    core::Jobs().requestCancel();
    core::LogInfo(L"[is] iptal istegi gonderildi; mevcut komutun bitmesi bekleniyor");
}

std::wstring confirmationPhrase(int diskNumber) {
    return L"FIZIKSELDISK" + core::formatU64((uint64_t)diskNumber);
}

bool ensureSafeToDestroy(std::wstring& why) {
    State& st = The();
    if (!core::processIsAdmin()) {
        why = L"Yonetici degil: ham disk yazmasi Windows tarafindan reddedilir.";
        return false;
    }
    const DiskRecord* rec = selectedDisk();
    if (!rec) { why = L"Disk secili degil."; return false; }
    if (rec->blockedBecause.size() > 0 || rec->isSystemCritical()) {
        const std::wstring phrase = confirmationPhrase(rec->number);
        if (st.destructiveOverride != phrase) {
            why = L"PhysicalDrive" + core::formatU64((uint64_t)rec->number) + L" (" + rec->model +
                  L", " + rec->roleLabel + L") icin onay gerekiyor. Onay deyimi: " + phrase;
            return false;
        }
    }
    const auto dm = core::diskinfo::dismountVolumes(rec->number);
    if (!dm.stillBusy.empty()) {
        std::wstring list;
        for (const auto& s : dm.stillBusy) list += s + L", ";
        why = L"Su birimler mesgul: " + list +
              L" - kismen kapatilmis disk ASLA silinmez. once dosya paylasimlarini/dizin "
              L"servisini kapatip tekrar deneyin.";
        return false;
    }
    if (!dm.dismounted.empty())
        core::LogInfo(L"[güvenlik] " + core::formatU64((uint64_t)dm.dismounted.size()) +
                      L" birimin baglantisi kesildi");
    return true;
}

void postRefresh() {
    core::Jobs().post(L"Disk listesi", [](core::CancelToken& t) { refreshJob(t); });
}
void postDeepInspect() {
    core::Jobs().post(L"Derin inceleme", [](core::CancelToken& t) { deepInspectJob(t); });
}
void postSmartRead() {
    core::Jobs().post(L"SMART okuma", [](core::CancelToken& t) { smartReadJob(t); });
}
void postSurfaceScan() {
    core::Jobs().post(L"Yuzey taramasi", [](core::CancelToken& t) { surfaceScanJob(t); });
}
void postWipePartitionTables() {
    core::Jobs().post(L"Bolum tablosu silme", [](core::CancelToken& t) { wipeTablesJob(t); });
}
void postOverwrite(hw::erase::Profile profile) {
    core::Jobs().post(L"Ezme", [profile](core::CancelToken& t) { overwriteJob(t, profile); });
}
void postHardwarePurge(bool enhanced, const std::wstring& password, bool useMaster) {
    core::Jobs().post(L"Donanim purge", [=](core::CancelToken& t) {
        purgeJob(t, enhanced, password, useMaster);
    });
}
void postAtaSetPassword(const std::wstring& password, bool useMaster) {
    core::Jobs().post(L"ATA SET PASSWORD", [=](core::CancelToken& t) {
        ataSecurityJob(t, 3, password, useMaster);
    });
}
void postAtaUnlock(const std::wstring& password, bool useMaster) {
    core::Jobs().post(L"ATA UNLOCK", [=](core::CancelToken& t) { ataSecurityJob(t, 0, password, useMaster); });
}
void postAtaDisablePassword(const std::wstring& password, bool useMaster) {
    core::Jobs().post(L"ATA DISABLE PASSWORD",
                      [=](core::CancelToken& t) { ataSecurityJob(t, 1, password, useMaster); });
}
void postAtaFreeze() {
    core::Jobs().post(L"ATA FREEZE", [](core::CancelToken& t) { ataSecurityJob(t, 2, L"", false); });
}
void postTcgDiscover() {
    core::Jobs().post(L"TCG Discovery", [](core::CancelToken& t) { tcgDiscoverJob(t); });
}
void postPsidRevert(const std::wstring& psid) {
    core::Jobs().post(L"PSID Revert", [psid](core::CancelToken& t) { psidRevertJob(t, psid); });
}
void postInspectLba0() {
    core::Jobs().post(L"LBA0 hex", [](core::CancelToken& t) { inspectLba0Job(t); });
}
void postReadNvmeHealth() { postDeepInspect(); }
void postRelayout() {
    core::Jobs().post(L"Bolum haritasi", [](core::CancelToken& t) { relayoutJob(t); });
}
void postSetDiskReadOnly(bool readOnly) {
    core::Jobs().post(L"Disk yazma korumasi", [readOnly](core::CancelToken& t) {
        diskAttrJob(t, readOnly ? core::diskinfo::kDiskAttrReadOnly : 0,
                    core::diskinfo::kDiskAttrReadOnly,
                    readOnly ? L"read-only ACIK" : L"read-only KAPALI");
    });
}
void postSetDiskOffline(bool offline) {
    core::Jobs().post(L"Disk mount durumu", [offline](core::CancelToken& t) {
        diskAttrJob(t, offline ? core::diskinfo::kDiskAttrOffline : 0,
                    core::diskinfo::kDiskAttrOffline,
                    offline ? L"cevrimdisi birak" : L"cevrimici al");
    });
}

void postSmartLogs() {
    core::Jobs().post(L"SMART loglari", [](core::CancelToken& t) { smartLogsJob(t); });
}

// The label is what the operator reads back in the log and status line, so it travels with the
// operation instead of being rebuilt at each call site.
void postSmartControl(SmartControl op) {
    std::wstring name = L"SMART komutu", label;
    switch (op) {
    case SmartControl::enableOperations:
        name = L"SMART ac"; label = L"SMART ENABLE OPERATIONS (D8h)"; break;
    case SmartControl::disableOperations:
        name = L"SMART kapat"; label = L"SMART DISABLE OPERATIONS (D9h)"; break;
    case SmartControl::autosaveOn:
        name = L"Autosave ac"; label = L"SMART autosave AC (D2h/F1h)"; break;
    case SmartControl::autosaveOff:
        name = L"Autosave kapat"; label = L"SMART autosave KAPA (D2h/F2h)"; break;
    case SmartControl::selfTestShort:
        name = L"Kisa self-test"; label = L"KISA self-test (D4h/01h)"; break;
    case SmartControl::selfTestExtended:
        name = L"Uzun self-test"; label = L"UZUN self-test (D4h/02h)"; break;
    case SmartControl::selfTestAbort:
        name = L"Self-testi durdur"; label = L"self-test ABORT (D4h/7Fh)"; break;
    case SmartControl::clearErrorLog:
        name = L"Hata logunu temizle"; label = L"WRITE LOG ile hata logu sifirlama (D6h/01h)"; break;
    default: break;
    }
    core::Jobs().post(name, [op, label](core::CancelToken& t) { smartCtlJob(t, op, label); });
}

void postSmartBaseline(bool take) {
    core::Jobs().post(take ? L"Baseline al" : L"Baseline sil",
                      [take](core::CancelToken& t) { smartBaselineJob(t, take); });
}

bool writeReport(const std::wstring& path, std::wstring& why) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { why = core::lastErrorText(); return false; }
    auto write = [&](const std::string& s) {
        DWORD put = 0;
        WriteFile(h, s.data(), (DWORD)s.size(), &put, nullptr);
    };
    const std::string u8 = "\xEF\xBB\xBF";
    write(u8);
    write("DiskUnlock raporu\r\n\r\n");
    for (const auto& d : snapshotDisks()) {
        write("PhysicalDrive" + core::narrow(std::to_wstring(d.number)) + " | " +
              core::narrow(d.model) + " | " + core::narrow(d.serial) + " | " +
              core::narrow(d.busLabel) + " | " + core::narrow(d.mediaLabel) + " | " +
              core::narrow(d.sizeText()) + " | " + core::narrow(d.sectorText()) + "\r\n");
        write("  rol: " + core::narrow(d.roleLabel) + "\r\n");
        write("  guvenlik: " + core::narrow(d.securityText()) + "\r\n");
        write("  saglik: " + core::narrow(d.healthText()) + "\r\n");
        write("  mount: " + core::narrow(d.mountStateText()) + " | harita: " +
              core::narrow(d.layoutSummary()) + "\r\n");
        if (d.layout.read) {
            for (const auto& p : d.layout.parts) {
                if (p.unused) continue;
                write("    bolum " + core::narrow(core::formatU64(p.index)) + " tip=" +
                      core::narrow(p.typeText) + " lba=" + core::narrow(core::formatU64(p.startLba)) +
                      " sektor=" + core::narrow(core::formatU64(p.sectorCount)) + " boyut=" +
                      core::narrow(core::formatBytes(p.sizeBytes)) +
                      (p.letters.empty() ? std::string(" harf=yok")
                                         : " harf=" + core::narrow(p.letters)) +
                      (p.bootable ? " boot" : "") + (p.hidden ? " gizli" : "") +
                      (p.noLetter ? " harfsiz" : "") + "\r\n");
            }
        }
        for (const auto& v : d.volumes)
            write("    volume " + core::narrow(v.mountPoint.empty() ? L"(bagsiz)" : v.mountPoint) +
                  " fs=" + core::narrow(v.fileSystem.empty() ? L"?" : v.fileSystem) +
                  " kapasite=" + core::narrow(core::formatBytes(v.capacityBytes)) +
                  " bos=" + core::narrow(core::formatBytes(v.freeBytes)) +
                  (v.bootForCurrentOs ? " BOOT" : "") +
                  (v.pagefileCandidate ? " PAGEFILE" : "") + "\r\n");
        if (d.geometryKnown && d.sizeBytes)
            write("  sicaklik kaynagi: " + (d.temperatureC >= 0 ? core::narrow(d.temperatureSource)
                                                                 : std::string("surucu bildirmiyor")) + "\r\n");
        if (d.smartRead) {
            write("  SMART atr sayisi: " + core::narrow(std::to_wstring((uint64_t)d.smart.attributes.size())) + "\r\n");
            for (const auto& a : d.smart.attributes)
                write("    id=" + core::narrow(std::to_wstring(a.id)) + " " + core::narrow(a.label()) +
                      " value=" + core::narrow(std::to_wstring(a.value)) +
                      " worst=" + core::narrow(std::to_wstring(a.worst)) +
                      " raw=" + core::narrow(std::to_wstring(a.raw)) + "\r\n");
            write("    self-test: " + core::narrow(d.smart.selfTestStatusText()) + "\r\n");
            write("    offline: " + core::narrow(d.smart.offlineStatusText()) + "\r\n");
        } else {
            write("  SMART: " + core::narrow(d.smartWhyText()) + "\r\n");
        }
        write("  smart loglari: " + core::narrow(d.smartLogsSummary()) + "\r\n");
        for (const auto& e : d.smartLogs.errors)
            write("    hata#" + core::narrow(core::formatU64(e.index)) + " poh=" +
                  core::narrow(core::formatU64(e.powerOnHours)) + " cmd=" +
                  core::narrow(core::formatHex(e.commandRegister)) + " err=" +
                  core::narrow(core::formatHex(e.errorRegister)) + " lba=" +
                  core::narrow(core::formatU64(e.lba)) + " durum=" +
                  core::narrow(e.stateText()) + "\r\n");
        for (const auto& e : d.smartLogs.selfTests)
            write("    test#" + core::narrow(core::formatU64(e.index)) + " " +
                  core::narrow(hw::ata::selfTestCodeLabel(e.typeCode)) + " poh=" +
                  core::narrow(core::formatU64(e.powerOnHours)) + " -> " +
                  core::narrow(e.statusText()) + "\r\n");
        for (const auto& e : d.nvmeSelfTest.results)
            write("    nvme test " + core::narrow(e.opText()) + " poh=" +
                  core::narrow(core::formatU64(e.powerOnHours)) + " -> " +
                  core::narrow(e.resultText()) + "\r\n");
        for (const auto& e : d.nvmeErrors)
            write("    nvme " + core::narrow(e.describe()) + "\r\n");
        write("  baseline: " + core::narrow(d.baselineSummary()) + "\r\n");
        if (d.planBuilt)
            write("  purge: " + core::narrow(std::wstring(hw::erase::purgeMethodLabel(d.purge))) +
                  " | " + core::narrow(d.purgeDetail) + "\r\n");
        if (!d.blockedBecause.empty())
            write("  ENGEL: " + core::narrow(d.blockedBecause) + "\r\n");
        write("\r\n");
    }
    CloseHandle(h);
    return true;
}

}  // namespace workers
}  // namespace app
