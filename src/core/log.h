#pragma once
#include "str.h"

namespace core {

enum class LogLevel : uint8_t { Info, Command, Ok, Warn, Fail };

struct LogRecord {
    SYSTEMTIME when{};
    LogLevel level = LogLevel::Info;
    std::wstring message;
};

// Thread-safe append from any worker; the UI thread drains it on a timer.
// Nothing here touches window handles, so a worker can never block on the GUI.
class LogBuffer {
public:
    void write(LogLevel level, const std::wstring& text);
    std::vector<LogRecord> drain(size_t maxEntries);
    size_t pendingEstimate() const;

private:
    mutable std::mutex mutex_;
    std::deque<LogRecord> queue_;
    size_t total_ = 0;
};

LogBuffer& Log();

inline void LogInfo(const std::wstring& m) { Log().write(LogLevel::Info, m); }
inline void LogCmd(const std::wstring& m)  { Log().write(LogLevel::Command, m); }
inline void LogOk(const std::wstring& m)   { Log().write(LogLevel::Ok, m); }
inline void LogWarn(const std::wstring& m) { Log().write(LogLevel::Warn, m); }
inline void LogFail(const std::wstring& m) { Log().write(LogLevel::Fail, m); }

}  // namespace core
