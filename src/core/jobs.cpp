#include "jobs.h"

namespace core {

JobQueue::JobQueue() { thread_ = std::thread(&JobQueue::workerMain, this); }

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool JobQueue::post(std::wstring name, Job job) {
    if (busy_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return false;
        pending_.emplace_back(std::move(name), std::move(job));
    }
    cv_.notify_one();
    return true;
}

std::wstring JobQueue::currentJobName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runningName_;
}

void JobQueue::requestCancel() { token_.cancel(); }

void JobQueue::workerMain() {
    for (;;) {
        std::pair<std::wstring, Job> item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !pending_.empty(); });
            if (stop_ && pending_.empty()) return;
            item = std::move(pending_.front());
            pending_.pop_front();
            runningName_ = item.first;
        }
        token_.reset();
        busy_.store(true);
        LogCmd(L"[JOB] Basladi: " + item.first);
        try {
            item.second(token_);
        } catch (const std::exception& e) {
            LogFail(L"[JOB] Istisna: " + core::widen(e.what()));
        } catch (...) {
            LogFail(L"[JOB] Bilinmeyen istisna.");
        }
        busy_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runningName_.clear();
        }
        LogInfo(token_.cancelled() ? L"[JOB] Iptal edildi: " + item.first
                                   : L"[JOB] Tamamlandi: " + item.first);
    }
}

JobQueue& Jobs() {
    static JobQueue instance;
    return instance;
}

}  // namespace core
