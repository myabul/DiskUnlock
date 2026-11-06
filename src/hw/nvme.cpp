#include "nvme.h"
#include "../core/str.h"

namespace hw {
namespace nvme {

namespace {

constexpr uint32_t kHealthLogBytes = 512;
// NVMe 1.4+ defines the Sanitize Status page as 32 bytes (SPROG, SSTAT, SCDW10, ...). Asking a
// controller for 20 of them means a short NUMD for no extra information, so read the whole page.
constexpr uint32_t kSanitizeLogBytes = 32;
// The self-test log is 564 bytes on paper; the read has to be dword-rounded for NUMD.
constexpr uint32_t kSelfTestLogBytes = 576;
constexpr uint32_t kErrorInfoEntryBytes = 384;
constexpr uint32_t kErrorInfoEntries = 4;

// The NVMe 16-byte counters are little-endian; anything above 64 bits is reported as an
// overflow rather than silently truncated to a wrong number.
uint64_t le64(const uint8_t* b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

void putLe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

uint32_t getLe32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// One contiguous allocation: header + 64-byte SQE + optional data buffer, because
// IOCTL_STORAGE_PROTOCOL_COMMAND takes offsets relative to the start of the struct.
struct CommandPack {
    STORAGE_PROTOCOL_COMMAND cmd;
    UCHAR nvmeCmd[kNvmeCmdSize];
    UCHAR data[2048];
};

constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

CmdResult runAdminCommand(HANDLE hDevice, const uint8_t sqe[kNvmeCmdSize], Direction dir,
                          void* data, uint32_t dataLen, uint32_t timeoutSec) {
    CmdResult r;
    CommandPack pack{};
    const uint32_t dataOffset = alignUp(uint32_t(sizeof(STORAGE_PROTOCOL_COMMAND) + kNvmeCmdSize), 8);

    pack.cmd.Version = STORAGE_PROTOCOL_STRUCTURE_VERSION;
    pack.cmd.Length = sizeof(STORAGE_PROTOCOL_COMMAND);
    pack.cmd.ProtocolType = ProtocolTypeNvme;
    // stornvme rejects a raw admin command that is not marked as an adapter request.
    pack.cmd.Flags = STORAGE_PROTOCOL_COMMAND_FLAG_ADAPTER_REQUEST;
    // The 64-byte submission entry is only interpretable if the driver is told which NVMe
    // command format it carries; leaving CommandSpecific at 0 makes stornvme answer
    // ERROR_INVALID_PARAMETER before the disk ever sees the opcode.
    pack.cmd.CommandSpecific = STORAGE_PROTOCOL_SPECIFIC_NVME_ADMIN_COMMAND;
    pack.cmd.CommandLength = kNvmeCmdSize;
    pack.cmd.TimeOutValue = timeoutSec;
    memcpy(pack.nvmeCmd, sqe, kNvmeCmdSize);

    uint8_t* base = reinterpret_cast<uint8_t*>(&pack);
    if (dir == Direction::DataIn && data && dataLen) {
        if (dataLen > sizeof(pack.data)) {
            r.detail = L"tampon, gonderilecek veri alanindan buyuk";
            return r;
        }
        pack.cmd.DataFromDeviceBufferOffset = dataOffset;
        pack.cmd.DataFromDeviceTransferLength = dataLen;
    } else if (dir == Direction::DataOut && data && dataLen) {
        if (dataLen > sizeof(pack.data)) {
            r.detail = L"tampon, gonderilecek veri alanindan buyuk";
            return r;
        }
        memcpy(base + dataOffset, data, dataLen);
        pack.cmd.DataToDeviceBufferOffset = dataOffset;
        pack.cmd.DataToDeviceTransferLength = dataLen;
    }

    const uint32_t inSize =
        sizeof(STORAGE_PROTOCOL_COMMAND) + kNvmeCmdSize + (dataLen ? alignUp(dataLen, 8) : 0);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_STORAGE_PROTOCOL_COMMAND, &pack, inSize, &pack,
                              sizeof(pack), &returned, nullptr);
    r.ioctlOk = ok == TRUE;
    r.winError = ok ? ERROR_SUCCESS : GetLastError();
    if (!r.ioctlOk) {
        r.detail = L"IOCTL_STORAGE_PROTOCOL_COMMAND: " + core::winErrorText(r.winError);
        return r;
    }

    switch (pack.cmd.ReturnStatus) {
    case STORAGE_PROTOCOL_STATUS_SUCCESS:
        r.deviceOk = true;
        break;
    case STORAGE_PROTOCOL_STATUS_NOT_SUPPORTED:
        r.detail = L"Surucu bu NVMe admin komutunu desteklemiyor (STATUS_NOT_SUPPORTED).";
        break;
    case STORAGE_PROTOCOL_STATUS_INVALID_REQUEST:
        r.detail = L"Komut gecersiz bulundu (STATUS_INVALID_REQUEST).";
        break;
    default: {
        wchar_t buf[96];
        swprintf(buf, 96, L"ReturnStatus=%lu ErrorCode=0x%lX", (unsigned long)pack.cmd.ReturnStatus,
                 (unsigned long)pack.cmd.ErrorCode);
        r.detail = buf;
        break;
    }
    }

    if (dir == Direction::DataIn && data && dataLen && r.deviceOk)
        memcpy(data, base + dataOffset, dataLen);
    return r;
}

// Get Log Page through IOCTL_STORAGE_QUERY_PROPERTY; this is the read-only path that works
// without the adapter-request command interface.
//
// The input is a STORAGE_PROPERTY_QUERY whose "AdditionalParameters" hold a bare
// STORAGE_PROTOCOL_SPECIFIC_DATA - the STORAGE_PROTOCOL_DATA_DESCRIPTOR header exists only on
// the reply. Putting it on the way in makes the driver read ProtocolType out of the descriptor's
// Version field, and the request dies as ERROR_INVALID_PARAMETER before reaching the controller.
// Both shapes are 8 bytes deep before the same SPECIFIC_DATA, and the IOCTL is METHOD_BUFFERED,
// so one buffer serves as request and reply.
bool queryLogPage(HANDLE hDevice, uint8_t logPageId, uint8_t* out, uint32_t outLen,
                  std::wstring& why) {
    struct QueryPack {
        STORAGE_PROPERTY_QUERY query;
        STORAGE_PROTOCOL_SPECIFIC_DATA psd;
        ULONG data[400];  // 1600 bytes: fits the 512-byte health log and the 1536-byte error log
    };
    QueryPack pack{};
    pack.query.PropertyId = StorageDeviceProtocolSpecificProperty;
    pack.query.QueryType = PropertyStandardQuery;

    STORAGE_PROTOCOL_SPECIFIC_DATA& psd = pack.psd;
    psd.ProtocolType = ProtocolTypeNvme;
    psd.DataType = NVMeDataTypeLogPage;
    psd.ProtocolDataRequestValue = logPageId;
    // Sub-value is the log-specific field (LSI), not a dword count: the driver takes NUMD from
    // ProtocolDataLength. Health, self-test and sanitize pages all define LSI as 0.
    psd.ProtocolDataRequestSubValue = 0;
    psd.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    psd.ProtocolDataLength = outLen;

    DWORD returned = 0;
    // Same buffer in and out (the IOCTL is METHOD_BUFFERED), and the full span goes in as well:
    // a driver that checks whether ProtocolDataLength fits inside the request gets the same
    // answer it would get from the reply.
    BOOL ok = DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &pack, sizeof(pack), &pack,
                              sizeof(pack), &returned, nullptr);
    if (!ok) {
        why = L"IOCTL_STORAGE_QUERY_PROPERTY(log): " + core::winErrorText(GetLastError());
        return false;
    }
    const ULONG dataBase = FIELD_OFFSET(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    const uint32_t offset = dataBase + psd.ProtocolDataOffset;
    if (psd.ProtocolDataLength < outLen || offset + outLen > returned) {
        why = L"Log sayfasi beklenenden kucuk dondu (ProtocolDataLength=" +
              core::formatU64(psd.ProtocolDataLength) + L", donen=" + core::formatU64(returned) + L").";
        return false;
    }
    memcpy(out, reinterpret_cast<uint8_t*>(&pack) + offset, outLen);
    return true;
}

}  // namespace

U128 U128::decode(const uint8_t* b) {
    U128 v;
    v.value = le64(b);
    for (int i = 8; i < 16; ++i)
        if (b[i]) v.overflow = true;
    return v;
}

std::wstring HealthInfo::criticalWarningText() const {
    if (!valid) return L"bilinmiyor";
    if (criticalWarning == 0) return L"uyari yok";
    std::wstring s;
    if (criticalWarning & 0x01) s += L"bakim gerektiriyor | ";
    if (criticalWarning & 0x02) s += L"asağı geçici sıcaklık eşiği | ";
    if (criticalWarning & 0x04) s += L"NUMD yedek azaldı | ";
    if (criticalWarning & 0x08) s += L"doğrulanmamış verisi var (kayıp riski) | ";
    if (criticalWarning & 0x10) s += L"kontrolcü sadece-okunur modda | ";
    if (criticalWarning & 0x20) s += L"zamanlama kaybı (backup yok) | ";
    if (s.empty()) s += L"tanımsız bit=" + core::formatHex(criticalWarning);
    return s;
}

std::wstring SanitizeStatus::describe() const {
    if (!valid) return L"sanitize durumu okunamadi";
    const uint32_t sanstat = globalDataEraseStatus & 0x7u;
    const wchar_t* state = L"bilinmeyen";
    switch (sanstat) {
    case 0: state = L"sanitize komutu gormedi"; break;
    case 1: state = L"DEVAM EDIYOR"; break;
    case 2: state = L"basariyla tamamlandi"; break;
    case 3: state = L"tamamlanma durumu belirsiz"; break;
    case 4: state = L"BABASARISIZ (islem durdu)"; break;
    case 5: state = L"basariyla tamamlandi, tahsis edilmis alan dolu"; break;
    default: break;
    }
    std::wstring s = state;
    if (inProgress && progressPercent != 0xFF)
        s += L" | kalan %" + core::formatU64(progressPercent);
    return s;
}

bool readHealth(HANDLE hDevice, HealthInfo& out) {
    out = HealthInfo{};
    uint8_t log[kHealthLogBytes] = {0};
    std::wstring why;
    if (!queryLogPage(hDevice, uint8_t(kNvmeLogHealthInfo), log, sizeof(log), why)) {
        out.note = why;
        return false;
    }
    NvmeHealthInfoLog& h = *reinterpret_cast<NvmeHealthInfoLog*>(log);
    out.valid = true;
    out.criticalWarning = h.criticalWarning;
    const uint32_t kelvin = uint32_t(h.compositeTemp[0]) | (uint32_t(h.compositeTemp[1]) << 8);
    out.compositeTempC = kelvin ? int32_t(kelvin) - 273 : -1;
    out.availableSpare = h.availableSpare;
    out.availableSpareThreshold = h.availableSpareThreshold;
    out.percentageUsed = h.percentageUsed;
    out.dataUnitsRead = U128::decode(h.dataUnitsRead);
    out.dataUnitsWritten = U128::decode(h.dataUnitsWritten);
    out.hostReadCommands = U128::decode(h.hostReadCommands);
    out.hostWriteCommands = U128::decode(h.hostWriteCommands);
    out.controllerBusyMinutes = U128::decode(h.controllerBusyTime);
    out.powerCycles = U128::decode(h.powerCycles);
    out.powerOnHours = U128::decode(h.powerOnHours);
    out.unsafeShutdowns = U128::decode(h.unsafeShutdowns);
    out.mediaErrors = U128::decode(h.mediaErrors);
    out.errorLogEntries = U128::decode(h.errorInfoLogEntries);
    return true;
}

bool readSanitizeStatus(HANDLE hDevice, SanitizeStatus& out) {
    out = SanitizeStatus{};
    uint8_t log[kSanitizeLogBytes] = {0};
    std::wstring why;
    if (!queryLogPage(hDevice, uint8_t(kNvmeLogSanitizeStatus), log, sizeof(log), why)) {
        out.note = why;
        return false;
    }
    // NVMe Sanitize Status Log: bytes 0-2 SPROG (percent remaining), 4-7 SSTAT.
    out.valid = true;
    const uint32_t sprog = uint32_t(log[0]) | (uint32_t(log[1]) << 8) | (uint32_t(log[2]) << 16);
    out.progressPercent = sprog > 100 ? 0xFF : uint8_t(sprog);
    out.globalDataEraseStatus = getLe32(log + 4);
    out.inProgress = (out.globalDataEraseStatus & 0x7u) == 1;
    return true;
}

CmdResult rawAdminCommand(HANDLE hDevice, const uint8_t sqe[kNvmeCmdSize], Direction dir,
                          void* data, uint32_t dataLen, uint32_t timeoutSec) {
    return runAdminCommand(hDevice, sqe, dir, data, dataLen, timeoutSec);
}

CmdResult getLogPage(HANDLE hDevice, uint8_t logPageId, void* buffer, uint32_t bufferLen,
                     uint32_t timeoutSec) {
    uint8_t sqe[kNvmeCmdSize] = {0};
    sqe[0] = opGetLogPage;
    // CDW10: LOG ID (7:0) |NumberOfDwords... | LSI (31:28)? NVMe places LOG ID in CDW10[7:0]
    // and NUMD in CDW10[27:16] (base 0) plus CDW11[15:0] for larger logs.
    const uint32_t numd = bufferLen / 4u;
    putLe32(sqe + 32, uint32_t(logPageId) | ((numd - 1u) << 16));
    return runAdminCommand(hDevice, sqe, Direction::DataIn, buffer, bufferLen, timeoutSec);
}

CmdResult formatNvm(HANDLE hDevice, uint32_t nsid, SecureErase ses, uint32_t timeoutSec) {
    uint8_t sqe[kNvmeCmdSize] = {0};
    sqe[0] = opFormatNvm;
    putLe32(sqe + 4, nsid);
    // CDW10 bits 11:10 = SES; bits 2:0 (LBAF), 5:3 (MSET), 9:7 (PI) stay 0 so the drive keeps
    // its current format - a wrong LBAF silently reformats the namespace to a different size.
    putLe32(sqe + 32, (uint32_t(ses) & 0x3u) << 10);
    return runAdminCommand(hDevice, sqe, Direction::NoData, nullptr, 0, timeoutSec);
}

CmdResult sanitize(HANDLE hDevice, SanitizeAction action, uint32_t timeoutSec) {
    uint8_t sqe[kNvmeCmdSize] = {0};
    sqe[0] = opSanitize;
    putLe32(sqe + 4, 0xFFFFFFFFu);  // SANITIZE is controller-wide; NSID must be all or nothing
    putLe32(sqe + 32, uint32_t(action) & 0x7u);
    return runAdminCommand(hDevice, sqe, Direction::NoData, nullptr, 0, timeoutSec);
}

std::wstring SelfTestResult::opText() const {
    switch (opCode) {
    case 0x1: return L"Kisa self-test";
    case 0x2: return L"Uzun self-test";
    case 0xE: return L"Ureticiye ozel test";
    default: break;
    }
    return L"Kod " + core::formatU64(opCode) + L" (rezerve/tanimsiz)";
}

std::wstring SelfTestResult::resultText() const {
    switch (resultCode) {
    case 0x0: return L"hatasiz tamamlandi";
    case 0x1: return L"Self-test komutuyla iptal edildi";
    case 0x2: return L"Kontrolcu resetiyle iptal edildi";
    case 0x3: return L"Namespace kaldirildigi icin iptal edildi";
    case 0x4: return L"Format NVM komutuyla iptal edildi";
    case 0x5: return L"Fatal veya bilinmeyen test hatasi";
    case 0x6: return L"Tamamlandi: basarisiz segment bilinmiyor";
    case 0x7: return L"Tamamlandi: basarisiz segment(ler) var";
    case 0x8: return L"Bilinmeyen nedenle iptal edildi";
    case 0x9: return L"Sanitize islemiyle iptal edildi";
    case 0xF: return L"kayit bos (kullanilmiyor)";
    default: break;
    }
    return L"Sonuc kodu " + core::formatU64(resultCode) + L" (rezerve)";
}

std::wstring SelfTestLog::statusText() const {
    if (!valid) return note.empty() ? L"self-test logu okunamadi" : note;
    switch (currentOperation & 0x0F) {
    case 0x0: return L"devam eden self-test yok";
    case 0x1: return L"KISA self-test suruyor, % tamamlanan: " +
                    core::formatU64(currentCompletion & 0x7F);
    case 0x2: return L"UZUN self-test suruyor, % tamamlanan: " +
                    core::formatU64(currentCompletion & 0x7F);
    case 0xE: return L"uretmiciye ozel test suruyor, % tamamlanan: " +
                    core::formatU64(currentCompletion & 0x7F);
    default: break;
    }
    return L"bilinmeyen aktif test kodu " + core::formatU64(currentOperation & 0x0F);
}

std::wstring ErrorInfoEntry::describe() const {
    if (!valid) return L"bos kayit";
    std::wstring s = L"hata#" + core::formatU64(errorCount) + L" opcode=" +
                     core::formatHex(opcode) + L" SQ=" + core::formatU64(sqId) + L"/CMD=" +
                     core::formatU64(cmdId) + L" status=" + core::formatHex(statusField);
    if (nsid) s += L" nsid=" + core::formatU64(nsid);
    if (lba) s += L" lba=" + core::formatU64(lba);
    return s;
}

bool readSelfTestLog(HANDLE hDevice, SelfTestLog& out) {
    out = SelfTestLog{};
    uint8_t log[kSelfTestLogBytes] = {0};
    std::wstring why;
    if (!queryLogPage(hDevice, uint8_t(kNvmeLogSelfTest), log, sizeof(log), why)) {
        out.note = why;
        return false;
    }
    out.valid = true;
    out.currentOperation = log[0];
    out.currentCompletion = log[1];
    for (uint32_t i = 0; i < 20; ++i) {
        const uint8_t* p = log + 4 + i * 28;   // Figure 213: 20 results of 28 bytes
        SelfTestResult r;
        r.opCode = uint8_t(p[0] >> 4);
        r.resultCode = uint8_t(p[0] & 0x0F);
        if (r.opCode == 0 || r.resultCode == 0xF) continue;  // unused slot
        r.segment = p[1];
        r.validBits = p[2];
        r.powerOnHours = le64(p + 4);
        r.nsid = getLe32(p + 12);
        r.firstErrorLba = le64(p + 16);
        r.statusCodeType = p[24];
        r.statusCode = p[25];
        r.valid = true;
        out.results.push_back(r);
    }
    return true;
}

bool readErrorInfoLog(HANDLE hDevice, std::vector<ErrorInfoEntry>& entries, std::wstring& why) {
    entries.clear();
    uint8_t log[kErrorInfoEntryBytes * kErrorInfoEntries] = {0};
    if (!queryLogPage(hDevice, uint8_t(kNvmeLogErrorInfo), log, sizeof(log), why)) return false;
    for (uint32_t i = 0; i < kErrorInfoEntries; ++i) {
        const uint8_t* p = log + i * kErrorInfoEntryBytes;
        ErrorInfoEntry e;
        e.errorCount = le64(p);
        if (e.errorCount == 0) continue;  // an untouched slot says nothing
        e.sqId = uint16_t(p[8]) | (uint16_t(p[9]) << 8);
        e.cmdId = uint16_t(p[10]) | (uint16_t(p[11]) << 8);
        e.statusField = uint16_t(p[12]) | (uint16_t(p[13]) << 8);
        e.opcode = p[14];
        e.nsid = getLe32(p + 24);
        e.lba = le64(p + 32);
        e.valid = true;
        entries.push_back(e);
    }
    return true;
}

CmdResult deviceSelfTest(HANDLE hDevice, uint8_t selfTestCode, uint32_t nsid, std::wstring& why,
                         uint32_t timeoutSec) {
    // Only the two codes the base spec defines are forwarded; vendor codes (0Eh) and reserved
    // values would either do nothing an operator can reason about or be rejected outright.
    if (selfTestCode != 0x01 && selfTestCode != 0x02) {
        why = L"NVMe Device Self-test yalnizca 1 (kisa) veya 2 (uzun) koduyla gonderilir; "
              L"istenen kod " + core::formatHex(selfTestCode) + L".";
        return CmdResult{};
    }
    uint8_t sqe[kNvmeCmdSize] = {0};
    sqe[0] = opDeviceSelfTest;
    putLe32(sqe + 4, nsid);
    putLe32(sqe + 32, selfTestCode);  // CDW10 bits 3:0 = STC
    why.clear();
    return runAdminCommand(hDevice, sqe, Direction::NoData, nullptr, 0, timeoutSec);
}

}  // namespace nvme
}  // namespace hw
