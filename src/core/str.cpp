#include "str.h"
#include <cwchar>
#include <cstring>

namespace core {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring(s.begin(), s.end());  // legacy byte strings: keep as-is
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], need);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], need, nullptr, nullptr);
    return out;
}

std::string narrowAnsi(const std::wstring& s) {
    if (s.empty()) return std::string();
    BOOL usedDefault = FALSE;
    int need = WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, &usedDefault);
    if (need <= 0) return std::string();
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), &out[0], need, nullptr, &usedDefault);
    out.resize(strlen(out.c_str()));
    return out;
}

std::wstring winErrorText(DWORD code) {
    if (code == ERROR_SUCCESS) return L"ERROR_SUCCESS";
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK;
    DWORD n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring text;
    if (n && buf) {
        text = buf;
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    }
    if (buf) LocalFree(buf);
    std::wstring hex = L"0x";
    wchar_t tmp[16];
    swprintf(tmp, 16, L"%08lX", (unsigned long)code);
    hex += tmp;
    return hex + (text.empty() ? L"" : L" - " + text);
}

std::wstring lastErrorText() { return winErrorText(GetLastError()); }

std::wstring formatBytes(uint64_t bytes) {
    const wchar_t* units[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    // %ls, not %s: this toolchain's wide printf family reads a bare %s argument as a *narrow*
    // string, so "GiB" would come out as "G" and every capacity in the UI would be one letter.
    if (u == 0) swprintf(buf, 64, L"%llu %ls", (unsigned long long)bytes, units[u]);
    else swprintf(buf, 64, L"%.2f %ls", v, units[u]);
    return buf;
}

std::wstring formatU64(uint64_t v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%llu", (unsigned long long)v);
    return buf;
}

std::wstring formatHex(uint64_t v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%llX", (unsigned long long)v);
    return buf;
}

std::wstring padRight(const std::wstring& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::wstring(width - s.size(), L' ');
}

std::wstring decodeAtaString(const uint16_t* words, size_t wordCount) {
    std::string s;
    s.reserve(wordCount * 2);
    for (size_t i = 0; i < wordCount; ++i) {
        s.push_back((char)(words[i] >> 8));
        s.push_back((char)(words[i] & 0xFF));
    }
    size_t begin = s.find_first_not_of(" \0");
    size_t end = s.find_last_not_of(" \0");
    if (begin == std::string::npos) return std::wstring();
    return widen(s.substr(begin, end - begin + 1));
}

std::wstring trimCopy(const std::wstring& s) {
    size_t begin = s.find_first_not_of(L" \t\r\n\0");
    if (begin == std::wstring::npos) return std::wstring();
    size_t end = s.find_last_not_of(L" \t\r\n\0");
    return s.substr(begin, end - begin + 1);
}

}  // namespace core
