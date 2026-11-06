#pragma once
#include "../pch.h"
#include "str.h"
#include <optional>
#include <cstring>

namespace core {

enum class BusKind : uint8_t { Unknown, Ata, Sata, Sas, Usb, Nvme, Raid, Virtual, Iscsi, Sd, Other };
enum class MediaKind : uint8_t { Unknown, Hdd, Ssd, Hybrid, UsbFlash, Virtual, NvmeSsd, NvmeHdd };

// Why a device reports the role it does; keeps "system disk" claims honest instead of guessed.
enum class DiskRole : uint8_t {
    Unknown = 0,
    DataOnly,
    SystemReserved,   // EFI system partition / MSR present
    OsVolume,         // hosts the volume that holds %SystemRoot%
    PageFile,         // hosts an active pagefile
    Recovery,         // OEM/repair partition only
};

struct VolumeOnDisk {
    std::wstring volumeGuidPath;   // "\\?\Volume{guid}" form
    std::wstring mountPoint;       // "C:\" when present
    std::wstring fileSystem;
    uint64_t capacityBytes = 0;
    uint64_t freeBytes = 0;
    bool bootForCurrentOs = false;
    bool pagefileCandidate = false;
};

// One entry of the on-disk partition table, read straight from the device rather than
// inferred: wiping has to show what it will destroy, including the partitions that carry no
// drive letter at all.
struct PartitionInfo {
    uint32_t index = 0;
    std::wstring style;            // "GPT" or "MBR"
    std::wstring typeText;         // resolved from the GUID / MBR byte, else the raw value
    std::wstring name;             // GPT partition name; empty on MBR, which has none
    uint64_t startLba = 0;
    uint64_t sectorCount = 0;
    uint64_t offsetBytes = 0;
    uint64_t sizeBytes = 0;
    bool bootable = false;         // MBR boot indicator only; GPT has no per-partition boot bit
    bool hidden = false;
    bool noLetter = false;
    bool platformRequired = false;
    bool unused = true;            // MBR empty slot or GPT all-zero entry
    std::wstring letters;          // drive letters of volumes sitting on this partition
};

// The whole partition table of one disk plus the style Windows reports for it. "read" is
// separate from "has partitions": a bare disk is a real answer, an IOCTL failure is not.
struct DiskLayout {
    bool read = false;
    std::wstring style;            // "GPT" / "MBR" / "BARE"
    std::wstring errorText;        // why the layout could not be read
    std::vector<PartitionInfo> parts;
    uint64_t usableStartLba = 0;   // GPT first usable LBA; 0 when the style does not report it
    uint64_t usableEndLba = 0;
};

// IOCTL_DISK_GET_DISK_ATTRIBUTES: the mount state Windows itself believes the disk is in.
struct DiskAttributes {
    bool read = false;
    bool readOnly = false;
    bool offline = false;
    bool notReady = false;
    uint64_t raw = 0;
    uint64_t supported = 0;
    std::wstring errorText;
};

struct DiskGeometry {
    uint64_t sizeBytes = 0;
    uint32_t logicalSectorSize = 0;
    uint32_t physicalSectorSize = 0;
    uint64_t logicalSectorCount = 0;
    uint64_t physicalSectorCount = 0;
    uint32_t tracksPerCylinder = 0;
    uint32_t sectorsPerTrack = 0;
    bool valid = false;
};

// A physical-drive handle plus sector-accurate, alignment-safe I/O.
class Device {
public:
    Device() = default;
    explicit Device(int physicalNumber);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    // Windows refuses GENERIC_WRITE on a PhysicalDrive while any volume on it is mounted,
    // so callers must state their intent and let dismounting happen explicitly.
    bool openForRead();
    bool openForUnbufferedRead();
    bool openForRawWrite();
    void close();

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE handle() const { return handle_; }
    int number() const { return number_; }

    // Byte offsets are always expressed in logical sectors; a 4Kn drive must not be
    // addressed as if it were 512-byte.
    bool sectorSize(uint32_t& logicalOut, uint32_t& physicalOut) const;
    uint64_t capacityBytes() const { return geometry_.sizeBytes; }
    const DiskGeometry& geometry() const { return geometry_; }

    bool readSectors(uint64_t startLba, void* buffer, uint64_t sectorCount, std::wstring& errorOut);
    bool writeSectors(uint64_t startLba, const void* buffer, uint64_t sectorCount, std::wstring& errorOut);
    bool flush(std::wstring& errorOut);

    bool sendIoctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize,
                   DWORD& returnedOut, std::wstring& errorOut) const;

    static bool isSectorAligned(const void* ptr, uint32_t sectorSize);

private:
    bool openShared(DWORD access, DWORD flagsAndAttrs);
    void refreshGeometry();

    int number_ = -1;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DiskGeometry geometry_;
};

// Page-aligned scratch buffer: FILE_FLAG_NO_BUFFERING rejects anything else, and the
// surface scan and wipe passes live here for their whole run.
class AlignedBuffer {
public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(size_t bytes) { reset(bytes); }
    ~AlignedBuffer() { release(); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& o) noexcept : ptr_(o.ptr_), size_(o.size_) { o.ptr_ = nullptr; o.size_ = 0; }
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) { release(); ptr_ = o.ptr_; size_ = o.size_; o.ptr_ = nullptr; o.size_ = 0; }
        return *this;
    }

    bool reset(size_t bytes) {
        release();
        // Round to the page so alignment holds for 512/2048/4096-byte sectors alike.
        size_t rounded = (bytes + 0xFFFF) & ~(size_t)0xFFFF;
        ptr_ = (uint8_t*)VirtualAlloc(nullptr, rounded, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        size_ = ptr_ ? bytes : 0;
        return ptr_ != nullptr;
    }
    void release() {
        if (ptr_) { VirtualFree(ptr_, 0, MEM_RELEASE); ptr_ = nullptr; }
        size_ = 0;
    }
    void zero() { if (ptr_) memset(ptr_, 0, size_); }
    uint8_t* data() const { return ptr_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    uint8_t* ptr_ = nullptr;
    size_t size_ = 0;
};

namespace diskinfo {

BusKind queryBusKind(HANDLE h);
MediaKind queryMediaKind(const Device& dev, BusKind bus, const std::wstring& modelHint);
std::wstring busLabel(BusKind bus);
std::wstring mediaLabel(MediaKind media);

// Serial numbers: STORAGE_DESCRIPTOR first, then the media-serial IOCTL that UASP bridges expose.
std::wstring querySerial(const Device& dev);
std::wstring queryModel(const Device& dev);
std::wstring queryFirmware(const Device& dev);
std::optional<std::wstring> queryMediaSerialNaive(HANDLE h);

// Partition + volume inspection used to guard destructive operations.
std::vector<VolumeOnDisk> volumesOnDisk(int physicalNumber);
DiskLayout readDiskLayout(const Device& dev, const std::vector<VolumeOnDisk>& volumes);
DiskAttributes queryDiskAttributes(const Device& dev);
// The three bits IOCTL_DISK_GET/SET_DISK_ATTRIBUTES exposes. Windows has no per-disk "hidden"
// or "mount manager" flag in this IOCTL, so nothing beyond these is claimed.
constexpr uint64_t kDiskAttrOffline = 0x1;
constexpr uint64_t kDiskAttrReadOnly = 0x2;
constexpr uint64_t kDiskAttrNotReady = 0x4;

// Both attribute writes are reversible and destroy nothing, which is what makes them safe to
// offer next to the wipe buttons; the reason for a refusal is returned, never swallowed.
bool setDiskAttributeFlags(int physicalNumber, uint64_t setBits, uint64_t clearBits,
                           std::wstring& why);
std::wstring partitionTypeText(const PARTITION_INFORMATION_EX& p, const std::wstring& style);
DiskRole classifyDiskRole(int physicalNumber, const std::vector<VolumeOnDisk>& volumes);
std::wstring roleLabel(DiskRole role);
bool diskHasPartitionTable(const Device& dev);

// Locks and dismounts every volume on the disk. Reports the ones that stayed busy:
// a partially dismounted disk must never be silently wiped.
struct DismountResult {
    bool allDismounted = false;
    std::vector<std::wstring> stillBusy;
    std::vector<std::wstring> dismounted;
};
DismountResult dismountVolumes(int physicalNumber);

// Puts back the volumes a destructive job took offline. Raw sector writes leave the mount
// state dirty, so destructive paths must pair this with dismountVolumes() and the caller has
// to report the volumes that stayed offline instead of leaving the user with a "missing" disk.
bool remountVolumes(int physicalNumber, std::vector<std::wstring>& failedOut);

}  // namespace diskinfo

bool processIsAdmin();

}  // namespace core
