// Thread-safe logger: stderr, append-only file (SIGHUP reopen for logrotate), or syslog.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unistd.h>

namespace sentinel {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel lv) noexcept { level_.store(static_cast<int>(lv), std::memory_order_relaxed); }
    LogLevel level() const noexcept { return static_cast<LogLevel>(level_.load(std::memory_order_relaxed)); }

    bool open_file(const std::string& path);  // false on failure (previous target stays active)
    bool reopen();                            // re-open the same file (log rotation)
    void enable_syslog(const char* ident);

    void log(LogLevel lv, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

private:
    Logger() = default;
    std::mutex mu_;
    int fd_ = STDERR_FILENO;
    bool use_syslog_ = false;
    std::string path_;
    std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
};

}  // namespace sentinel

#define SLOG_DEBUG(...) ::sentinel::Logger::instance().log(::sentinel::LogLevel::Debug, __VA_ARGS__)
#define SLOG_INFO(...) ::sentinel::Logger::instance().log(::sentinel::LogLevel::Info, __VA_ARGS__)
#define SLOG_WARN(...) ::sentinel::Logger::instance().log(::sentinel::LogLevel::Warn, __VA_ARGS__)
#define SLOG_ERROR(...) ::sentinel::Logger::instance().log(::sentinel::LogLevel::Error, __VA_ARGS__)
