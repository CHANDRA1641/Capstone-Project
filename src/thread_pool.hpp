// Fixed-size worker pool with a bounded task queue (rejects instead of growing without limit).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "log.hpp"

namespace sentinel {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads, std::size_t max_queue = 1024) : max_queue_(max_queue) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { worker_loop(); });
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ~ThreadPool() { shutdown(); }

    // Returns false (task NOT queued) when the pool is stopping or the queue is full.
    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_ || q_.size() >= max_queue_) return false;
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    // Finish all queued tasks, then join the workers. Idempotent.
    void shutdown() {
        std::lock_guard<std::mutex> jg(join_mu_);
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> g(mu_);
        return q_.size();
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;  // stop_ is set and nothing is left to do
                task = std::move(q_.front());
                q_.pop_front();
            }
            try {
                task();
            } catch (const std::exception& e) {
                SLOG_ERROR("thread pool task threw: %s", e.what());
            } catch (...) {
                SLOG_ERROR("thread pool task threw an unknown exception");
            }
        }
    }

    mutable std::mutex mu_;
    std::mutex join_mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    const std::size_t max_queue_;
    bool stop_ = false;
};

}  // namespace sentinel
