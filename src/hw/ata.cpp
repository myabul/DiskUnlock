#include "ata.h"
#include "../core/str.h"

namespace hw {
namespace ata {

namespace {

constexpr uint8_t kAtaPassThrough16 = 0x85;

// Mirrors smartmontools' sat_device::ata_pass_through() byte-for-byte; the register slots
// are not interchangeable and a Feature placed in the wrong byte is silently accepted
// (and ignored) by the bridge.
void buildCdb(uint8_t* cdb, const Taskfile& tf, Direction dir, bool ckCond) {
    int protocol = 3;  // 3 = non-data, 4 = PIO data-in, 5 = PIO data-out
    int tDir = 1;      // 1 = from device, 0 = to device
    int tLength = 0;   // 2 = sector_count holds the block count
    if (dir == Direction::DataIn) {
        protocol = 4;
        tLength = 2;
    } else if (dir == Direction::DataOut) {
        protocol = 5;
        tLength = 2;
        tDir = 0;
    }

    cdb[0] = kAtaPassThrough16;
    cdb[1] = uint8_t((protocol << 1) | (tf.lba48 ? 1 : 0));  // multiple_count=0, protocol, EXTEND
    cdb[2] = uint8_t((ckCond ? 1 : 0) << 5 | (tDir << 3) | (1 << 2) | tLength);  // TTYPE=1 (512 blocks)
    cdb[3] = tf.featuresHi;
    cdb[4] = tf.features;
    cdb[5] = tf.countHi;
    cdb[6] = tf.count;
    cdb[7] = tf.lbaLowHi;
    cdb[8] = tf.lbaLow;
    cdb[9] = tf.lbaMidHi;
    cdb[10] = tf.lbaMid;
    cdb[11] = tf.lbaHighHi;
    cdb[12] = tf.lbaHigh;
    cdb[13] = uint8_t(tf.device | 0x80);  // SAT: bit 7 selects the ATA device
    cdb[14] = tf.command;
    cdb[15] = 0;
}

uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

uint64_t le48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 5; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// SMART READ DATA ends with a byte whose value makes the sum of all 512 bytes zero.
bool checksumOk(const uint8_t* log) {
    uint8_t sum = 0;
    for (int i = 0; i < 512; ++i) sum = uint8_t(sum + log[i]);
    return sum == 0;
}

}  // namespace

std::wstring SecurityState::describe() const {
    if (!valid) return L"gucelik bilgisi okunamadi";
    std::wstring s;
    if (!supported) return L"ATA Security desteklenmiyor";
    s += enabled ? L"Etkin" : L"Etkin degil";
    if (locked) s += L" | KILITLI";
    if (frozen) s += L" | DONDURULMU";
    if (countExpired) s += L" | Giris hakki tukendi (guc dongusu sart)";
    if (enhancedEraseSupported) s += L" | Enhanced erase var";
    s += masterIsMaximum ? L" | Master=Maximum" : L" | Master=High";
    return s;
}

std::wstring SmartAttribute::label() const {
    switch (id) {
    case 1:   return L"Read Error Rate";
    case 2:   return L"Throughput Performance";
    case 3:   return L"Spin-Up Time";
    case 4:   return L"Start/Stop Count";
    case 5:   return L"Reallocated Sectors Count";
    case 6:   return L"Read Channel Margin";
    case 7:   return L"Seek Error Rate";
    case 8:   return L"Seek Time Performance";
    case 9:   return L"Power-On Hours";
    case 10:  return L"Spin Retry Count";
    case 11:  return L"Recalibration Retries";
    case 12:  return L"Power Cycle Count";
    case 13:  return L"Soft Read Error Rate";
    case 67:  return L"Write Error Rate (SSD)";
    case 70:  return L"Garbage Collection Count";
    case 71:  return L"Program Fail Count";
    case 72:  return L"Erase Fail Count";
    case 73:  return L"Erase Fail Count";
    case 74:  return L"Head Stability";
    case 75:  return L"Induced Op-Vibration Detection";
    case 76:  return L"Load Grow Rate";
    case 77:  return L"Head Flyability (Flying Height)";
    case 78:  return L"Head Restraint Voltage";
    case 79:  return L"Air Flow Volume";
    case 80:  return L"X-Write Alignment";
    case 84:  return L"End-to-End Error";
    case 85:  return L"Total Pat Write Resequence Times";
    case 86:  return L"Read Error Retry Rate";
    case 87:  return L"Floating Event";
    case 88:  return L"Vibrations During Off-Track Failures";
    case 89:  return L"Suspend Retry Count";
    case 90:  return L"Spin Attempts";
    case 91:  return L"Program Fail Count";
    case 92:  return L"Sensor Against Magnetic Flux";
    case 93:  return L"Hardware ECC Recovered";
    case 94:  return L"Temperature";
    case 95:  return L"G-sense Error Rate";
    case 96:  return L"Unexpected Power Loss";
    case 97:  return L"Program Fail Count";
    case 98:  return L"Guard Band Origin Sector";
    case 99:  return L"Idle Time";
    case 100: return L"Idle Time Overpower";
    case 101: return L"Load Cycle Count";
    case 102: return L"Parallel ECC Sector Recovery";
    case 103: return L"No Talk / Head Frequency Cycles";
    case 104: return L"Device Wear Leveling Count";
    case 105: return L"Total Count of Sectors Rewritten";
    case 106: return L"Metric of a Percent of Lifetime Used Range";
    case 108: return L"Port Multiplier Zone";
    case 112: return L"Buffer Integrity";
    case 113: return L"System (Flash) Memory Allowance";
    case 117: return L"System (Flash) Memory Protection";
    case 120: return L"Hardware ECC Recovered";
    case 122: return L"Read Error Rate";
    case 124: return L"Read Scan Error Rate";
    case 125: return L"Good Sectors / Flawed Sectors";
    case 126: return L"Load Cycle Count";
    case 127: return L"Short Offline Uncorrectable";
    case 128: return L"Command Timeout";
    case 129: return L"Read Error Rate";
    case 130: return L"Write Error Rate";
    case 131: return L"Restore Error Count";
    case 132: return L"Save Error Count";
    case 133: return L"Control Find Error Rate";
    case 134: return L"Load-in Time";
    case 135: return L"Unrecoverable Count";
    case 136: return L"Read Sector Rate";
    case 137: return L"Lifetime (from Ashbe)";
    case 139: return L"PAN";
    case 140: return L"Zoned Read Error Rate";
    case 141: return L"Disk Off-Track Failure Rate";
    case 142: return L"Vibration During Read";
    case 143: return L"Vibration During Write";
    case 144: return L"Logical Fly Height";
    case 145: return L"Firmware ECC Recovery";
    case 147: return L"Reallocation Event Count";
    case 148: return L"Store-in Plane Operation";
    case 149: return L"ATA Error Count";
    case 151: return L"Device Failure Count";
    case 152: return L"Critical Timeout Count";
    case 153: return L"Initial Error Rate";
    case 154: return L"Data Read Error";
    case 157: return L"Seek Error Rate";
    case 158: return L"Drive Read Error Rate";
    case 159: return L"Spin Retry Count";
    case 160: return L"Corrected Disk Read Errors";
    case 161: return L"Block Reassign Count";
    case 162: return L"Data Error Rate / Hardware ECC from Cns";
    case 163: return L"Position Error Rate";
    case 164: return L"Read Error Rate";
    case 165: return L"Write Error Rate";
    case 166: return L"Reallocated Event Count";
    case 167: return L"Write Sectors Allocation Count";
    case 168: return L"Max Wear Leveling Count";
    case 169: return L"Time Manufacturing";
    case 170: return L"Reserved Block Defect Count";
    case 171: return L"Program Fail Count";
    case 172: return L"Erase Fail Count";
    case 173: return L"Wear Levelling Fail Count";
    case 174: return L"Unexpected Power Loss Count";
    case 175: return L"Failed Power Recovery Induction";
    case 176: return L"Firmware ECC Corrections";
    case 177: return L"Wear Range Delta";
    case 178: return L"Used Reserved Block Count";
    case 179: return L"Used Reserved Block Count Total";
    case 180: return L"Program Fail Count";
    case 181: return L"Erase Fail Count";
    case 182: return L"Unused Reserved Block Count";
    case 183: return L"Runtime Bad Block";
    case 184: return L"End-to-End Error";
    case 185: return L"Head Thermal Assemblies";
    case 186: return L"Induced Op-Vibration";
    case 187: return L"Reported Uncorrectable Errors";
    case 188: return L"Command Timeout";
    case 189: return L"High Fly Writes";
    case 190: return L"Airflow Temperature";
    case 191: return L"G-sense Error Rate";
    case 192: return L"Power-Off Retract Count";
    case 193: return L"Load Cycle Count";
    case 194: return L"Temperature";
    case 195: return L"ECC Surge On";
    case 196: return L"Reallocation Event Count";
    case 197: return L"Current Pending Sector Count";
    case 198: return L"Uncorrectable Sector Count";
    case 199: return L"UDMA CRC Error Rate";
    case 200: return L"Multi-Zone Error Rate";
    case 201: return L"Soft Read Error Rate";
    case 202: return L"Disk Shock (in Operation)";
    case 203: return L"Over Time";
    case 204: return L"Software ECC Recovery";
    case 205: return L"Thermal Assemblies";
    case 206: return L"HDD (Wear)";
    case 207: return L"SATA Downshift Error Count";
    case 208: return L"Average Erase Count";
    case 209: return L"Minimum Spares";
    case 210: return L"Sensoring Error Rate";
    case 211: return L"Minimum Spares";
    case 220: return L"Disk Shift";
    case 221: return L"TCO Reels Count";
    case 222: return L"Loaded Hours";
    case 223: return L"Load Friction / Load Retry Count";
    case 224: return L"Load Friction";
    case 225: return L"Loaded Hours";
    case 226: return L"Load Bytes";
    case 227: return L"Internal Power Device";
    case 228: return L"Power Staging Assist";
    case 229: return L"Flash Writes";
    case 230: return L"Drive Control";
    case 231: return L"Good Wink Count";
    case 232: return L"Good Wink Count";
    case 233: return L"Average Block-Erase Count";
    case 234: return L"Uncorrectable Sector Count";
    case 240: return L"Head Flying Hours";
    case 241: return L"Total LBAs Written";
    case 242: return L"Total LBAs Read";
    case 250: return L"Read Error Retry Rate";
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"Bilinmeyen atr (id=%u)", (unsigned)id);
    return buf;
}

CmdResult command(HANDLE hDevice, const Taskfile& tf, Direction dir, void* buffer,
                  uint32_t bufferLen, uint32_t timeoutSec) {
    uint8_t cdb[16] = {0};
    // Register read-back is only free on non-data requests; PIO transfers keep CK_COND clear
    // so the bridge returns the payload instead of a check condition.
    const bool ckCond = (dir == Direction::NoData);
    buildCdb(cdb, tf, dir, ckCond);
    return scsiCmd(hDevice, cdb, 16, dir, buffer, bufferLen, timeoutSec);
}

bool readIdentify(HANDLE hDevice, Identify& out, std::wstring* failDetail) {
    out = Identify{};
    if (failDetail) failDetail->clear();
    uint8_t buf[512] = {0};
    Taskfile tf;
    tf.command = opIdentifyDevice;
    tf.count = 1;
    tf.lbaLow = 1;
    CmdResult r = command(hDevice, tf, Direction::DataIn, buf, sizeof(buf), 30);
    if (!r.deviceOk) {
        out.valid = false;
        // The refusal itself is the finding: "no SMART here" is only honest once the tool can
        // show which layer said no (Windows IOCTL, SCSI status, or the drive's own ATA status).
        if (failDetail) *failDetail = describeResult(r);
        return false;
    }
    for (int i = 0; i < 256; ++i) out.w[i] = le16(buf + i * 2);
    out.valid = true;
    out.capabilityWordsValid =
        ((out.w[82] & out.w[85]) == (out.w[85] & out.w[86])) && (out.w[85] & 0x01);
    return true;
}

std::wstring Identify::serial() const { return core::trimCopy(core::decodeAtaString(w + 10, 10)); }
std::wstring Identify::firmware() const { return core::trimCopy(core::decodeAtaString(w + 23, 4)); }
std::wstring Identify::model() const { return core::trimCopy(core::decodeAtaString(w + 27, 20)); }
std::wstring Identify::modelTrusted() const {
    std::wstring s = core::trimCopy(core::decodeAtaString(w + 170, 6));
    return s.empty() ? model() : s;
}

uint64_t Identify::userAddressableSectors() const {
    // Words 100-103 (48-bit) are authoritative; 60-61 saturates at 137 GiB and is only a
    // fallback for pre-ATA/6 devices.
    uint64_t wide = (uint64_t(w[101]) << 16) | w[100];
    uint64_t wideHi = (uint64_t(w[103]) << 16) | w[102];
    uint64_t v = wide | (wideHi << 32);
    if (v) return v;
    return (uint64_t(w[61]) << 16) | w[60];
}

Identify::SectorSizes Identify::sectorSizes() const {
    SectorSizes s;
    const uint16_t w106 = w[106];
    if ((w106 & 0xC000) != 0xC000) return s;  // bits 15:14 must be 1 for this word to mean anything
    s.valid = true;
    s.longerThan512 = (w106 & 0x1000) != 0;
    s.multiplePerPhysical = (w106 & 0x2000) != 0;
    const uint32_t shift = w106 & 0x000F;
    s.logical = s.longerThan512 ? ((uint32_t(w[118]) << 16) | w[117]) : 512u;
    if (s.logical == 0) s.logical = 512u;
    s.physical = s.multiplePerPhysical ? s.logical << shift : s.logical;
    return s;
}

SecurityState securityStateOf(const Identify& id) {
    SecurityState st;
    if (!id.valid) return st;
    const uint16_t w128 = id.w[128];
    st.valid = true;
    st.supported = (w128 & 0x01) != 0;
    st.enabled = (w128 & 0x02) != 0;
    st.locked = (w128 & 0x04) != 0;
    st.frozen = (w128 & 0x08) != 0;
    st.countExpired = (w128 & 0x10) != 0;
    st.enhancedEraseSupported = (w128 & 0x20) != 0;
    st.masterIsMaximum = (w128 & 0x0100) != 0;
    return st;
}

bool readSmart(HANDLE hDevice, const Identify& id, SmartSnapshot& out) {
    out = SmartSnapshot{};
    if (!id.valid || !id.smartSupported()) {
        out.note = L"Bu surucu IDENTIFY kelime 82 bit 0'a gore SMART desteklemiyor.";
        return false;
    }

    auto smartCmd = [&](uint8_t feature, Direction dir, void* buf, uint32_t len,
                        uint32_t timeoutSec) {
        Taskfile tf;
        tf.command = opSmart;
        tf.features = feature;
        tf.lbaMid = kSmartLbaMid;
        tf.lbaHigh = kSmartLbaHigh;
        tf.count = 1;
        if (feature == featSmartReadThresholds) tf.lbaLow = 1;
        return command(hDevice, tf, dir, buf, len, timeoutSec);
    };

    uint8_t data[512] = {0};
    CmdResult rd = smartCmd(featSmartReadData, Direction::DataIn, data, sizeof(data), 30);
    if (!rd.deviceOk) {
        out.note = L"SMART READ DATA basarisiz: " + describeResult(rd);
        return false;
    }
    if (!checksumOk(data)) out.note = L"SMART yapisi checksum uyarisi (veriler yine de kullanilabilir).";

    uint8_t thr[512] = {0};
    bool haveThresholds =
        smartCmd(featSmartReadThresholds, Direction::DataIn, thr, sizeof(thr), 30).deviceOk;

    for (int i = 0; i < 30; ++i) {
        const uint8_t* p = data + 2 + i * 12;
        SmartAttribute a;
        a.id = p[0];
        if (a.id == 0) continue;  // unused slot
        a.flags = le16(p + 1);
        a.value = p[3];
        a.worst = p[4];
        a.raw = le48(p + 5);
        if (haveThresholds) {
            const uint8_t* t = thr + 2 + i * 12;
            if (t[0] == a.id) {
                a.threshold = t[1];
                a.thresholdValid = true;
            }
        }
        out.attributes.push_back(a);
    }

    // Bytes 362-376 of the SMART READ DATA structure: offline collection state, the result
    // byte of the most recent self-test, and the device's own test-duration estimates.
    out.revision = le16(data + 0);
    out.offlineDataCollectionStatus = data[362];
    out.selfTestStatus = data[363];
    out.totalTimeToCompleteOfflineMin = le16(data + 364);
    out.offlineDataCollectionCapability = data[367];
    out.errorLogCapability = data[370];
    out.shortTestMinutes = data[372];
    out.conveyanceTestMinutes = data[374];
    // Word 375-376 carries the 16-bit extended-test estimate; the single byte at 373 is the
    // legacy view and is only consulted when the word says nothing.
    out.extendedTestMinutes = le16(data + 375) ? le16(data + 375) : data[373];

    // SMART RETURN STATUS: the verdict is carried by the echoed LBA mid/high registers.
    Taskfile st;
    st.command = opSmart;
    st.features = featSmartReturnStatus;
    st.lbaMid = kSmartLbaMid;
    st.lbaHigh = kSmartLbaHigh;
    CmdResult rs = command(hDevice, st, Direction::NoData, nullptr, 0, 30);
    if (rs.sense.ataReturnPresent) {
        const uint8_t mid = rs.sense.ataLbaMid;
        const uint8_t high = rs.sense.ataLbaHigh;
        if (mid == kSmartLbaMid && high == kSmartLbaHigh)
            out.health = Health::Passed;
        else if (mid == kSmartStatusFailedMid && high == kSmartStatusFailedHigh)
            out.health = Health::Failed;
        else
            out.health = Health::Indeterminate;
        out.smartEnabledByDevice = true;
    } else {
        out.health = Health::Unknown;
        out.note += rs.deviceOk ? L" SMART RETURN STATUS register geri dondurmedi."
                                : L" SMART RETURN STATUS reddedildi: " + describeResult(rs);
    }
    return !out.attributes.empty();
}

const wchar_t* healthLabel(Health h) {
    switch (h) {
    case Health::Passed:        return L"PASSED";
    case Health::Failed:        return L"FAILED";
    case Health::Indeterminate: return L"belirsiz";
    case Health::Unknown:       return L"bilinmiyor";
    }
    return L"bilinmiyor";
}

bool normalizePassword(const std::wstring& in, std::string& out, std::wstring& why) {
    if (in.empty()) {
        why = L"Parola bos olamaz.";
        return false;
    }
    std::string s = core::narrowAnsi(in);
    if (s.size() > 32) {
        why = L"ATA Security parolasi en fazla 32 bayt olabilir (verilen: " +
              core::formatU64(s.size()) + L"). Keserek kisaltmiyorum.";
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7E) {
            why = L"Parola yalnizca gorunur ASCII 0x21-0x7E icerebilir (bayt " +
                  core::formatU64(i) + L" gecersiz).";
            return false;
        }
    }
    out = s;
    why.clear();
    return true;
}

namespace {

// Security status data structure: byte 0 carries user/master + erase-selection bits and the
// password occupies bytes 2..33, NUL padded (hdparm: data[0]=master, memcpy(data+2, pass, 32)).
void fillSecurityPacket(uint8_t* pkt, bool master, const std::string& password) {
    memset(pkt, 0, 512);
    pkt[0] = master ? 0x01 : 0x00;
    memcpy(pkt + 2, password.data(), password.size());
}

CmdResult securityDataOut(HANDLE hDevice, uint8_t opcode, const uint8_t* pkt,
                          uint32_t timeoutSec) {
    Taskfile tf;
    tf.command = opcode;
    tf.count = 1;  // one 512-byte block of Security status data
    return command(hDevice, tf, Direction::DataOut, const_cast<uint8_t*>(pkt), 512, timeoutSec);
}

}  // namespace

CmdResult securitySetPassword(HANDLE hDevice, bool master, const std::string& password,
                              bool eraseLockingMedia, uint16_t masterRevisionCode) {
    uint8_t pkt[512];
    fillSecurityPacket(pkt, master, password);
    if (eraseLockingMedia) pkt[1] |= 0x01;  // 0 = High, 1 = Maximum erase-locking media
    if (master && masterRevisionCode) {
        pkt[34] = uint8_t(masterRevisionCode & 0xFF);
        pkt[35] = uint8_t(masterRevisionCode >> 8);
    }
    return securityDataOut(hDevice, opSecuritySetPassword, pkt, 30);
}

CmdResult securityUnlock(HANDLE hDevice, bool master, const std::string& password) {
    uint8_t pkt[512];
    fillSecurityPacket(pkt, master, password);
    return securityDataOut(hDevice, opSecurityUnlock, pkt, 30);
}

CmdResult securityDisablePassword(HANDLE hDevice, bool master, const std::string& password) {
    uint8_t pkt[512];
    fillSecurityPacket(pkt, master, password);
    return securityDataOut(hDevice, opSecurityDisablePassword, pkt, 30);
}

CmdResult securityFreezeLock(HANDLE hDevice) {
    Taskfile tf;
    tf.command = opSecurityFreezeLock;
    return command(hDevice, tf, Direction::NoData, nullptr, 0, 30);
}

CmdResult securityEraseUnit(HANDLE hDevice, bool master, bool enhanced,
                           const std::string& password, uint32_t timeoutSec) {
    // ERASE PREPARE is a no-data command and must immediately precede ERASE UNIT, otherwise
    // the device aborts with ABORTED COMMAND.
    Taskfile prep;
    prep.command = opSecurityErasePrepare;
    CmdResult pr = command(hDevice, prep, Direction::NoData, nullptr, 0, 30);
    if (!pr.deviceOk) return pr;

    uint8_t pkt[512];
    fillSecurityPacket(pkt, master, password);
    if (enhanced) pkt[0] |= 0x02;  // ERASE SPECIFIED / enhanced selection bit
    return securityDataOut(hDevice, opSecurityEraseUnit, pkt, timeoutSec);
}

namespace {

// SMART READ/WRITE LOG: the log address travels in LBA low and the sector count in the
// sector count register; everything else is the fixed 4F/C2 pair.
CmdResult smartLogCmd(HANDLE hDevice, uint8_t feature, uint8_t logAddr, uint8_t sectors,
                      Direction dir, void* buffer, uint32_t bufferLen) {
    Taskfile tf;
    tf.command = opSmart;
    tf.features = feature;
    tf.lbaMid = kSmartLbaMid;
    tf.lbaHigh = kSmartLbaHigh;
    tf.lbaLow = logAddr;
    tf.count = sectors;
    return command(hDevice, tf, dir, buffer, bufferLen, 30);
}

bool allZero(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

std::wstring hexByte(uint8_t b) {
    wchar_t buf[8];
    swprintf(buf, 8, L"%02X", unsigned(b));
    return buf;
}

}  // namespace

bool readSmartLogs(HANDLE hDevice, SmartLogs& out) {
    out = SmartLogs{};
    uint8_t sector[512] = {0};

    // --- summary error log (log address 01h) ----------------------------------------
    CmdResult re = smartLogCmd(hDevice, featSmartReadLog, logErrorSummary, 1,
                               Direction::DataIn, sector, sizeof(sector));
    if (re.deviceOk) {
        out.errorRead = true;
        out.errorLogVersion = sector[0];
        const uint8_t pointer = sector[1];          // slot the device writes next
        out.ataErrorCount = le16(sector + 2 + 5 * 90);
        if (pointer > 5) {
            out.errorNote = L"Hata logu indeks kaydi gecersiz (0x" +
                            hexByte(pointer) + L"); kayitlar siralanamiyor.";
        } else {
            for (int k = 4; k >= 0; --k) {          // newest first, as the circular buffer allows
                const uint8_t* e = sector + 2 + ((pointer + k) % 5) * 90;
                ErrorLogEntry entry;
                entry.index = uint32_t(5 - k);
                if (allZero(e, 90)) continue;       // unused structure: zero-filled per spec
                const uint8_t* err = e + 5 * 12;    // error data structure follows the 5 commands
                const uint8_t* lastCmd = e + 4 * 12;
                entry.errorRegister = err[1];
                entry.sectorCount = err[2];
                entry.statusRegister = err[7];
                entry.state = err[27];
                entry.powerOnHours = le16(err + 28);
                entry.featuresRegister = lastCmd[1];
                entry.commandRegister = lastCmd[7];
                entry.deviceHead = err[6];
                entry.lba = (uint32_t(err[6] & 0x0F) << 24) | (uint32_t(err[5]) << 16) |
                            (uint32_t(err[4]) << 8) | err[3];
                entry.valid = true;
                out.errors.push_back(entry);
            }
        }
    } else {
        out.errorNote = L"Hata logu okunamadi: " + describeResult(re);
    }

    // --- self-test log (log address 06h) ---------------------------------------------
    memset(sector, 0, sizeof(sector));
    CmdResult rs = smartLogCmd(hDevice, featSmartReadLog, logSelfTest, 1,
                               Direction::DataIn, sector, sizeof(sector));
    if (rs.deviceOk) {
        out.selfTestRead = true;
        out.selfTestRevision = le16(sector + 0);
        const uint8_t mostRecent = sector[508];     // index of the slot written next
        if (mostRecent > 20) {
            out.selfTestNote = L"Self-test log indeks kaydi gecersiz (0x" +
                               hexByte(mostRecent) + L").";
        } else {
            for (int i = 20; i >= 0; --i) {
                const uint8_t* e = sector + 2 + ((i + mostRecent) % 21) * 24;
                SelfTestLogEntry entry;
                entry.index = uint32_t(21 - i);
                if (allZero(e, 24)) continue;
                entry.typeCode = e[0];
                entry.statusByte = e[1];
                entry.powerOnHours = le16(e + 2);
                entry.firstErrorLba = uint32_t(e[5]) | (uint32_t(e[6]) << 8) |
                                      (uint32_t(e[7]) << 16) | (uint32_t(e[8]) << 24);
                entry.valid = true;
                out.selfTests.push_back(entry);
            }
        }
    } else {
        out.selfTestNote = L"Self-test logu okunamadi: " + describeResult(rs);
    }
    return out.errorRead || out.selfTestRead;
}

CmdResult smartSetOperations(HANDLE hDevice, bool enable) {
    Taskfile tf;
    tf.command = opSmart;
    tf.features = enable ? featSmartEnableOperations : featSmartDisableOperations;
    tf.lbaMid = kSmartLbaMid;
    tf.lbaHigh = kSmartLbaHigh;
    return command(hDevice, tf, Direction::NoData, nullptr, 0, 30);
}

CmdResult smartSetAutoSave(HANDLE hDevice, bool enable) {
    Taskfile tf;
    tf.command = opSmart;
    tf.features = featSmartAutoSave;
    tf.count = enable ? kSmartAutosaveOnCount : kSmartAutosaveOffCount;
    tf.lbaMid = kSmartLbaMid;
    tf.lbaHigh = kSmartLbaHigh;
    return command(hDevice, tf, Direction::NoData, nullptr, 0, 30);
}

CmdResult smartExecuteSelfTest(HANDLE hDevice, uint8_t code) {
    Taskfile tf;
    tf.command = opSmart;
    tf.features = featSmartExecuteOffline;
    tf.lbaLow = code;  // ACS puts the self-test code in the sector number register
    tf.lbaMid = kSmartLbaMid;
    tf.lbaHigh = kSmartLbaHigh;
    return command(hDevice, tf, Direction::NoData, nullptr, 0, 30);
}

CmdResult smartClearErrorLog(HANDLE hDevice) {
    // A zeroed error log sector: version kept, pointer and error count cleared. Anything
    // else is a guess about what the device expects to receive back.
    uint8_t sector[512] = {0};
    sector[0] = 0x01;
    sector[1] = 0x00;
    return smartLogCmd(hDevice, featSmartWriteLog, logErrorSummary, 1, Direction::DataOut,
                       sector, sizeof(sector));
}

std::wstring SmartSnapshot::offlineStatusText() const {
    std::wstring s;
    switch (offlineDataCollectionStatus & 0x7F) {
    case 0x00: s = L"offline toplama hic baslatilmadi"; break;
    case 0x01: s = L"offline toplama suruyor"; break;
    case 0x02: s = L"offline toplama hatasiz bitti"; break;
    case 0x03: s = L"offline toplama duraklatildi"; break;
    case 0x04: s = L"offline toplama host komutuyla askiya alindi"; break;
    case 0x05: s = L"offline toplama host tarafindan iptal edildi"; break;
    case 0x06: s = L"offline toplama surucu hatasiyla iptal edildi"; break;
    default:
        s = (offlineDataCollectionStatus & 0x7F) >= 0x40 ? L"uretmiciye ozel durum"
                                                         : L"rezerve edilmis durum";
        break;
    }
    s += offlineDataCollectionStatus & 0x80 ? L" | auto-offline ACIK" : L" | auto-offline kapali";
    return s;
}

std::wstring SmartSnapshot::selfTestStatusText() const {
    const uint8_t code = selfTestStatus >> 4;
    const uint8_t remain = uint32_t(selfTestStatus & 0x0F) * 10u;
    const wchar_t* text = L"?";
    switch (code) {
    case 0x0: text = L"onceki test hatasiz bitti veya hic test calismadi"; break;
    case 0x1: text = L"test host tarafindan iptal edildi"; break;
    case 0x2: text = L"test host resetiyle bolundu"; break;
    case 0x3: text = L"test olamayacak sekimde basarisiz (fatal)"; break;
    case 0x4: text = L"test bitti: bilinmeyen bir test elemani basarisiz"; break;
    case 0x5: text = L"test bitti: elektriksel eleman basarisiz"; break;
    case 0x6: text = L"test bitti: servo/seek elemani basarisiz"; break;
    case 0x7: text = L"test bitti: okuma elemani basarisiz"; break;
    case 0x8: text = L"test bitti: surucu dusmuste (handling damage) suphesi"; break;
    case 0xF: text = L"TEST SURUYOR"; break;
    default: text = L"rezerve deger"; break;
    }
    std::wstring s = std::wstring(text);
    if (code == 0xF) s += L", kalan %" + core::formatU64(remain);
    return s + L" (bayt 363 = " + hexByte(selfTestStatus) + L")";
}

std::wstring selfTestCodeLabel(uint8_t code) {
    switch (code) {
    case stAutoSaveOffline: return L"Offline (otomatik)";
    case stShort:           return L"Kisa offline";
    case stExtended:        return L"Uzun offline";
    case stConveyance:      return L"Tasima testi";
    case stSelective:       return L"Secmeli offline";
    case stAbort:           return L"Testi durdur";
    default: break;
    }
    if (code >= 0x81 && code <= 0x84) return L"Captive varyanti";
    if (code >= 0x90 || (code >= 0x40 && code <= 0x7E)) return L"Ureticiye ozel test";
    return L"Rezerve/karinisik kod";
}

std::wstring ErrorLogEntry::stateText() const {
    switch (state & 0x0F) {
    case 0x0: return L"bilinmeyen durum";
    case 0x1: return L"uyku (sleep)";
    case 0x2: return L"standby";
    case 0x3: return L"active/idle";
    case 0x4: return L"SMART offline/self-test sirasinda";
    default: break;
    }
    return (state & 0x0F) >= 0x0B ? L"uretmiciye ozel" : L"rezerve";
}

std::wstring SelfTestLogEntry::statusText() const {
    const uint8_t code = statusByte >> 4;
    const wchar_t* text = L"rezerve";
    switch (code) {
    case 0x0: text = L"hatasiz tamamlandi"; break;
    case 0x1: text = L"host iptal etti"; break;
    case 0x2: text = L"host resetiyle bolundu"; break;
    case 0x3: text = L"fatal/bilinmeyen hata"; break;
    case 0x4: text = L"bilinmeyen eleman basarisiz"; break;
    case 0x5: text = L"elektriksel eleman basarisiz"; break;
    case 0x6: text = L"servo/seek basarisiz"; break;
    case 0x7: text = L"okuma basarisiz"; break;
    case 0x8: text = L"dusme hasari suphesi"; break;
    case 0xF: text = L"SURUYOR"; break;
    default: break;
    }
    std::wstring s = std::wstring(text);
    if (code == 0xF) s += L" %" + core::formatU64(percentRemaining());
    return s;
}

bool SelfTestLogEntry::failed() const {
    const uint8_t code = statusByte >> 4;
    return code >= 0x3 && code <= 0x8;
}

std::wstring SmartLogs::summary() const {
    std::wstring s;
    if (errorRead) {
        s += L"hata logu v" + core::formatU64(errorLogVersion) + L": " +
             core::formatU64((uint64_t)errors.size()) + L" kayit (cihaz toplami " +
             core::formatU64(ataErrorCount) + L" hata saydi)";
    } else if (!errorNote.empty()) {
        s += errorNote;
    }
    if (selfTestRead) {
        if (!s.empty()) s += L" | ";
        s += L"self-test log rev" + core::formatU64(selfTestRevision) + L": " +
             core::formatU64((uint64_t)selfTests.size()) + L" kayit";
    } else if (!selfTestNote.empty()) {
        if (!s.empty()) s += L" | ";
        s += selfTestNote;
    }
    return s.empty() ? L"SMART logu okunmadi" : s;
}

}  // namespace ata
}  // namespace hw
