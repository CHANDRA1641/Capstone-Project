// Generic containers: fixed-capacity ring buffer and a thread-safe bounded queue.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sentinel {

// Fixed-capacity circular buffer that overwrites the oldest element when full.
// NOT thread-safe: wrap it (see SampleStore) when shared.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : buf_(validate(capacity)) {}

    std::size_t capacity() const noexcept { return buf_.size(); }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::uint64_t overwritten() const noexcept { return overwritten_; }

    void push(const T& v) {
        buf_[head_] = v;
        head_ = (head_ + 1) % buf_.size();
        if (size_ == buf_.size()) ++overwritten_;
        else ++size_;
    }

    std::optional<T> latest() const {
        if (size_ == 0) return std::nullopt;
        return buf_[(head_ + buf_.size() - 1) % buf_.size()];
    }

    // The n most recent items, oldest first. n == 0 or n > size() means "everything".
    std::vector<T> last(std::size_t n) const {
        if (n == 0 || n > size_) n = size_;
        std::vector<T> out;
        out.reserve(n);
        const std::size_t start = (head_ + buf_.size() - n) % buf_.size();
        for (std::size_t i = 0; i < n; ++i) out.push_back(buf_[(start + i) % buf_.size()]);
        return out;
    }

    void clear() noexcept {
        head_ = 0;
        size_ = 0;
    }

private:
    static std::size_t validate(std::size_t c) {
        if (c == 0) throw std::invalid_argument("RingBuffer capacity must be > 0");
        return c;
    }
    std::vector<T> buf_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t overwritten_ = 0;
};

// Multi-producer queue with a hard capacity. Producers never block: when full the
// item is dropped and counted (back-pressure by design, protects real-time paths).
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : cap_(capacity) {}

    bool try_push(T v) {
        std::lock_guard<std::mutex> g(mu_);
        if (q_.size() >= cap_) {
            ++dropped_;
            return false;
        }
        q_.push_back(std::move(v));
        return true;
    }

    // Move everything currently queued into `out` (appended). Returns the count moved.
    std::size_t drain(std::vector<T>& out) {
        std::lock_guard<std::mutex> g(mu_);
        const std::size_t n = q_.size();
        out.reserve(out.size() + n);
        for (auto& v : q_) out.push_back(std::move(v));
        q_.clear();
        return n;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return q_.size();
    }
    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> g(mu_);
        return dropped_;
    }

private:
    mutable std::mutex mu_;
    std::deque<T> q_;
    const std::size_t cap_;
    std::uint64_t dropped_ = 0;
};

}  // namespace sentinel
