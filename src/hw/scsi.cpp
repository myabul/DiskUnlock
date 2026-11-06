#include "scsi.h"

namespace hw {

#pragma pack(push, 8)
struct ScsiPtDirectWithSense {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG filler;
    UCHAR sense[64];
};
#pragma pack(pop)

static const uint8_t kSenseAtaReturnDescriptor = 0x09;

// Walks the embedded sense descriptors and picks up the SAT ATA Return Descriptor, which is
// the only way to see the taskfile registers (ATA ERR/STATUS) after CK_COND requests.
static void scanDescriptors(const UCHAR* s, size_t pos, size_t end, SenseInfo& out) {
    while (pos + 1 < end) {
        uint8_t type = s[pos];
        uint8_t dlen = s[pos + 1];
        if (dlen == 0) break;
        if (pos + 2 + dlen > end) break;
        if (type == kSenseAtaReturnDescriptor && dlen >= 0x0C) {
            out.ataReturnPresent = true;
            out.ataError       = s[pos + 3];
            out.ataSectorCount = s[pos + 5];
            out.ataLbaLow      = s[pos + 7];
            out.ataLbaMid      = s[pos + 9];
            out.ataLbaHigh     = s[pos + 11];
            out.ataDevice      = s[pos + 12];
            out.ataStatus      = s[pos + 13];
            return;
        }
        pos += 2 + dlen;
    }
}

static void parseSense(const UCHAR* s, size_t len, SenseInfo& out) {
    if (len < 2) return;
    uint8_t response = s[0] & 0x7F;
    if (response == 0x72 || response == 0x73) {
        out.valid = true;
        out.senseKey = s[1] & 0x0F;
        scanDescriptors(s, 2, len, out);
        return;
    }
    if (response == 0x70 || response == 0x71) {
        if (len < 8) return;
        out.valid = true;
        out.senseKey = s[2] & 0x0F;
        size_t extra = len >= 14 ? 8 + s[7] : 8;
        if (len >= 14) {
            out.asc = s[12];
            out.ascq = s[13];
        }
        scanDescriptors(s, 8, (std::min)(extra, len), out);
        // SAT-2 and later may echo the taskfile in *fixed* format sense instead of a
        // descriptor: ASC=00h, ASCQ=1Dh. Drivers that do this are common on Windows/SAT.
        if (!out.ataReturnPresent && out.asc == 0x00 && out.ascq == 0x1D && len >= 12) {
            out.ataReturnPresent = true;
            out.ataError       = s[3];
            out.ataStatus      = s[4];
            out.ataDevice      = s[5];
            out.ataSectorCount = s[6];
            out.ataLbaLow      = s[9];
            out.ataLbaMid      = s[10];
            out.ataLbaHigh     = s[11];
        }
        return;
    }
}

FaultKind classify(const CmdResult& r) {
    if (!r.ioctlOk) {
        switch (r.winError) {
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_BAD_COMMAND:
            return FaultKind::AccessDenied;
        case ERROR_TIMEOUT:
        case ERROR_SEM_TIMEOUT:
            return FaultKind::Timeout;
        default:
            return FaultKind::Other;
        }
    }
    if (!r.sense.valid) {
        if (r.scsiStatus == 0x00) return FaultKind::None;
        return FaultKind::NoSense;
    }
    switch (r.sense.senseKey) {
    case 0x0: return FaultKind::None;
    case 0x2: return FaultKind::NotReady;
    case 0x3: return FaultKind::MediaError;
    case 0x4: return FaultKind::HardwareError;
    case 0x5: return FaultKind::InvalidCommand;
    case 0x6: return FaultKind::UnitAttention;
    case 0x7: return FaultKind::DataProtected;
    case 0xB: return FaultKind::Aborted;
    default:  return FaultKind::Other;
    }
}

std::wstring faultLabel(FaultKind k) {
    switch (k) {
    case FaultKind::None:           return L"Temiz";
    case FaultKind::MediaError:     return L"MEDYA HATASI (fiziksel bozuk sektor)";
    case FaultKind::HardwareError:  return L"Donanim hatasi";
    case FaultKind::NotReady:       return L"Suru hazir degil (spin-up / medya yok)";
    case FaultKind::UnitAttention:  return L"Birim degisti veya resetlendi";
    case FaultKind::InvalidCommand: return L"Komut reddedildi (opcode/alan destklenmiyor)";
    case FaultKind::DataProtected:  return L"Veri korumasi (kilitle korunan alan)";
    case FaultKind::Aborted:        return L"Istemci veya hedef komutu iptal etti";
    case FaultKind::AccessDenied:   return L"Erisim reddedildi (Windows bolumu kullanmda)";
    case FaultKind::Timeout:        return L"Zaman asimi";
    case FaultKind::NoSense:        return L"Sense verisi dondurulmedi";
    case FaultKind::Other:          return L"Diger hata";
    }
    return L"Diger";
}

std::wstring describeResult(const CmdResult& r) {
    std::wstring s = faultLabel(classify(r));
    wchar_t buf[128];
    if (r.sense.valid) {
        swprintf(buf, 128, L" | SK=%u ASC=%02X/%02X", (unsigned)r.sense.senseKey,
                 (unsigned)r.sense.asc, (unsigned)r.sense.ascq);
        s += buf;
    }
    if (r.sense.ataReturnPresent) {
        swprintf(buf, 128, L" | ATA ERR=%02X STATUS=%02X", (unsigned)r.sense.ataError,
                 (unsigned)r.sense.ataStatus);
        s += buf;
    }
    if (!r.ioctlOk && r.winError) {
        swprintf(buf, 128, L" | winerror=%lu", (unsigned long)r.winError);
        s += buf;
        s += L" (" + core::winErrorText(r.winError) + L")";
    }
    if (!r.detail.empty()) s += L" | " + r.detail;
    return s;
}

CmdResult scsiCmd(HANDLE hDevice, const uint8_t* cdb, uint8_t cdbLen, Direction dir,
                  void* buffer, uint32_t bufferLen, uint32_t timeoutSec) {
    CmdResult r;
    if (hDevice == INVALID_HANDLE_VALUE || !cdb || cdbLen == 0 || cdbLen > 16) {
        r.winError = ERROR_INVALID_PARAMETER;
        r.detail = L"gecersiz CDB veya handle";
        return r;
    }
    if (dir != Direction::NoData && (!buffer || bufferLen == 0)) {
        r.winError = ERROR_INVALID_PARAMETER;
        r.detail = L"yon bildirildi ama tampon yok";
        return r;
    }
    if (dir == Direction::NoData) bufferLen = 0;

    ScsiPtDirectWithSense pkt{};
    pkt.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    pkt.sptd.CdbLength = cdbLen;
    pkt.sptd.SenseInfoLength = (UCHAR)sizeof(pkt.sense);
    pkt.sptd.SenseInfoOffset = offsetof(ScsiPtDirectWithSense, sense);
    pkt.sptd.DataIn = (dir == Direction::DataIn)   ? SCSI_IOCTL_DATA_IN
                      : (dir == Direction::DataOut) ? SCSI_IOCTL_DATA_OUT
                                                    : SCSI_IOCTL_DATA_UNSPECIFIED;
    pkt.sptd.DataTransferLength = bufferLen;
    pkt.sptd.DataBuffer = buffer;
    pkt.sptd.TimeOutValue = timeoutSec;
    memcpy(pkt.sptd.Cdb, cdb, cdbLen);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_SCSI_PASS_THROUGH_DIRECT, &pkt, sizeof(pkt), &pkt,
                              sizeof(pkt), &returned, nullptr);
    r.ioctlOk = ok == TRUE;
    r.winError = ok ? ERROR_SUCCESS : GetLastError();
    r.scsiStatus = pkt.sptd.ScsiStatus;
    parseSense(pkt.sense, sizeof(pkt.sense), r.sense);
    if (!r.ioctlOk) return r;

    // A CK_COND request completes with CHECK CONDITION *on purpose* so the taskfile can be
    // read back; the ATA STATUS bits are the authoritative result in that case.
    if (r.sense.ataReturnPresent) {
        uint8_t st = r.sense.ataStatus;
        r.deviceOk = (st & 0x80) == 0 && (st & 0x40) != 0 && (st & 0x01) == 0;  // !BSY && DRDY && !ERR
        // A bogus sense key alongside a clean ATA status is a known SAT quirk; trust the drive.
        if (r.deviceOk && !r.ioctlOk) {
            r.ioctlOk = true;
            r.winError = ERROR_SUCCESS;
        }
    } else {
        r.deviceOk = r.ioctlOk && (r.scsiStatus == 0x00 || r.sense.senseKey == 0x1);
    }
    return r;
}

}  // namespace hw
