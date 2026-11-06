#include "log.h"

namespace core {

void LogBuffer::write(LogLevel level, const std::wstring& text) {
    LogRecord rec;
    GetLocalTime(&rec.when);
    rec.level = level;
    rec.message = text;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(rec));
    ++total_;
}

std::vector<LogRecord> LogBuffer::drain(size_t maxEntries) {
    std::vector<LogRecord> out;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty() && out.size() < maxEntries) {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return out;
}

size_t LogBuffer::pendingEstimate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

LogBuffer& Log() {
    static LogBuffer instance;
    return instance;
}

}  // namespace core
