// Sample sources: a user-space simulator and the real kernel driver, behind one interface.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "sample.hpp"
#include "util.hpp"

namespace sentinel {

class SampleSource {
public:
    virtual ~SampleSource() = default;

    virtual std::string name() const = 0;

    // Acquire resources. Throws std::system_error / std::runtime_error on failure.
    virtual void open() = 0;

    // Block until one sample is available (returns true) or `stop_fd` becomes readable
    // / the source fails (returns false). `stop_fd` may be -1 to mean "never".
    virtual bool next(Sample& out, int stop_fd) = 0;

    // Thread-safe with respect to next(). Returns false if the value is rejected.
    virtual bool set_period_ms(std::uint32_t ms) = 0;
    virtual std::uint32_t period_ms() const = 0;
};

std::unique_ptr<SampleSource> make_simulated_source(std::uint32_t period_ms, std::uint32_t seed);
std::unique_ptr<SampleSource> make_device_source(const std::string& device_path);

// Runs a source on its own thread and forwards each sample to `sink`.
class Producer {
public:
    using Sink = std::function<void(const Sample&)>;

    Producer(SampleSource& src, Sink sink);
    ~Producer();
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    void start();
    void stop();  // idempotent; call from the owning thread only
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

private:
    void run();

    SampleSource& src_;
    Sink sink_;
    UniqueFd stop_fd_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> failed_{false};
    std::thread th_;
};

}  // namespace sentinel
