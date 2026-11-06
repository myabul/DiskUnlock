#pragma once

#define UNICODE
#define _UNICODE
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

// Storage protocols used here (NVMe protocol command, STOR property queries, DPI-awareness
// contexts) only exist from Windows 10 onward.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <dbt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <gdiplus.h>

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <chrono>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <memory>

// winioctl.h does not ship the NVMe log-page struct on MinGW.
#pragma pack(push, 1)
struct NvmeHealthInfoLog {
    uint8_t  criticalWarning;
    uint8_t  compositeTemp[2];
    uint8_t  availableSpare;
    uint8_t  availableSpareThreshold;
    uint8_t  percentageUsed;
    uint8_t  enduranceGroupGlobalEva;
    uint8_t  reserved0[25];
    uint8_t  dataUnitsRead[16];
    uint8_t  dataUnitsWritten[16];
    uint8_t  hostReadCommands[16];
    uint8_t  hostWriteCommands[16];
    uint8_t  controllerBusyTime[16];
    uint8_t  powerCycles[16];
    uint8_t  powerOnHours[16];
    uint8_t  unsafeShutdowns[16];
    uint8_t  mediaErrors[16];
    uint8_t  errorInfoLogEntries[16];
    uint32_t warningCompositeTempTime;
    uint32_t criticalCompositeTempTime;
    uint16_t tempSensor[8];
    uint8_t  reserved1[296];
};
#pragma pack(pop)
static_assert(sizeof(NvmeHealthInfoLog) == 512, "NVMe health log must be 512 bytes");

constexpr uint32_t kNvmeLogHealthInfo = 0x02;
constexpr uint32_t kNvmeLogErrorInfo = 0x01;
constexpr uint32_t kNvmeLogSelfTest = 0x06;
constexpr uint32_t kNvmeLogSanitizeStatus = 0x81;
constexpr uint32_t kNvmeIdentifyCnsController = 0x01;
constexpr uint32_t kNvmeAdminOpcodeFormatNvm = 0x80;
constexpr uint32_t kNvmeAdminOpcodeSanitize = 0x84;
constexpr uint32_t kNvmeAdminOpcodeGetLogPage = 0x02;
constexpr uint32_t kNvmeCmdSize = 64;
constexpr uint32_t kNvmeStatusCodeTypeAdmin = 0x0001;

constexpr uint32_t kStorageProtocolSpecificNvmeAdminCommand = 0x00000001;
