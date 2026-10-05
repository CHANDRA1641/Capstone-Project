// Small, dependency-free helpers shared by every component.
#pragma once

#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <system_error>

namespace sentinel {

inline constexpr const char* kVersion = "1.0.0";

// RAII owner of a POSIX file descriptor (move-only).
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.release()) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~UniqueFd() { reset(); }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);  // never retry close() on Linux
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

[[noreturn]] inline void throw_errno(const std::string& what, int err = errno) {
    throw std::system_error(err, std::generic_category(), what);
}

inline std::uint64_t mono_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
inline std::uint64_t mono_ms() noexcept { return mono_ns() / 1000000ull; }
inline std::uint64_t realtime_ms() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// Wake a poller blocked on an eventfd / drain the counter afterwards.
inline void eventfd_signal(int fd) noexcept {
    const std::uint64_t one = 1;
    ssize_t r;
    do {
        r = ::write(fd, &one, sizeof one);
    } while (r < 0 && errno == EINTR);
    (void)r;  // EAGAIN means the counter is saturated: the reader is already due to wake.
}
inline void eventfd_drain(int fd) noexcept {
    std::uint64_t v;
    ssize_t r;
    do {
        r = ::read(fd, &v, sizeof v);
    } while (r < 0 && errno == EINTR);
    (void)r;
}

// printf-style formatting into a std::string.
inline std::string strfmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline std::string strfmt(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s;
    if (n > 0) {
        s.resize(static_cast<std::size_t>(n));
        std::vsnprintf(s.data(), static_cast<std::size_t>(n) + 1, fmt, ap2);
    }
    va_end(ap2);
    return s;
}

inline std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += strfmt("\\u%04x", c);
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

}  // namespace sentinel
