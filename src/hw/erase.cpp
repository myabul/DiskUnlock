#include "erase.h"
#include "../core/str.h"
#include <random>

namespace hw {
namespace erase {

namespace {

constexpr uint32_t kChunkBytes = 8u * 1024u * 1024u;
constexpr uint64_t kFrontBytes = 1024ull * 1024ull;  // the GPT reserved area at LBA 0
constexpr uint64_t kBackupGptSectors = 33;           // 32 backup entries + backup header

uint8_t patternByte(Pattern p) {
    switch (p) {
    case Pattern::Zero: return 0x00;
    case Pattern::Ones: return 0xFF;
    case Pattern::Pseudorandom: return 0x00;  // unused; random bytes are generated per chunk
    }
    return 0x00;
}

// A repeating single-chunk pattern, as the first prototype did, is not a random pass: the
// same bytes simply cycle. Fresh words are drawn for every chunk instead.
void fillRandom(std::mt19937_64& rng, uint8_t* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const uint64_t v = rng();
        memcpy(p + i, &v, 8);
    }
    if (i < n) {
        const uint64_t v = rng();
        memcpy(p + i, &v, n - i);
    }
}

double mibPerSec(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return 0.0;
    return double(bytes) / (1024.0 * 1024.0) / seconds;
}

using clock_ = std::chrono::steady_clock;

}  // namespace

const wchar_t* profileLabel(Profile p) {
    switch (p) {
    case Profile::NistClearOnePass:  return L"NIST SP 800-88r1 Clear - tek gecis 0x00";
    case Profile::SinglePassRandom:  return L"Tek gecis pseudorandom";
    case Profile::DoDStyleThreePass: return L"DoD 5220.22-M deseni - 0x00 / 0xFF / pseudorandom";
    }
    return L"Bilinmeyen profil";
}

const std::vector<Pass>& passesFor(Profile p) {
    static const std::vector<Pass> clear{{Pattern::Zero, L"Gecis 1/1: 0x00 sifirlama"}};
    static const std::vector<Pass> random{{Pattern::Pseudorandom, L"Gecis 1/1: pseudorandom"}};
    static const std::vector<Pass> dod{
        {Pattern::Zero, L"Gecis 1/3: 0x00 sifirlama"},
        {Pattern::Ones, L"Gecis 2/3: 0xFF bit deseni"},
        {Pattern::Pseudorandom, L"Gecis 3/3: pseudorandom desen"},
    };
    switch (p) {
    case Profile::NistClearOnePass:  return clear;
    case Profile::SinglePassRandom:  return random;
    case Profile::DoDStyleThreePass: return dod;
    }
    return clear;
}

uint32_t verificationPassCount(Profile p) { (void)p; return 1; }

Result overwrite(core::Device& dev, Profile profile, core::CancelToken& cancel,
                 const ProgressFn& onProgress, bool verifyLastPass) {
    Result res;
    const core::DiskGeometry& g = dev.geometry();
    if (!g.valid || g.logicalSectorCount == 0) {
        res.error = L"Disk geometrisi alinamadi; silme baslatilamadi.";
        return res;
    }
    const uint32_t sec = g.logicalSectorSize ? g.logicalSectorSize : 512u;
    const uint64_t totalSectors = g.logicalSectorCount;

    uint32_t chunkSectors = kChunkBytes / sec;
    if (chunkSectors == 0) chunkSectors = 1;
    core::AlignedBuffer buf(size_t(chunkSectors) * sec);
    if (!buf) {
        res.error = L"Silme tamponu icin sayfa hizali bellek ayrilamadi.";
        return res;
    }

    const std::vector<Pass>& passes = passesFor(profile);
    Progress prog;
    prog.totalBytes = totalSectors * sec;
    prog.totalSteps = uint32_t(passes.size()) + (verifyLastPass ? 1u : 0u);

    std::mt19937_64 rng(std::random_device{}());
    std::wstring err;

    for (size_t pi = 0; pi < passes.size(); ++pi) {
        if (cancel.cancelled()) {
            res.cancelled = true;
            res.error = L"Kullanici tarafindan iptal edildi.";
            return res;
        }
        const Pass& pass = passes[pi];
        prog.passIndex = uint32_t(pi);
        prog.stepLabel = pass.label;
        prog.doneBytes = 0;
        if (onProgress) onProgress(prog);

        const auto stepStart = clock_::now();
        for (uint64_t lba = 0; lba < totalSectors;) {
            if (cancel.cancelled()) {
                res.cancelled = true;
                res.error = L"Kullanici tarafindan iptal edildi (LBA " + core::formatU64(lba) +
                            L"). Disk bu gecis ile tam olarak ezilmedi.";
                return res;
            }
            const uint64_t n = (std::min)(uint64_t(chunkSectors), totalSectors - lba);
            const size_t bytes = size_t(n) * sec;
            if (pass.pattern == Pattern::Pseudorandom) fillRandom(rng, buf.data(), bytes);
            else memset(buf.data(), patternByte(pass.pattern), bytes);

            if (!dev.writeSectors(lba, buf.data(), n, err)) {
                res.error = L"Yazma basarisiz (LBA " + core::formatU64(lba) + L"): " + err;
                res.note = L"Disk bu noktada birakildi; onceki gecisler tamamlanmis olabilir.";
                return res;
            }
            lba += n;
            prog.doneBytes = lba * sec;
            res.bytesWritten += bytes;
            prog.mibPerSec = mibPerSec(prog.doneBytes,
                std::chrono::duration<double>(clock_::now() - stepStart).count());
            if (onProgress) onProgress(prog);
        }

        // Buffered writes only reach the media once the handle is flushed; skipping this is
        // how a "completed" wipe can leave the tail of the disk untouched.
        if (!dev.flush(err)) res.note = L"Flush basarisiz: " + err + L" (veriler onbellekte kalmis olabilir)";
        res.stepsDone = uint32_t(pi) + 1;
    }

    if (!verifyLastPass) {
        res.completed = true;
        return res;
    }

    const Pass& last = passes.back();
    prog.passIndex = uint32_t(passes.size());
    prog.doneBytes = 0;
    prog.stepLabel = last.pattern == Pattern::Pseudorandom
                         ? L"Dogrulama: son gecis okunuyor (desen karsilastirmasi yok)"
                         : L"Dogrulama: son gecis deseni karsilastiriliyor";
    if (onProgress) onProgress(prog);
    const auto verifyStart = clock_::now();

    if (last.pattern == Pattern::Pseudorandom) {
        // The generator state for earlier chunks is gone, so the only honest check left is
        // that every sector now reads back without a media error.
        res.note = L"Pseudorandom gecis yeniden uretilmedigi icin icerik karsilastirmasi "
                   L"yapilamadi; yalnizca okunabilirlik dogrulandi.";
        for (uint64_t lba = 0; lba < totalSectors;) {
            if (cancel.cancelled()) {
                res.cancelled = true;
                res.error = L"Dogrulama iptal edildi.";
                return res;
            }
            const uint64_t n = (std::min)(uint64_t(chunkSectors), totalSectors - lba);
            if (!dev.readSectors(lba, buf.data(), n, err)) {
                res.verifyMismatchSectors += n;
                res.note += L" | LBA " + core::formatU64(lba) + L" okunamadi: " + err;
            }
            lba += n;
            prog.doneBytes = lba * sec;
            prog.mibPerSec = mibPerSec(prog.doneBytes,
                std::chrono::duration<double>(clock_::now() - verifyStart).count());
            if (onProgress) onProgress(prog);
        }
    } else {
        const uint8_t expect = patternByte(last.pattern);
        for (uint64_t lba = 0; lba < totalSectors;) {
            if (cancel.cancelled()) {
                res.cancelled = true;
                res.error = L"Dogrulama iptal edildi.";
                return res;
            }
            const uint64_t n = (std::min)(uint64_t(chunkSectors), totalSectors - lba);
            if (!dev.readSectors(lba, buf.data(), n, err)) {
                res.verifyMismatchSectors += n;
                res.note += L" | LBA " + core::formatU64(lba) + L" okunamadi: " + err;
            } else {
                // Compare per sector: one surviving byte anywhere in a sector makes that
                // sector unverified, and the count is what the report needs to show.
                uint64_t bad = 0;
                for (uint64_t s = 0; s < n; ++s) {
                    const uint8_t* p = buf.data() + size_t(s) * sec;
                    for (uint32_t b = 0; b < sec; ++b) {
                        if (p[b] != expect) { ++bad; break; }
                    }
                }
                if (bad) {
                    res.verifyMismatchSectors += bad;
                    res.note += L" | LBA " + core::formatU64(lba) + L": " +
                                core::formatU64(bad) + L" sektor deseni taşımıyor";
                }
            }
            lba += n;
            prog.doneBytes = lba * sec;
            prog.mibPerSec = mibPerSec(prog.doneBytes,
                std::chrono::duration<double>(clock_::now() - verifyStart).count());
            if (onProgress) onProgress(prog);
        }
    }

    res.stepsDone = prog.totalSteps;
    res.completed = true;
    return res;
}

TableWipeResult wipePartitionTables(core::Device& dev, std::wstring& why) {
    TableWipeResult r;
    const core::DiskGeometry& g = dev.geometry();
    if (!g.valid || g.logicalSectorCount == 0) {
        r.error = L"Disk geometrisi alinamadi.";
        return r;
    }
    const uint32_t sec = g.logicalSectorSize ? g.logicalSectorSize : 512u;
    const uint64_t total = g.logicalSectorCount;

    r.frontSectors = (std::min)(kFrontBytes / sec, total);
    if (r.frontSectors == 0) r.frontSectors = 1;
    r.backSectors = (std::min)(kBackupGptSectors, total);

    core::AlignedBuffer buf(size_t(r.frontSectors) * sec);
    if (!buf) { r.error = L"Sifir tamponu ayiramadi."; return r; }
    buf.zero();

    if (!dev.writeSectors(0, buf.data(), r.frontSectors, why)) {
        r.error = L"LBA 0-1MB bolumu temizlenemedi: " + why;
        return r;
    }

    // The backup GPT lives at the very end. Overlapping ranges on tiny devices are skipped
    // rather than written twice.
    const uint64_t backStart = total - r.backSectors;
    if (backStart >= r.frontSectors) {
        const uint64_t n = (std::min)(uint64_t(r.frontSectors), r.backSectors);
        if (!dev.writeSectors(total - n, buf.data(), n, why)) {
            r.note = L"On taraf temizlendi ama yedek GPT (son " + core::formatU64(n) +
                     L" sektor) yazilamadi: " + why;
            r.completed = true;
            return r;
        }
    }
    r.completed = true;
    return r;
}

bool rereadPartitionTable(core::Device& dev, std::wstring& why) {
    DWORD returned = 0;
    if (dev.sendIoctl(IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, returned, why))
        return true;
    return false;
}

const wchar_t* purgeMethodLabel(PurgeMethod m) {
    switch (m) {
    case PurgeMethod::None:               return L"Purge yok - yalnizca overwriting";
    case PurgeMethod::AtaSecureErase:     return L"ATA SECURITY ERASE UNIT";
    case PurgeMethod::AtaSecureEraseEnh:  return L"ATA SECURITY ERASE UNIT (enhanced)";
    case PurgeMethod::NvmeSanitize:       return L"NVMe SANITIZE (block erase)";
    case PurgeMethod::NvmeFormatCrypto:   return L"NVMe FORMAT NVM SES=2 (crypto erase)";
    case PurgeMethod::NvmeFormatUser:     return L"NVMe FORMAT NVM SES=1 (user data erase)";
    }
    return L"Bilinmeyen yontem";
}

PurgePlan buildPlan(core::Device& dev, const ata::Identify& id) {
    PurgePlan p;
    p.bus = core::diskinfo::queryBusKind(dev.handle());

    if (p.bus == core::BusKind::Nvme) {
        p.best = PurgeMethod::NvmeSanitize;
        p.detail = L"NVMe: SANITIZE komutu tum denetleyiciyi kapsar, parola istemez. "
                   L"Ses tabanli kripto imha icin FORMAT NVM SES=2 secenegi de denenbilir.";
        return p;
    }

    if (!id.valid) {
        p.best = PurgeMethod::None;
        p.detail = L"IDENTIFY DEVICE yanit vermedi (USB köprüsü ATA komutlarını taşımıyor olabilir). "
                   L"Donanım purge yeteneği bilinmiyor; yalnızca sektör ezme kullanılabilir.";
        return p;
    }

    if (!id.securitySupported()) {
        p.best = PurgeMethod::None;
        p.detail = L"Sürücü ATA Security Feature Set desteklemiyor (word 82 bit 1 = 0). "
                   L"SECURITY ERASE UNIT gonderilemez; ezme tek secenek.";
        return p;
    }

    const ata::SecurityState st = ata::securityStateOf(id);
    p.ataSecurityFrozen = st.frozen;
    p.ataSecurityLocked = st.locked;
    p.ataSecurityCountExpired = st.countExpired;
    p.needsPassword = st.enabled;
    p.estimateValid = id.eraseTimeValidNormal();
    p.estimatedMinutesNormal = id.eraseTimeMinutesNormal();
    p.estimatedMinutesEnhanced = id.eraseTimeMinutesEnhanced();

    if (st.frozen) {
        p.best = PurgeMethod::None;
        p.detail = L"ATA Security FREEZE kilidi aktif (word 128 bit 3). SECURITY ERASE UNIT bu "
                   L"POWER STATE icinde reddedilir; surucuyu kapatip acmak gerekir. "
                   L"Donanim purge yerine ezme kullanin ya da makineyi yeniden baslatin.";
        return p;
    }
    if (st.countExpired) {
        p.best = PurgeMethod::None;
        p.detail = L"Guvlik parola deneme sayisi tukendi (count expired). Power cycle olmadan "
                   L"hicbir ATA Security komutu kabul edilmez.";
        return p;
    }

    p.best = st.enhancedEraseSupported ? PurgeMethod::AtaSecureEraseEnh
                                       : PurgeMethod::AtaSecureErase;
    p.detail = L"ATA SECURITY ERASE UNIT gonderilebilir";
    if (st.enhancedEraseSupported) p.detail += L" (enhanced destekleniyor: sakali alanlar da kapsanir)";
    if (p.estimateValid)
        p.detail += L"; surucunun tahmini suresi " + core::formatU64(p.estimatedMinutesNormal) +
                    L" dakika (normal) / " + core::formatU64(p.estimatedMinutesEnhanced) +
                    L" dakika (enhanced)";
    else
        p.detail += L"; surucu süre tahmini raporlamiyor (word 89 bit 15 = 0)";
    if (id.sanitizeSupported())
        p.detail += L" | ACS Sanitize Device kelime 59 bit 12 kurulu ama bu build o komutu "
                    L"gondermiyor: CDB koda bu ortamda dogrulanabildi. SECURITY ERASE UNIT kullaniliyor.";
    if (p.bus == core::BusKind::Usb)
        p.detail += L" | USB/SAT kopru uzerinden SECURITY ERASE UNIT risklidir; kopru komutu "
                    L"zaman asimina ugratabilir. Mumkunse direct SATA portu kullanin.";
    if (p.bus == core::BusKind::Sas || p.bus == core::BusKind::Iscsi)
        p.detail += L" | Gercek SAS hedefi icin SPC SANITIZE/ERASE CDB koda dogrulanamadi; bu "
                    L"yontem yalnizca surucu SAT ATA komutlarini tasiyorsa calisir. Degilse ezme kullanin.";
    return p;
}

CmdResult runPurge(core::Device& dev, PurgeMethod method, const std::string& ataPassword,
                   bool useMaster, uint32_t timeoutSec) {
    const HANDLE h = dev.handle();
    switch (method) {
    case PurgeMethod::AtaSecureErase:
        return ata::securityEraseUnit(h, useMaster, false, ataPassword, timeoutSec);
    case PurgeMethod::AtaSecureEraseEnh:
        return ata::securityEraseUnit(h, useMaster, true, ataPassword, timeoutSec);
    case PurgeMethod::NvmeSanitize:
        return nvme::sanitize(h, nvme::SanitizeAction::BlockErase, timeoutSec);
    case PurgeMethod::NvmeFormatCrypto:
        return nvme::formatNvm(h, 0xFFFFFFFFu, nvme::SecureErase::Cryptographic, timeoutSec);
    case PurgeMethod::NvmeFormatUser:
        return nvme::formatNvm(h, 0xFFFFFFFFu, nvme::SecureErase::UserUserData, timeoutSec);
    case PurgeMethod::None: break;
    }
    CmdResult r;
    r.winError = ERROR_NOT_SUPPORTED;
    r.detail = L"Bu yontem secili degil ya da desteklenmiyor; donanım purge gönderilmedi.";
    return r;
}

}  // namespace erase
}  // namespace hw
