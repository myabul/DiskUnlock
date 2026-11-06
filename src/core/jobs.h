#pragma once
#include "../pch.h"
#include "log.h"
#include <functional>
#include <condition_variable>
#include <thread>

namespace core {

// Cooperative cancellation token shared by a running job and the UI.
class CancelToken {
public:
    void cancel() { cancelled_.store(true); }
    bool cancelled() const { return cancelled_.load(); }
    void reset() { cancelled_.store(false); }
private:
    std::atomic<bool> cancelled_{false};
};

// Serial job queue. Storage commands must never overlap: two concurrent wipe/scan jobs on
// one device corrupt each other's results, so exactly one job runs at a time.
class JobQueue {
public:
    using Job = std::function<void(CancelToken&)>;

    JobQueue();
    ~JobQueue();

    // Returns false if a job is already running; the UI keeps one action in flight.
    bool post(std::wstring name, Job job);
    bool busy() const { return busy_.load(); }
    std::wstring currentJobName() const;
    void requestCancel();

    // Called by the UI timer so workers can push progress without touching HWNDs.
    CancelToken& activeToken() { return token_; }

private:
    void workerMain();

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::wstring, Job>> pending_;
    std::wstring runningName_;
    std::atomic<bool> busy_{false};
    bool stop_ = false;
    CancelToken token_;
};

JobQueue& Jobs();

}  // namespace core
