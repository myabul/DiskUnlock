#include "device.h"
#include "log.h"

namespace core {

Device::Device(int physicalNumber) : number_(physicalNumber) {}

Device::~Device() { close(); }

Device::Device(Device&& other) noexcept
    : number_(other.number_), handle_(other.handle_), geometry_(other.geometry_) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.number_ = -1;
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        close();
        number_ = other.number_;
        handle_ = other.handle_;
        geometry_ = other.geometry_;
        other.handle_ = INVALID_HANDLE_VALUE;
        other.number_ = -1;
    }
    return *this;
}

std::wstring drivePath(int number) {
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
}

bool Device::openShared(DWORD access, DWORD flags) {
    close();
    HANDLE h = CreateFileW(drivePath(number_).c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
    refreshGeometry();
    return true;
}

bool Device::openForRead() {
    // Buffered read: metadata and low-level queries. Volume-owner sectors may still be refused
    // by the storage stack, which callers must surface instead of calling a hardware fault.
    return openShared(GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN);
}

bool Device::openForUnbufferedRead() {
    // Unbuffered: the only way a latency measurement reflects the device rather than the
    // system cache. Requires page-aligned buffers and sector-multiple offsets/lengths.
    return openShared(GENERIC_READ, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN);
}

bool Device::openForRawWrite() {
    if (openShared(GENERIC_READ | GENERIC_WRITE, 0)) return true;
    // Storage-Elevation (Windows 11+) allows unmounted raw writes without full admin.
    return openShared(GENERIC_READ | GENERIC_WRITE,
                      FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN);
}

void Device::close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

void Device::refreshGeometry() {
    geometry_ = DiskGeometry{};
    DWORD returned = 0;
    DISK_GEOMETRY_EX ex{};
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &ex, sizeof(ex),
                        &returned, nullptr) && ex.DiskSize.QuadPart > 0) {
        geometry_.sizeBytes = (uint64_t)ex.DiskSize.QuadPart;
        geometry_.logicalSectorSize = ex.Geometry.BytesPerSector ? ex.Geometry.BytesPerSector : 512;
        geometry_.tracksPerCylinder = ex.Geometry.TracksPerCylinder;
        geometry_.sectorsPerTrack = ex.Geometry.SectorsPerTrack;
        geometry_.logicalSectorCount = geometry_.sizeBytes / geometry_.logicalSectorSize;
        geometry_.physicalSectorCount = geometry_.logicalSectorCount;
        geometry_.physicalSectorSize = geometry_.logicalSectorSize;
        geometry_.valid = true;
        return;
    }
    GET_LENGTH_INFORMATION len{};
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof(len),
                        &returned, nullptr) && len.Length.QuadPart > 0) {
        geometry_.sizeBytes = (uint64_t)len.Length.QuadPart;
        geometry_.logicalSectorSize = 512;
        geometry_.logicalSectorCount = geometry_.sizeBytes / 512;
        geometry_.physicalSectorCount = geometry_.logicalSectorCount;
        geometry_.physicalSectorSize = 512;
        geometry_.valid = true;
    }
}

bool Device::sectorSize(uint32_t& logicalOut, uint32_t& physicalOut) const {
    if (!geometry_.valid || geometry_.logicalSectorSize == 0) return false;
    logicalOut = geometry_.logicalSectorSize;
    physicalOut = geometry_.physicalSectorSize ? geometry_.physicalSectorSize : geometry_.logicalSectorSize;
    return true;
}

bool Device::readSectors(uint64_t startLba, void* buffer, uint64_t sectorCount, std::wstring& errorOut) {
    uint32_t sec = geometry_.logicalSectorSize ? geometry_.logicalSectorSize : 512;
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)(startLba * sec);
    if (!SetFilePointerEx(handle_, off, nullptr, FILE_BEGIN)) {
        errorOut = core::lastErrorText();
        return false;
    }
    DWORD want = (DWORD)(sectorCount * sec);
    DWORD got = 0;
    if (!ReadFile(handle_, buffer, want, &got, nullptr)) {
        errorOut = core::lastErrorText();
        return false;
    }
    if (got != want) {
        errorOut = L"short read: beklenen " + core::formatU64(want) + L", alinan " + core::formatU64(got);
        return false;
    }
    return true;
}

bool Device::writeSectors(uint64_t startLba, const void* buffer, uint64_t sectorCount, std::wstring& errorOut) {
    uint32_t sec = geometry_.logicalSectorSize ? geometry_.logicalSectorSize : 512;
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)(startLba * sec);
    if (!SetFilePointerEx(handle_, off, nullptr, FILE_BEGIN)) {
        errorOut = core::lastErrorText();
        return false;
    }
    DWORD want = (DWORD)(sectorCount * sec);
    DWORD put = 0;
    if (!WriteFile(handle_, buffer, want, &put, nullptr)) {
        errorOut = core::lastErrorText();
        return false;
    }
    if (put != want) {
        errorOut = L"short write: beklenen " + core::formatU64(want) + L", yazilan " + core::formatU64(put);
        return false;
    }
    return true;
}

bool Device::flush(std::wstring& errorOut) {
    if (FlushFileBuffers(handle_)) return true;
    errorOut = core::lastErrorText();
    return false;
}

bool Device::sendIoctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize,
                       DWORD& returnedOut, std::wstring& errorOut) const {
    returnedOut = 0;
    if (DeviceIoControl(handle_, code, (LPVOID)in, inSize, out, outSize, &returnedOut, nullptr)) return true;
    errorOut = core::lastErrorText();
    return false;
}

bool Device::isSectorAligned(const void* ptr, uint32_t sectorSize) {
    return (reinterpret_cast<uintptr_t>(ptr) % (sectorSize ? sectorSize : 512)) == 0;
}

namespace diskinfo {

static bool queryDescriptor(const Device& dev, std::vector<uint8_t>& outBuf) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    outBuf.assign(4096, 0);
    DWORD returned = 0;
    std::wstring err;
    if (!dev.sendIoctl(IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), outBuf.data(),
                       (DWORD)outBuf.size(), returned, err))
        return false;
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(outBuf.data());
    if (returned < sizeof(STORAGE_DEVICE_DESCRIPTOR) || d->Size > returned) return false;
    return true;
}

static std::wstring asciiAt(const std::vector<uint8_t>& buf, DWORD offset, DWORD limit) {
    if (offset == 0 || offset >= limit || offset >= buf.size()) return std::wstring();
    std::string s;
    for (size_t i = offset; i < buf.size(); ++i) {
        char c = (char)buf[i];
        if (c == '\0') break;
        s.push_back(c);
    }
    return core::trimCopy(core::widen(s));
}

BusKind queryBusKind(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buf(4096, 0);
    DWORD returned = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf.data(),
                         (DWORD)buf.size(), &returned, nullptr))
        return BusKind::Unknown;
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    switch (d->BusType) {
    case BusTypeAta:  return BusKind::Ata;
    case BusTypeSata: return BusKind::Sata;
    case BusTypeSas:  return BusKind::Sas;
    case BusTypeUsb:  return BusKind::Usb;
    case BusTypeNvme: return BusKind::Nvme;
    case BusTypeRAID: return BusKind::Raid;
    case BusTypeiScsi: return BusKind::Iscsi;
    case BusTypeSd:
    case BusTypeMmc:  return BusKind::Sd;
    default:          return BusKind::Other;
    }
}

MediaKind queryMediaKind(const Device& dev, BusKind bus, const std::wstring& modelHint) {
    if (bus == BusKind::Virtual) return MediaKind::Virtual;
    if (modelHint.find(L"VHD") != std::wstring::npos || modelHint.find(L"Virtual Disk") != std::wstring::npos)
        return MediaKind::Virtual;
    if (bus == BusKind::Nvme) return MediaKind::NvmeSsd;
    if (bus == BusKind::Usb) return MediaKind::UsbFlash;

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
    DWORD returned = 0;
    std::wstring err;
    if (dev.sendIoctl(IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), returned, err) &&
        returned >= sizeof(sp)) {
        return sp.IncursSeekPenalty ? MediaKind::Hdd : MediaKind::Ssd;
    }
    return MediaKind::Unknown;
}

std::wstring busLabel(BusKind bus) {
    switch (bus) {
    case BusKind::Ata:     return L"ATA/PATA";
    case BusKind::Sata:    return L"SATA (AHCI)";
    case BusKind::Sas:     return L"SAS";
    case BusKind::Usb:     return L"USB";
    case BusKind::Nvme:    return L"NVMe PCIe";
    case BusKind::Raid:    return L"RAID (volume)";
    case BusKind::Iscsi:   return L"iSCSI";
    case BusKind::Sd:      return L"SD/MMC";
    case BusKind::Virtual: return L"Virtual";
    case BusKind::Other:   return L"Diger";
    default:               return L"Bilinmiyor";
    }
}

std::wstring mediaLabel(MediaKind m) {
    switch (m) {
    case MediaKind::Hdd:      return L"Mekanik HDD";
    case MediaKind::Ssd:      return L"SSD";
    case MediaKind::Hybrid:   return L"Hibrit";
    case MediaKind::UsbFlash: return L"USB Flash";
    case MediaKind::Virtual:  return L"Sanal Disk";
    case MediaKind::NvmeSsd:  return L"NVMe SSD";
    case MediaKind::NvmeHdd:  return L"NVMe HDD";
    case MediaKind::Unknown:  break;
    }
    return L"Bilinmiyor";
}

std::wstring querySerial(const Device& dev) {
    std::vector<uint8_t> buf;
    if (queryDescriptor(dev, buf)) {
        auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        std::wstring s = asciiAt(buf, d->SerialNumberOffset, d->Size);
        if (!s.empty()) return s;
    }
    // UASP/USB bridges expose the serial only through the media-serial IOCTL.
    auto naive = queryMediaSerialNaive(dev.handle());
    if (naive && !naive->empty()) return *naive;
    return std::wstring();
}

std::wstring queryModel(const Device& dev) {
    std::vector<uint8_t> buf;
    if (!queryDescriptor(dev, buf)) return std::wstring();
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    std::wstring vendor = asciiAt(buf, d->VendorIdOffset, d->Size);
    std::wstring product = asciiAt(buf, d->ProductIdOffset, d->Size);
    if (vendor.empty()) return product;
    if (product.empty()) return vendor;
    return vendor + L" " + product;
}

std::wstring queryFirmware(const Device& dev) {
    std::vector<uint8_t> buf;
    if (!queryDescriptor(dev, buf)) return std::wstring();
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    return asciiAt(buf, d->ProductRevisionOffset, d->Size);
}

std::optional<std::wstring> queryMediaSerialNaive(HANDLE h) {
    BYTE out[256] = {0};
    DWORD returned = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_MEDIA_SERIAL_NUMBER, nullptr, 0, out, sizeof(out),
                         &returned, nullptr))
        return std::nullopt;
    std::string s((char*)out, returned);
    size_t b = s.find_first_not_of(" \0");
    if (b == std::string::npos) return std::nullopt;
    return core::trimCopy(core::widen(s));
}

std::vector<VolumeOnDisk> volumesOnDisk(int physicalNumber) {
    std::vector<VolumeOnDisk> result;
    wchar_t volumePath[MAX_PATH] = {0};
    HANDLE snap = FindFirstVolumeW(volumePath, ARRAYSIZE(volumePath));
    if (snap == INVALID_HANDLE_VALUE) return result;
    do {
        std::wstring guidPath = L"\\\\?\\" + std::wstring(volumePath);
        HANDLE vh = CreateFileW(guidPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (vh == INVALID_HANDLE_VALUE)
            vh = CreateFileW(guidPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
        if (vh == INVALID_HANDLE_VALUE) continue;

        STORAGE_DEVICE_NUMBER sdn{};
        DWORD returned = 0;
        bool mapped = DeviceIoControl(vh, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof(sdn),
                                      &returned, nullptr);
        CloseHandle(vh);
        if (!mapped || sdn.DeviceType != FILE_DEVICE_DISK || (int)sdn.DeviceNumber != physicalNumber)
            continue;

        VolumeOnDisk v;
        v.volumeGuidPath = guidPath;
        wchar_t names[8192] = {0};
        DWORD needed = 0;
        if (GetVolumePathNamesForVolumeNameW(guidPath.c_str(), names, ARRAYSIZE(names), &needed)) {
            for (wchar_t* p = names; *p;) {
                std::wstring mp = p;
                if (v.mountPoint.empty()) v.mountPoint = mp;
                p += wcslen(p) + 1;
            }
        }
        wchar_t fs[64] = {0};
        if (GetVolumeInformationW(guidPath.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs,
                                  ARRAYSIZE(fs)))
            v.fileSystem = fs;

        ULARGE_INTEGER total{}, free_{};
        if (!v.mountPoint.empty() && GetDiskFreeSpaceExW(v.mountPoint.c_str(), nullptr, &total, &free_)) {
            v.capacityBytes = total.QuadPart;
            v.freeBytes = free_.QuadPart;
        }
        if (!v.mountPoint.empty()) {
            wchar_t sysRoot[MAX_PATH] = {0};
            if (GetWindowsDirectoryW(sysRoot, ARRAYSIZE(sysRoot))) {
                std::wstring root(sysRoot, sysRoot + 3);  // "C:\"
                if (_wcsicmp(root.c_str(), v.mountPoint.c_str()) == 0) v.bootForCurrentOs = true;
            }
            std::wstring page = v.mountPoint + L"pagefile.sys";
            WIN32_FIND_DATAW fd{};
            HANDLE ff = FindFirstFileW(page.c_str(), &fd);
            if (ff != INVALID_HANDLE_VALUE) { FindClose(ff); v.pagefileCandidate = true; }
        }
        result.push_back(std::move(v));
    } while (FindNextVolumeW(snap, volumePath, ARRAYSIZE(volumePath)));
    FindVolumeClose(snap);
    return result;
}

// Well-known GPT partition type GUIDs. Anything outside the table is reported as its raw
// GUID instead of being renamed by guesswork.
static std::wstring guidText(const GUID& g) {
    wchar_t b[64];
    swprintf(b, 64, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2,
             g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
             g.Data4[6], g.Data4[7]);
    return b;
}

static const GUID kGuidEfi = {0xc12a7328, 0xf81f, 0x11d2, {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};
static const GUID kGuidMsr = {0xe3c9e316, 0x0b5c, 0x4db8, {0x81, 0x7d, 0xf9, 0x2d, 0x00, 0x15, 0x5d, 0x4b}};
static const GUID kGuidBasic = {0xebd0a0a2, 0xb9e5, 0x4433, {0x87, 0xce, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};
static const GUID kGuidRecovery = {0xde94bba4, 0x06d1, 0x4d40, {0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac}};
static const GUID kGuidLinuxSwap = {0x0657fd6d, 0xa4ab, 0x43da, {0x84, 0x43, 0xa9, 0xb8, 0xf5, 0xf8, 0xff, 0xc3}};
static const GUID kGuidLinuxData = {0x0fc63daf, 0x843d, 0x471d, {0x8e, 0x79, 0x4d, 0x47, 0x98, 0xe7, 0xdc, 0xeb}};

// WCHAR[36] with no length promise: it is not always terminated, and the unused tail is
// garbage rather than spaces, so the copy stops at the first NUL and drops the rest.
static std::wstring PrintableName(const WCHAR* raw) {
    size_t len = 0;
    while (len < 36 && raw[len] != 0) ++len;
    std::wstring s(raw, len);
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return std::wstring();
    return core::trimCopy(s);
}

static bool guidAllZero(const GUID& g) {
    static const GUID zero{};
    return memcmp(&g, &zero, sizeof(GUID)) == 0;
}

std::wstring partitionTypeText(const PARTITION_INFORMATION_EX& p, const std::wstring& style) {
    if (style == L"GPT") {
        const GUID& g = p.Gpt.PartitionType;
        if (IsEqualGUID(g, kGuidEfi)) return L"EFI sistem bolumu (FAT32, boot)";
        if (IsEqualGUID(g, kGuidMsr)) return L"Microsoft Reserved (MSR)";
        if (IsEqualGUID(g, kGuidBasic)) return L"Microsoft Basic Data (NTFS/ReFS)";
        if (IsEqualGUID(g, kGuidRecovery)) return L"Windows Recovery";
        if (IsEqualGUID(g, kGuidLinuxSwap)) return L"Linux swap";
        if (IsEqualGUID(g, kGuidLinuxData)) return L"Linux filesystem";
        return L"tip GUID " + guidText(g);
    }
    if (style == L"MBR") {
        const uint8_t t = p.Mbr.PartitionType;
        switch (t) {
        case 0x00: return L"bos slot";
        case 0x01: case 0x11: return L"FAT12";
        case 0x04: case 0x14: case 0x06: case 0x0E: case 0x1C: case 0x0C: return L"FAT32 / W95 LBA";
        case 0x05: case 0x0F: return L"genisletilmis / kap";
        case 0x07: return L"NTFS / IFS";
        case 0x0B: return L"FAT32 CHS";
        case 0x27: return L"Windows Recovery (hidden)";
        case 0x26: return L"hidden NTFS";
        case 0x82: return L"Linux swap";
        case 0x83: return L"Linux";
        case 0x8E: return L"Linux LVM";
        case 0xA5: return L"BSD";
        case 0xEE: return L"GPT koruyucu MBR";
        case 0xEF: return L"EFI / BIOS boot";
        case 0xFB: case 0xFC: return L"VMware VMFS / reserved";
        case 0xFE: return L"LBA kap";
        default: {
            wchar_t b[32];
            swprintf(b, 32, L"MBR tipi 0x%02X", (unsigned)t);
            return b;
        }
        }
    }
    return L"tablo yok";
}

DiskLayout readDiskLayout(const Device& dev, const std::vector<VolumeOnDisk>& volumes) {
    DiskLayout out;
    const int number = dev.number();

    std::vector<uint8_t> buf(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                                 128 * sizeof(PARTITION_INFORMATION_EX),
                             0);
    DWORD returned = 0;
    std::wstring err;
    if (!dev.sendIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), (DWORD)buf.size(),
                       returned, err)) {
        out.errorText = err;
        return out;
    }
    out.read = true;
    auto* lay = reinterpret_cast<PDRIVE_LAYOUT_INFORMATION_EX>(buf.data());

    std::wstring style = lay->PartitionStyle == PARTITION_STYLE_GPT
                             ? L"GPT"
                             : (lay->PartitionStyle == PARTITION_STYLE_MBR ? L"MBR" : L"BARE");
    out.style = style;
    const uint64_t ss = dev.geometry().logicalSectorSize ? dev.geometry().logicalSectorSize : 512;
    if (lay->PartitionStyle == PARTITION_STYLE_GPT) {
        out.usableStartLba = (uint64_t)lay->Gpt.StartingUsableOffset.QuadPart / ss;
        out.usableEndLba = out.usableStartLba +
                           (uint64_t)lay->Gpt.UsableLength.QuadPart / ss;
    }

    // Drive letters are attached by byte range: a volume's extents tell which disk and which
    // offset they live on, which is the only way to say "this partition is D:" honestly.
    struct Ext {
        uint64_t start = 0, end = 0;
        std::wstring letter;
    };
    std::vector<Ext> extents;
    for (const auto& v : volumes) {
        if (v.mountPoint.empty()) continue;
        HANDLE vh = CreateFileW(v.volumeGuidPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
        if (vh == INVALID_HANDLE_VALUE) continue;
        uint8_t ebuf[1024] = {0};
        DWORD got = 0;
        if (DeviceIoControl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, ebuf,
                            sizeof(ebuf), &got, nullptr)) {
            auto* e = reinterpret_cast<PVOLUME_DISK_EXTENTS>(ebuf);
            for (DWORD i = 0; i < e->NumberOfDiskExtents; ++i) {
                if ((int)e->Extents[i].DiskNumber != number) continue;
                Ext x;
                x.start = (uint64_t)e->Extents[i].StartingOffset.QuadPart;
                x.end = x.start + (uint64_t)e->Extents[i].ExtentLength.QuadPart;
                x.letter = v.mountPoint.substr(0, 1);
                extents.push_back(std::move(x));
            }
        }
        CloseHandle(vh);
    }

    for (DWORD i = 0; i < lay->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& p = lay->PartitionEntry[i];
        PartitionInfo pi;
        pi.index = p.PartitionNumber;
        pi.style = style;
        pi.offsetBytes = (uint64_t)p.StartingOffset.QuadPart;
        pi.sizeBytes = (uint64_t)p.PartitionLength.QuadPart;
        pi.startLba = pi.offsetBytes / ss;
        pi.sectorCount = pi.sizeBytes / ss;
        pi.typeText = partitionTypeText(p, style);
        // The boot indicator is an MBR concept. On GPT the firmware finds the ESP by its type
        // GUID, so claiming a GPT "bootable" bit would be a guess dressed up as a reading.
        pi.bootable = style == L"MBR" && p.Mbr.BootIndicator != 0;
        if (style == L"GPT") {
            const uint64_t f = p.Gpt.Attributes;
            pi.hidden = (f & GPT_BASIC_DATA_ATTRIBUTE_HIDDEN) != 0;
            pi.noLetter = (f & GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER) != 0;
            pi.platformRequired = (f & GPT_ATTRIBUTE_PLATFORM_REQUIRED) != 0;
            pi.name = PrintableName(p.Gpt.Name);
        }
        pi.unused = pi.sizeBytes == 0 ||
                    (style == L"GPT" && guidAllZero(p.Gpt.PartitionType)) ||
                    (style == L"MBR" && p.Mbr.PartitionType == 0);
        for (const Ext& x : extents) {
            if (x.start >= pi.offsetBytes && x.end <= pi.offsetBytes + pi.sizeBytes) {
                if (pi.letters.find(x.letter) == std::wstring::npos) pi.letters += x.letter;
            }
        }
        out.parts.push_back(std::move(pi));
    }
    return out;
}

// MinGW's winioctl.h still predates the disk-attribute IOCTLs, so the documented wire layout
// and control codes are spelled out here. Index 0x3f with FILE_READ_ACCESS for the read and
// FILE_ANY_ACCESS for the write is what the Windows driver expects; a mismatch would show up
// as a refused IOCTL rather than silently lying about the flags.
struct GET_DISK_ATTRS {
    uint32_t Version;
    uint32_t Reserved1;
    uint64_t SupportedAttributes;
    uint64_t Attributes;
};

struct SET_DISK_ATTRS {
    uint64_t Version;             // documented as sizeof(SET_DISK_ATTRIBUTES) via a union
    uint64_t Attributes;
    uint64_t AttributesMask;
    uint64_t Reserved2;
};

static constexpr DWORD kIoctlGetDiskAttrs =
    CTL_CODE(IOCTL_DISK_BASE, 0x003f, METHOD_BUFFERED, FILE_READ_ACCESS);
static constexpr DWORD kIoctlSetDiskAttrs =
    CTL_CODE(IOCTL_DISK_BASE, 0x003f, METHOD_BUFFERED, FILE_ANY_ACCESS);
static constexpr DWORD kIoctlUpdateDiskProps =
    CTL_CODE(IOCTL_DISK_BASE, 0x0050, METHOD_BUFFERED, FILE_ANY_ACCESS);

DiskAttributes queryDiskAttributes(const Device& dev) {
    DiskAttributes a;
    if (!dev.valid()) {
        a.errorText = L"disk haziari yok";
        return a;
    }
    GET_DISK_ATTRS gda{};
    gda.Version = (uint32_t)sizeof(GET_DISK_ATTRS);
    DWORD returned = 0;
    std::wstring err;
    if (dev.sendIoctl(kIoctlGetDiskAttrs, nullptr, 0, &gda, sizeof(gda), returned, err)) {
        a.read = true;
        a.raw = gda.Attributes;
        a.supported = gda.SupportedAttributes;
        a.readOnly = (gda.Attributes & kDiskAttrReadOnly) != 0;
        a.offline = (gda.Attributes & kDiskAttrOffline) != 0;
        a.notReady = (gda.Attributes & kDiskAttrNotReady) != 0;
    } else {
        a.errorText = err;
    }
    return a;
}

bool setDiskAttributeFlags(int physicalNumber, uint64_t setBits, uint64_t clearBits,
                           std::wstring& why) {
    Device dev(physicalNumber);
    // Windows rejects GENERIC_WRITE on a disk whose volumes are mounted, so the caller has to
    // see that exact reason instead of a generic failure.
    if (!dev.openForRawWrite()) {
        why = L"disk yazma icin acilamadi: " + core::winErrorText(GetLastError()) +
              L" (volumeler mount olabilir)";
        return false;
    }
    SET_DISK_ATTRS sda{};
    sda.Version = sizeof(SET_DISK_ATTRS);
    sda.Attributes = setBits;                    // cleared bits are 0 here, which is the point
    sda.AttributesMask = setBits | clearBits;
    DWORD returned = 0;
    if (!dev.sendIoctl(kIoctlSetDiskAttrs, &sda, sizeof(sda), nullptr, 0, returned, why)) {
        why = L"IOCTL_DISK_SET_DISK_ATTRIBUTES: " + why;
        return false;
    }
    // The mount manager keeps its own view of the disk; without this the flag can read back
    // correctly while Explorer still shows the old state.
    dev.sendIoctl(kIoctlUpdateDiskProps, nullptr, 0, nullptr, 0, returned, why);
    return true;
}

DiskRole classifyDiskRole(int physicalNumber, const std::vector<VolumeOnDisk>& volumes) {
    for (const auto& v : volumes) {
        if (v.bootForCurrentOs) return DiskRole::OsVolume;
        if (v.pagefileCandidate) return DiskRole::PageFile;
    }
    // An EFI system partition or MSR on the disk means firmware boots from it: wiping makes the
    // machine unbootable even when no drive letter is assigned to those partitions.
    Device dev(physicalNumber);
    if (dev.openForRead()) {
        std::vector<uint8_t> buf(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                                 128 * sizeof(PARTITION_INFORMATION_EX), 0);
        DWORD returned = 0;
        std::wstring err;
        if (dev.sendIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), (DWORD)buf.size(),
                          returned, err)) {
            auto* l = reinterpret_cast<PDRIVE_LAYOUT_INFORMATION_EX>(buf.data());
            if (l->PartitionStyle == PARTITION_STYLE_GPT) {
                for (DWORD i = 0; i < l->PartitionCount; ++i) {
                    const GUID& t = l->PartitionEntry[i].Gpt.PartitionType;
                    if (IsEqualGUID(t, kGuidEfi) || IsEqualGUID(t, kGuidMsr))
                        return DiskRole::SystemReserved;
                    if (IsEqualGUID(t, kGuidRecovery)) return DiskRole::Recovery;
                }
            }
        }
    }
    return volumes.empty() ? DiskRole::Unknown : DiskRole::DataOnly;
}

std::wstring roleLabel(DiskRole role) {
    switch (role) {
    case DiskRole::DataOnly:       return L"Veri diski";
    case DiskRole::SystemReserved: return L"SISTEM / EFI BOOT";
    case DiskRole::OsVolume:       return L"ISLETIM SISTEMI";
    case DiskRole::PageFile:       return L"PAGEFILE ICERIYOR";
    case DiskRole::Recovery:       return L"KURTARMA BOLUMU";
    default:                       return L"Bilinmiyor";
    }
}

bool diskHasPartitionTable(const Device& dev) {
    std::vector<uint8_t> buf(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 16 * sizeof(PARTITION_INFORMATION_EX), 0);
    DWORD returned = 0;
    std::wstring err;
    if (!dev.sendIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), (DWORD)buf.size(),
                       returned, err))
        return false;
    auto* l = reinterpret_cast<PDRIVE_LAYOUT_INFORMATION_EX>(buf.data());
    if (l->PartitionStyle == PARTITION_STYLE_GPT) return l->PartitionCount > 0;
    if (l->PartitionStyle == PARTITION_STYLE_MBR) {
        for (DWORD i = 0; i < l->PartitionCount && i < 4; ++i) {
            const auto& mbr = l->PartitionEntry[i].Mbr;
            if (mbr.BootIndicator || (mbr.PartitionType != 0 &&
                                      l->PartitionEntry[i].PartitionLength.QuadPart > 0))
                return true;
        }
    }
    return false;
}

DismountResult dismountVolumes(int physicalNumber) {
    DismountResult res;
    for (auto& v : volumesOnDisk(physicalNumber)) {
        HANDLE vh = CreateFileW(v.volumeGuidPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (vh == INVALID_HANDLE_VALUE) {
            res.stillBusy.push_back(v.mountPoint.empty() ? v.volumeGuidPath : v.mountPoint);
            continue;
        }
        DWORD returned = 0;
        bool locked = DeviceIoControl(vh, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
        bool dismounted = DeviceIoControl(vh, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned,
                                         nullptr);
        CloseHandle(vh);
        if (locked && dismounted) {
            res.dismounted.push_back(v.mountPoint.empty() ? v.volumeGuidPath : v.mountPoint);
        } else {
            res.stillBusy.push_back(v.mountPoint.empty() ? v.volumeGuidPath : v.mountPoint);
        }
    }
    res.allDismounted = res.stillBusy.empty() && !res.dismounted.empty();
    return res;
}

// A raw write invalidates the mount state; without this the disk looks "gone" to the user
// after every wipe. Volumes without a mount point are skipped - there is nothing to bring up.
bool remountVolumes(int physicalNumber, std::vector<std::wstring>& failedOut) {
    failedOut.clear();
    for (auto& v : volumesOnDisk(physicalNumber)) {
        if (v.mountPoint.empty()) continue;
        HANDLE vh = CreateFileW(v.volumeGuidPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
        if (vh == INVALID_HANDLE_VALUE) {
            failedOut.push_back(v.mountPoint);
            continue;
        }
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(vh, IOCTL_VOLUME_ONLINE, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(vh);
        if (!ok) failedOut.push_back(v.mountPoint + L" (" + lastErrorText() + L")");
    }
    return failedOut.empty();
}

}  // namespace diskinfo

bool processIsAdmin() {
    BOOL isAdmin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &isAdmin);
        FreeSid(group);
    }
    return isAdmin == TRUE;
}

}  // namespace core
