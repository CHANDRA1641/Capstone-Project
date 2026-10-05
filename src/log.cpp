#include "log.hpp"

#include <fcntl.h>
#include <syslog.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace sentinel {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

bool Logger::open_file(const std::string& path) {
    const int nfd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
    if (nfd < 0) return false;
    std::lock_guard<std::mutex> g(mu_);
    if (fd_ > STDERR_FILENO) ::close(fd_);
    fd_ = nfd;
    path_ = path;
    use_syslog_ = false;
    return true;
}

bool Logger::reopen() {
    std::string p;
    {
        std::lock_guard<std::mutex> g(mu_);
        p = path_;
    }
    return p.empty() ? true : open_file(p);
}

void Logger::enable_syslog(const char* ident) {
    std::lock_guard<std::mutex> g(mu_);
    ::openlog(ident, LOG_PID, LOG_DAEMON);
    use_syslog_ = true;
}

void Logger::log(LogLevel lv, const char* fmt, ...) {
    if (static_cast<int>(lv) < level_.load(std::memory_order_relaxed)) return;

    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    static const char* const names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    static const int prio[] = {LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR};
    const int idx = static_cast<int>(lv);

    std::lock_guard<std::mutex> g(mu_);
    if (use_syslog_) {
        ::syslog(prio[idx], "%s", msg);
        return;
    }
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    tm t{};
    ::gmtime_r(&ts.tv_sec, &t);
    char line[900];
    int n = std::snprintf(line, sizeof line, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %-5s %s\n", t.tm_year + 1900,
                          t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000L, names[idx], msg);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) >= sizeof line) {
        n = static_cast<int>(sizeof line) - 1;
        line[n - 1] = '\n';
    }
    const ssize_t r = ::write(fd_, line, static_cast<std::size_t>(n));
    (void)r;  // nothing sensible to do if logging itself fails
}

}  // namespace sentinel
