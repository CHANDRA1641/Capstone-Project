// Thread-safe rolling history of recent samples.
#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "containers.hpp"
#include "sample.hpp"

namespace sentinel {

class SampleStore {
public:
    explicit SampleStore(std::size_t capacity) : ring_(capacity) {}

    void push(const Sample& s) {
        std::lock_guard<std::mutex> g(mu_);
        ring_.push(s);
        ++total_;
    }
    std::optional<Sample> latest() const {
        std::lock_guard<std::mutex> g(mu_);
        return ring_.latest();
    }
    // n most-recent samples, oldest first; n == 0 means "all retained".
    std::vector<Sample> last(std::size_t n) const {
        std::lock_guard<std::mutex> g(mu_);
        return ring_.last(n);
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return ring_.size();
    }
    std::size_t capacity() const {
        std::lock_guard<std::mutex> g(mu_);
        return ring_.capacity();
    }
    std::uint64_t total() const {
        std::lock_guard<std::mutex> g(mu_);
        return total_;
    }

private:
    mutable std::mutex mu_;
    RingBuffer<Sample> ring_;
    std::uint64_t total_ = 0;
};

}  // namespace sentinel
