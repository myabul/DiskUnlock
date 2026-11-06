#include "state.h"
#include "../core/str.h"

namespace app {

std::wstring DiskRecord::sizeText() const {
    if (!geometryKnown || sizeBytes == 0) return L"bilinmiyor";
    return core::formatBytes(sizeBytes);
}

std::wstring DiskRecord::sectorText() const {
    if (!geometryKnown || logicalSector == 0) return L"bilinmiyor";
    std::wstring s = core::formatU64(logicalSector) + L"B mantiksal";
    if (physicalSector && physicalSector != logicalSector)
        s += L" / " + core::formatU64(physicalSector) + L"B fiziksel";
    else if (logicalSector == 4096)
        s += L" (4Kn)";
    return s;
}

std::wstring DiskRecord::securityText() const {
    std::wstring s;
    if (ataIdentifyOk) {
        if (!ataSecuritySupported) s += L"ATA Security yok";
        else {
            s += ataSecurityEnabled ? L"ATA Security AKTIF" : L"ATA Security tanimli, parola yok";
            if (ataLocked) s += L" | KILITLI";
            if (ataFrozen) s += L" | FREEZE";
            if (ataCountExpired) s += L" | deneme sayisi tukendi";
            if (ataEnhancedErase) s += L" | enhanced erase";
        }
    } else if (bus == core::BusKind::Nvme) {
        s += L"ATA Security degil (NVMe Format/Sanitize)";
    } else {
        s += L"ATA kimligi okunamadi";
    }
    if (tcgProbed) s += L" | TCG: " + tcg.summary();
    return s;
}

std::wstring DiskRecord::healthText() const {
    std::wstring s;
    if (nvmeHealthRead && nvmeHealth.valid) {
        s += L"NVMe: " + core::formatU64(nvmeHealth.percentageUsed) + L"% kullanim";
        if (nvmeHealth.compositeTempC >= 0)
            s += L" | " + core::formatU64(uint64_t(nvmeHealth.compositeTempC)) + L"C";
        s += L" | saglik: " + nvmeHealth.criticalWarningText();
    } else if (smartRead) {
        s += std::wstring(hw::ata::healthLabel(smart.health));
        if (temperatureC >= 0) s += L" | " + core::formatU64(uint64_t(temperatureC)) + L"C";
        if (wearPercent >= 0)
            s += L" | asinma %" + core::formatU64(uint64_t(wearPercent)) + L" (" + wearSource + L")";
        if (reallocatedSectors >= 0)
            s += L" | yeniden ayirmalar: " + core::formatU64(uint64_t(reallocatedSectors));
    } else {
        s += L"Saglik okunmadi - derin inceleme gerekiyor";
    }
    if (!s.empty() && (!smart.note.empty())) s += L" | " + smart.note;
    return s;
}

std::wstring DiskRecord::mountStateText() const {
    if (!attrs.read)
        return attrs.errorText.empty() ? L"durum okunamadi"
                                       : L"durum okunamadi: " + attrs.errorText;
    std::wstring s = attrs.offline ? L"OFFLINE" : L"online";
    if (attrs.readOnly) s += L" | YAZMA KORUMALI";
    if (attrs.notReady) s += L" | hazir degil";
    return s;
}

std::wstring DiskRecord::layoutSummary() const {
    if (!layout.read)
        return layout.errorText.empty() ? L"bolum tablosu okunamadi"
                                       : L"bolum tablosu okunamadi: " + layout.errorText;
    uint32_t used = 0;
    for (const auto& p : layout.parts)
        if (!p.unused) ++used;
    std::wstring s = layout.style + L", " + core::formatU64(used) + L" bolum";
    if (layout.style == L"GPT")
        s += L" | kullanilabilir LBA " + core::formatU64(layout.usableStartLba) + L"-" +
             core::formatU64(layout.usableEndLba);
    return s;
}

bool HealthBaseline::ataRawOf(uint8_t id, uint64_t& out) const {
    for (const auto& e : ataRaw)
        if (e.first == id) { out = e.second; return true; }
    return false;
}

std::wstring DiskRecord::smartWhyText() const {
    if (smartRead && !smart.attributes.empty()) return L"";
    if (bus == core::BusKind::Nvme) {
        std::wstring s = L"NVMe surucusunde ATA SMART yoktur; saglik verisi NVMe Health Log "
                         L"(log page 02h) satirlarindadir.";
        if (!nvmeHealthRead)
            s += L" NVMe logu henuz okunmadi - disk secildiginde kendiliginden okunur, "
                 L"'Derin teshis' de dahil eder.";
        else if (!nvmeHealth.valid) s += L" NVMe logu da yanitlamadi: " + nvmeHealth.note;
        return s;
    }
    if (!ataIdentifyOk) {
        // The measured refusal beats the category guess: "USB köprüsü taşımıyor" is a theory,
        // the winerror and the sense bytes are what actually happened on this device.
        std::wstring evidence = ataIdentifyFail.empty() ? L"" : L" Olcülen ret: " + ataIdentifyFail;
        if (identityFromDescriptor)
            return L"Bu cihaza IDENTIFY DEVICE ulasilamiyor (kimlik STORAGE_DESCRIPTOR/INQUIRY "
                   L"ile geldi; USB/Virtual/RAID koprusu ATA komutlarini tasimiyor). SMART "
                   L"ATAnin bir parcasidir, burada TANIMLI DEGIL - bu yuzden tablo bos." +
                   evidence;
        return L"IDENTIFY DEVICE yanit vermedi; SMART desteklenip desteklenmedigi bilinmiyor." +
               evidence;
    }
    if (!ataSmartSupported)
        return L"Surucu IDENTIFY kelime 82 bit 0'da SMART desteklemedigini raporluyor.";
    if (!ataSmartEnabled)
        return L"SURUCUDE SMART KAPALI (kelime 85 bit 0 = 0); bu yuzden hicbir attribute "
               L"raporlanmiyor. 'SMART ac' komutu ham yazma erisimi ister: once bolumleri "
               L"cikarin.";
    if (!smart.note.empty()) return L"SMART READ DATA deger döndürmedi: " + smart.note;
    return L"SMART destekleniyor ve acik, fakat READ DATA attribute döndürmedi.";
}

std::wstring DiskRecord::selfTestActivityText() const {
    if (bus == core::BusKind::Nvme)
        return nvmeSelfTestRead ? nvmeSelfTest.statusText() : L"self-test logu okunmadi";
    if (!smartRead) return L"SMART okunmadi; self-test durumu bilinmiyor";
    std::wstring s = smart.selfTestStatusText();
    if (smart.shortTestMinutes)
        s += L" | surucunun tahmini: kisa " + core::formatU64(smart.shortTestMinutes) + L" dk";
    if (smart.extendedTestMinutes)
        s += L", uzun " + core::formatU64(smart.extendedTestMinutes) + L" dk";
    return s;
}

std::wstring DiskRecord::smartLogsSummary() const {
    if (bus == core::BusKind::Nvme) {
        if (!nvmeSelfTestRead && !nvmeErrorsRead) return L"NVMe loglari okunmadi";
        std::wstring s;
        if (nvmeSelfTestRead)
            s += L"self-test: " + core::formatU64((uint64_t)nvmeSelfTest.results.size()) +
                 L" kayit (" + nvmeSelfTest.statusText() + L")";
        else
            s += nvmeSelfTest.note.empty() ? L"self-test logu okunamadi" : nvmeSelfTest.note;
        s += nvmeErrorsRead
                 ? L" | hata logu: " + core::formatU64((uint64_t)nvmeErrors.size()) + L" kayit"
                 : L" | hata logu okunamadi";
        return s;
    }
    if (!smartLogsRead) return L"SMART loglari okunmadi";
    if (!smartLogs.anything())
        return (smartLogs.errorNote.empty() ? L"" : smartLogs.errorNote) +
               (smartLogs.selfTestNote.empty() ? L"" : L" | " + smartLogs.selfTestNote);
    return smartLogs.summary();
}

std::wstring DiskRecord::baselineSummary() const {
    if (!baseline.valid)
        return L"baseline yok: delta sutunu bos. 'Baseline al' ile simdi ki degerleri "
               L"referans noktası yapin.";
    std::wstring s = L"baseline " + baseline.takenAt + L" | " +
                     core::formatU64((uint64_t)baseline.ataRaw.size()) + L" SMART sayaci";
    if (bus == core::BusKind::Nvme || nvmeHealthRead)
        s += L" | NVMe %kullanim " +
             (baseline.nvmePercentageUsed >= 0 ? core::formatU64(uint64_t(baseline.nvmePercentageUsed))
                                               : L"-") +
             L", POH " + core::formatU64(baseline.nvmePowerOnHours) + L", media errors " +
             core::formatU64(baseline.nvmeMediaErrors);
    s += L" | NOT: surucunun kendi sayaclari hicbir komutla sifirlanamaz; bu yalnizca karsilastirma "
         L"noktasi.";
    return s;
}

std::wstring Score::valueText() const {
    if (!known) return L"bilinmiyor";
    return L"%" + core::formatU64((uint64_t)(percent < 0 ? 0 : percent));
}

// Health is only ever derived from what the drive itself reported, and the derivation is
// spelled out in `basis` so the number can be argued with. Nothing here guesses a value for
// a field the device did not answer.
Score DiskRecord::healthScore() const {
    Score sc;
    if (bus == core::BusKind::Nvme) {
        if (!nvmeHealthRead) {
            sc.whyUnknown =
                L"NVMe Health Log (page 02h) henuz okunmadi - disk secilince otomatik okunur.";
            return sc;
        }
        if (!nvmeHealth.valid) {
            sc.whyUnknown = L"NVMe Health Log yanitlamadi: " + nvmeHealth.note;
            return sc;
        }
        const int used = nvmeHealth.percentageUsed > 100 ? 100 : int(nvmeHealth.percentageUsed);
        sc.percent = 100 - used;
        sc.basis = L"NVMe Health Log: 100 - percentage_used (" +
                   core::formatU64(nvmeHealth.percentageUsed) + L") | critical_warning = " +
                   nvmeHealth.criticalWarningText();
        if (nvmeHealth.availableSpareThreshold)
            sc.basis += L" | spare " + core::formatU64(nvmeHealth.availableSpare) + L"% (esik " +
                        core::formatU64(nvmeHealth.availableSpareThreshold) + L"%)";
        // The media and persistent-memory bits are the controller saying stored data is no
        // longer trustworthy; no wear estimate can keep a number above zero after that.
        if (nvmeHealth.criticalWarning & (0x04 | 0x20)) {
            sc.percent = 0;
            sc.basis += L" | controller guvenilmez veri bildirdi -> 0";
        } else if (nvmeHealth.availableSpareThreshold &&
                   nvmeHealth.availableSpare <= nvmeHealth.availableSpareThreshold) {
            sc.percent = 0;
            sc.basis += L" | available_spare esigin altinda -> 0";
        }
        sc.known = true;
        return sc;
    }
    if (!smartRead || smart.attributes.empty()) {
        sc.whyUnknown = smartWhyText();
        if (sc.whyUnknown.empty())
            sc.whyUnknown = L"SMART okunmadi - saglik yuzdesi uydurulamaz.";
        return sc;
    }
    if (smart.health == hw::ata::Health::Failed) {
        sc.known = true;
        sc.percent = 0;
        sc.basis = L"SMART RETURN STATUS 'failed' dondu (LBA mid/high = F4h/2Ch).";
        return sc;
    }
    int best = 100, counted = 0;
    std::wstring worst;
    for (const auto& a : smart.attributes) {
        // Only vendor-prefailure attributes with a real threshold carry information: a
        // threshold of 0 means "no limit", and an analysis attribute's value is a running
        // counter, not a health margin.
        if (!a.thresholdValid || !a.preFailure() || a.threshold == 0 || a.threshold >= 100)
            continue;
        ++counted;
        const int margin =
            a.value <= a.threshold ? 0 : int(100 * (int(a.value) - int(a.threshold)) /
                                                    (100 - int(a.threshold)));
        if (margin < best) {
            best = margin;
            worst = a.label() + L" (value " + core::formatU64(uint64_t(a.value)) + L", esik " +
                    core::formatU64(uint64_t(a.threshold)) + L")";
        }
    }
    if (!counted) {
        sc.whyUnknown =
            L"Surucu hicbir PREFAIL attribute icin gecerli esik degeri raporlamadi; bu "
            L"kosulda yuzde turetmek sayilarin kendisinden daha fazla iddia olur.";
        if (smart.health == hw::ata::Health::Passed)
            sc.whyUnknown += L" (SMART RETURN STATUS 'passed' - bu ayri bir bilgidir.)";
        return sc;
    }
    sc.known = true;
    sc.percent = best;
    sc.basis = L"ATA SMART: " + core::formatU64(uint64_t(counted)) +
               L" esikli prefail attribute, en dusuk margin " +
               (worst.empty() ? L"esik asilmadi" : worst) +
               L" | formul (value-esik)/(100-esik).";
    if (smart.health == hw::ata::Health::Indeterminate)
        sc.basis += L" | SMART RETURN STATUS belirsiz";
    return sc;
}

// Performance is a measurement, not an attribute: the surface pass is the only thing here that
// times reads, so a drive that was never scanned has no percentage - and a scan of another
// drive is not evidence about this one.
Score DiskRecord::performanceScore(const SurfaceStats& s) const {
    Score sc;
    if (s.scannedDisk != number) {
        sc.whyUnknown = s.scannedDisk < 0
                            ? L"yuzey taramasi henuz calistirilmadi - performans olcumden "
                              L"turetilmez, uydurulmaz"
                            : L"son yuzey taramasi PhysicalDrive" +
                                  core::formatU64((uint64_t)s.scannedDisk) +
                                  L" uzerinde yapildi; bu surucu icin olcum yok";
        return sc;
    }
    if (s.blocksScanned == 0) {
        sc.whyUnknown = L"yuzey taramasi hic blok olcmedi (erken durduruldu?)";
        return sc;
    }
    const uint64_t good = uint64_t(s.fast) + s.normal;
    sc.known = true;
    sc.percent = int(100 * good / s.blocksScanned);
    sc.basis = L"yuzey taramasi: hizli(<5ms) " + core::formatU64(s.fast) + L" + normal(<20ms) " +
               core::formatU64(s.normal) + L" / " + core::formatU64(s.blocksScanned) +
               L" blok | orta " + core::formatU64(s.mid) + L" yavas " + core::formatU64(s.slow) +
               L" hatali " + core::formatU64(s.bad) + L" | maks " +
               core::formatU64(s.maxLatencyMs) + L"ms";
    if (s.bad) sc.basis += L" | okunamayan blok var";
    return sc;
}

void State::setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_ = s;
}

std::wstring State::status() {
    std::lock_guard<std::mutex> lk(statusMutex_);
    return status_.empty() ? L"Hazir" : status_;
}

State& The() {
    static State state;
    return state;
}

std::vector<DiskRecord> snapshotDisks() {
    State& st = The();
    std::lock_guard<std::mutex> lk(st.mutex_);
    return st.disks;
}

const DiskRecord* selectedDisk() {
    static thread_local DiskRecord copy;
    State& st = The();
    const int idx = st.selectedIndex();
    std::lock_guard<std::mutex> lk(st.mutex_);
    if (idx < 0 || idx >= (int)st.disks.size()) return nullptr;
    copy = st.disks[idx];
    return &copy;
}

}  // namespace app
