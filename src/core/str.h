#pragma once
#include "../pch.h"

namespace core {

std::wstring widen(const std::string& s);
std::string  narrow(const std::wstring& s);          // CP_UTF8, lossless round-trip safe
std::string  narrowAnsi(const std::wstring& s);      // CP_ACP, for ATA/SCSI byte payloads

// Human readable Win32 error text; "0" for ERROR_SUCCESS so absence of an error is explicit.
std::wstring winErrorText(DWORD code);
std::wstring lastErrorText();

std::wstring formatBytes(uint64_t bytes);
std::wstring formatU64(uint64_t v);
std::wstring formatHex(uint64_t v);
std::wstring padRight(const std::wstring& s, size_t width);

// ATA/SCSI identification strings arrive space padded and byte-swapped per 16-bit word.
std::wstring decodeAtaString(const uint16_t* words, size_t wordCount);
std::wstring trimCopy(const std::wstring& s);

}  // namespace core
