// sentinel-cli - command-line client for sentineld (TCP protocol and shared-memory stats).
#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "shm.hpp"
#include "util.hpp"

using namespace sentinel;

namespace {

void usage(std::FILE* out) {
    std::fprintf(out,
        "sentinel-cli %s\n\n"
        "Usage: sentinel-cli [-H host] [-p port] [-t seconds] <command> [args]\n\n"
        "Network commands (talk to sentineld over TCP):\n"
        "  ping                 liveness check\n"
        "  latest               most recent sample\n"
        "  stats [n]            min/max/mean/median/stddev over the last n samples (default: all retained)\n"
        "  history <n>          the last n samples (1..1000)\n"
        "  info                 daemon status\n"
        "  set-period <ms>      change the sampling period (10..10000)\n"
        "  watch [count]        stream live samples (forever, or `count` of them)\n"
        "  raw <text...>        send a raw protocol line\n\n"
        "Local command (no network, reads POSIX shared memory):\n"
        "  shm [name]           live dashboard numbers from /sentinel_stats\n\n"
        "Options: -H host (default 127.0.0.1)  -p port (default 9090)  -t timeout seconds (default 5)\n"
        "Exit status: 0 success, 1 server returned ERR, 2 connection/usage failure.\n",
        kVersion);
}

UniqueFd connect_to(const std::string& host, const std::string& port, int timeout_s) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) throw std::runtime_error("cannot resolve " + host + ": " + ::gai_strerror(rc));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(res, ::freeaddrinfo);

    int last_err = ECONNREFUSED;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol));
        if (!fd.valid()) continue;
        int r = ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            pollfd p{fd.get(), POLLOUT, 0};
            r = ::poll(&p, 1, timeout_s * 1000) == 1 ? 0 : -1;
            if (r == 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr != 0) {
                    r = -1;
                    errno = soerr;
                }
            } else {
                errno = ETIMEDOUT;
            }
        }
        if (r != 0) {
            last_err = errno;
            continue;
        }
        const int flags = ::fcntl(fd.get(), F_GETFL, 0);
        ::fcntl(fd.get(), F_SETFL, flags & ~O_NONBLOCK);
        timeval tv{timeout_s, 0};
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        return fd;
    }
    throw std::system_error(last_err, std::generic_category(), "cannot connect to " + host + ":" + port);
}

bool send_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_line(int fd, std::string& buf, std::string& line) {
    for (;;) {
        const std::size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            line = buf.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf.erase(0, nl + 1);
            return true;
        }
        char tmp[8192];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            if (buf.size() > (8u << 20)) return false;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;  // EOF, timeout or error
    }
}

int show_shm(const std::string& name) {
    ShmReader reader(name);
    ShmSnapshot s{};
    if (!reader.read(s)) {
        std::fprintf(stderr, "no data published yet (or writer is stalled)\n");
        return 2;
    }
    const std::uint64_t now = realtime_ms();
    const double age_s = now >= s.updated_unix_ms ? static_cast<double>(now - s.updated_unix_ms) / 1000.0 : 0.0;
    std::printf("daemon pid      : %llu%s\n", static_cast<unsigned long long>(s.pid), age_s > 5.0 ? "  (STALE: no update for >5s)" : "");
    std::printf("last update     : %.1f s ago\n", age_s);
    std::printf("sampling period : %llu ms\n", static_cast<unsigned long long>(s.period_ms));
    std::printf("samples total   : %llu\n", static_cast<unsigned long long>(s.samples_total));
    std::printf("clients         : %llu (%llu subscribed)\n", static_cast<unsigned long long>(s.clients), static_cast<unsigned long long>(s.subscribers));
    if (s.has_latest) {
        std::printf("latest          : seq=%llu temp=%.3f C humidity=%.3f %%\n", static_cast<unsigned long long>(s.latest_seq), s.latest_temp_c, s.latest_hum_pct);
        std::printf("window (%3llu)   : temp %.3f..%.3f (avg %.3f) C | humidity %.3f..%.3f (avg %.3f) %%\n", static_cast<unsigned long long>(s.window_count),
                    s.temp_min_c, s.temp_max_c, s.temp_avg_c, s.hum_min_pct, s.hum_max_pct, s.hum_avg_pct);
    }
    return 0;
}

bool parse_u32(const char* s, std::uint32_t& out) {
    const char* e = s + std::strlen(s);
    const auto [p, ec] = std::from_chars(s, e, out);
    return ec == std::errc() && p == e;
}

int run(int argc, char** argv) {
    std::string host = "127.0.0.1", port = "9090";
    int timeout_s = 5;
    int c;
    while ((c = ::getopt(argc, argv, "+H:p:t:h")) != -1) {
        switch (c) {
            case 'H': host = optarg; break;
            case 'p': port = optarg; break;
            case 't': timeout_s = std::max(1, std::atoi(optarg)); break;
            case 'h': usage(stdout); return 0;
            default: usage(stderr); return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }
    const std::string cmd = argv[optind++];
    const int nargs = argc - optind;
    char** args = argv + optind;

    if (cmd == "shm") return show_shm(nargs >= 1 ? args[0] : "/sentinel_stats");

    std::string request;
    std::uint32_t count = 0;
    bool watch = false;
    if (cmd == "ping" || cmd == "latest" || cmd == "info") {
        request = cmd;
    } else if (cmd == "stats") {
        request = nargs >= 1 ? std::string("STATS ") + args[0] : "STATS";
    } else if (cmd == "history") {
        if (nargs != 1) { usage(stderr); return 2; }
        request = std::string("HISTORY ") + args[0];
    } else if (cmd == "set-period") {
        if (nargs != 1) { usage(stderr); return 2; }
        request = std::string("SET PERIOD ") + args[0];
    } else if (cmd == "watch") {
        watch = true;
        if (nargs >= 1 && !parse_u32(args[0], count)) { usage(stderr); return 2; }
        request = "SUBSCRIBE";
    } else if (cmd == "raw") {
        if (nargs < 1) { usage(stderr); return 2; }
        for (int i = 0; i < nargs; ++i) request += (i ? " " : "") + std::string(args[i]);
    } else {
        usage(stderr);
        return 2;
    }

    const UniqueFd fd = connect_to(host, port, timeout_s);
    std::string buf, line;
    if (!send_all(fd.get(), request + "\n")) {
        std::fprintf(stderr, "send failed\n");
        return 2;
    }
    if (!read_line(fd.get(), buf, line)) {
        std::fprintf(stderr, "no reply from server (timeout or connection closed)\n");
        return 2;
    }
    std::printf("%s\n", line.c_str());
    const bool ok = line.rfind("OK", 0) == 0;
    if (!ok) return 1;

    if (watch) {
        std::fflush(stdout);
        std::uint32_t seen = 0;
        timeval none{0, 0};  // streaming: no receive timeout
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
        while (read_line(fd.get(), buf, line)) {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
            if (count != 0 && ++seen >= count) break;
        }
        (void)send_all(fd.get(), "QUIT\n");
    } else {
        (void)send_all(fd.get(), "QUIT\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sentinel-cli: %s\n", e.what());
        return 2;
    }
}
