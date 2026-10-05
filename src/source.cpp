#include "source.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <random>

#include "log.hpp"

namespace sentinel {
namespace {

constexpr std::int32_t kTempMinMc = 15000, kTempMaxMc = 35000;
constexpr std::int32_t kHumMinMpct = 20000, kHumMaxMpct = 80000;

bool period_ok(std::uint32_t ms) { return ms >= SENTINEL_PERIOD_MIN_MS && ms <= SENTINEL_PERIOD_MAX_MS; }

// ---------------------------------------------------------------------------
// SimulatedSource: random-walk temperature/humidity paced by a timerfd.
// Mirrors what the kernel driver does so the whole stack runs without hardware.
// ---------------------------------------------------------------------------
class SimulatedSource final : public SampleSource {
public:
    SimulatedSource(std::uint32_t period_ms, std::uint32_t seed) : period_ms_(period_ms), rng_(seed) {}

    std::string name() const override { return "simulated"; }

    void open() override {
        const std::uint32_t p = period_ms_.load();
        if (!period_ok(p)) throw std::invalid_argument("period out of range");
        tfd_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
        if (!tfd_.valid()) throw_errno("timerfd_create");
        arm(p);
    }

    bool next(Sample& out, int stop_fd) override {
        pollfd fds[2] = {{tfd_.get(), POLLIN, 0}, {stop_fd, POLLIN, 0}};  // negative fd is ignored by poll()
        for (;;) {
            fds[0].revents = fds[1].revents = 0;
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (fds[1].revents != 0) return false;  // stop request wins over data
            if (fds[0].revents & POLLIN) {
                std::uint64_t expirations = 0;
                const ssize_t n = ::read(tfd_.get(), &expirations, sizeof expirations);
                if (n != static_cast<ssize_t>(sizeof expirations)) {
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    return false;
                }
                fill(out, expirations);
                return true;
            }
        }
    }

    bool set_period_ms(std::uint32_t ms) override {
        if (!period_ok(ms)) return false;
        try {
            arm(ms);
        } catch (const std::exception&) {
            return false;
        }
        period_ms_.store(ms);
        return true;
    }
    std::uint32_t period_ms() const override { return period_ms_.load(); }

private:
    void arm(std::uint32_t ms) {
        itimerspec its{};
        its.it_interval.tv_sec = static_cast<time_t>(ms / 1000);
        its.it_interval.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
        its.it_value = its.it_interval;
        if (::timerfd_settime(tfd_.get(), 0, &its, nullptr) != 0) throw_errno("timerfd_settime");
    }

    // Runs on the producer thread only, so no locking is needed for the walk state.
    void fill(Sample& out, std::uint64_t expirations) {
        std::uniform_int_distribution<std::int32_t> dt(-120, 120), dh(-250, 250);
        temp_mc_ = std::clamp<std::int32_t>(temp_mc_ + dt(rng_), kTempMinMc, kTempMaxMc);
        hum_mpct_ = std::clamp<std::int32_t>(hum_mpct_ + dh(rng_), kHumMinMpct, kHumMaxMpct);
        out.ts_ns = mono_ns();
        out.temp_mc = temp_mc_;
        out.hum_mpct = hum_mpct_;
        out.seq = seq_;
        // If the timer fired more than once we were late: the skipped ticks are "lost"
        // samples, visible as a gap in `seq` and flagged just like the kernel driver does.
        out.flags = expirations > 1 ? SENTINEL_FLAG_OVERRUN : 0u;
        seq_ += static_cast<std::uint32_t>(expirations);
    }

    std::atomic<std::uint32_t> period_ms_;
    UniqueFd tfd_;
    std::mt19937 rng_;
    std::int32_t temp_mc_ = 22000;
    std::int32_t hum_mpct_ = 45000;
    std::uint32_t seq_ = 0;
};

// ---------------------------------------------------------------------------
// DeviceSource: reads struct sentinel_sample records from /dev/sentinel0.
// ---------------------------------------------------------------------------
class DeviceSource final : public SampleSource {
public:
    explicit DeviceSource(std::string path) : path_(std::move(path)) {}

    std::string name() const override { return "device:" + path_; }

    void open() override {
        const int fd = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            const int e = errno;
            throw std::system_error(e, std::generic_category(),
                                    "open " + path_ + " (is the sentinel_drv module loaded? see README)");
        }
        fd_.reset(fd);
        sentinel_dev_stats st{};
        if (::ioctl(fd_.get(), SENTINEL_IOC_GET_STATS, &st) == 0) period_ms_.store(st.period_ms);
    }

    bool next(Sample& out, int stop_fd) override {
        for (;;) {
            if (pos_ < count_) {
                out = batch_[pos_++];
                return true;
            }
            pollfd fds[2] = {{fd_.get(), POLLIN, 0}, {stop_fd, POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (fds[1].revents != 0) return false;
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                SLOG_ERROR("device %s reported an error condition", path_.c_str());
                return false;
            }
            if (fds[0].revents & POLLIN) {
                const ssize_t n = ::read(fd_.get(), batch_.data(), sizeof(Sample) * batch_.size());
                if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR) continue;
                    SLOG_ERROR("read(%s): %s", path_.c_str(), std::strerror(errno));
                    return false;
                }
                if (n == 0 || static_cast<std::size_t>(n) % sizeof(Sample) != 0) return false;
                count_ = static_cast<std::size_t>(n) / sizeof(Sample);
                pos_ = 0;
            }
        }
    }

    bool set_period_ms(std::uint32_t ms) override {
        if (!period_ok(ms)) return false;
        std::uint32_t v = ms;
        if (::ioctl(fd_.get(), SENTINEL_IOC_SET_PERIOD, &v) != 0) return false;
        period_ms_.store(ms);
        return true;
    }
    std::uint32_t period_ms() const override { return period_ms_.load(); }

private:
    std::string path_;
    UniqueFd fd_;
    std::atomic<std::uint32_t> period_ms_{0};
    std::array<Sample, 16> batch_{};
    std::size_t pos_ = 0, count_ = 0;
};

}  // namespace

std::unique_ptr<SampleSource> make_simulated_source(std::uint32_t period_ms, std::uint32_t seed) {
    return std::make_unique<SimulatedSource>(period_ms, seed);
}
std::unique_ptr<SampleSource> make_device_source(const std::string& device_path) {
    return std::make_unique<DeviceSource>(device_path);
}

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------
Producer::Producer(SampleSource& src, Sink sink) : src_(src), sink_(std::move(sink)) {
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) throw_errno("eventfd");
    stop_fd_.reset(fd);
}

Producer::~Producer() { stop(); }

void Producer::start() {
    th_ = std::thread([this] { run(); });
}

void Producer::stop() {
    if (!stop_requested_.exchange(true)) eventfd_signal(stop_fd_.get());
    if (th_.joinable()) th_.join();
}

void Producer::run() {
    Sample s{};
    try {
        while (src_.next(s, stop_fd_.get())) sink_(s);
    } catch (const std::exception& e) {
        SLOG_ERROR("producer: %s", e.what());
    }
    if (!stop_requested_.load()) {
        SLOG_ERROR("sample source '%s' terminated unexpectedly", src_.name().c_str());
        failed_.store(true, std::memory_order_release);
    }
}

}  // namespace sentinel
