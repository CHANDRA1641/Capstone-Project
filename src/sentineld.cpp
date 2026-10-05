// sentineld - telemetry daemon: sensor source -> history -> TCP server + shared-memory stats.
#include <getopt.h>
#include <poll.h>
#include <sys/signalfd.h>

#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "daemonize.hpp"
#include "log.hpp"
#include "sample.hpp"
#include "server.hpp"
#include "shm.hpp"
#include "source.hpp"
#include "store.hpp"
#include "util.hpp"

using namespace sentinel;

namespace {

struct Options {
    std::string source = "sim";
    std::string device = "/dev/sentinel0";
    std::uint32_t period_ms = 100;
    std::string listen = "127.0.0.1:9090";
    std::size_t history = 4096;
    std::size_t workers = 0;  // 0 = auto
    bool daemon = false;
    std::string pidfile;
    std::string log_file;
    bool verbose = false;
    std::string shm = "/sentinel_stats";
    std::uint32_t seed = 0xC0FFEEu;
};

void usage(std::FILE* out, const char* argv0) {
    std::fprintf(out,
        "sentineld %s - sensor telemetry daemon\n\n"
        "Usage: %s [options]\n\n"
        "  -s, --source sim|device   sample source (default: sim)\n"
        "  -D, --device PATH         kernel device node for --source device (default: /dev/sentinel0)\n"
        "  -p, --period MS           sampling period, %u..%u ms (default: 100)\n"
        "  -l, --listen HOST:PORT    TCP listen address, IPv4 or [IPv6] (default: 127.0.0.1:9090)\n"
        "  -H, --history N           samples kept in memory (default: 4096)\n"
        "  -w, --workers N           worker threads (default: number of CPUs, min 2)\n"
        "  -S, --shm NAME            POSIX shared-memory stats segment, '' to disable (default: /sentinel_stats)\n"
        "  -d, --daemon              run in the background\n"
        "  -P, --pidfile PATH        write a pidfile and refuse to start twice\n"
        "  -L, --log-file PATH       append logs to PATH (default: stderr; syslog when --daemon)\n"
        "  -v, --verbose             debug logging\n"
        "  -V, --version             print version\n"
        "  -h, --help                this text\n\n"
        "Signals: SIGINT/SIGTERM shut down cleanly, SIGHUP reopens the log file, SIGUSR1 logs statistics.\n",
        kVersion, argv0, kMinPeriodMs, kMaxPeriodMs);
}

bool parse_uint(const char* s, std::uint64_t lo, std::uint64_t hi, std::uint64_t& out) {
    std::uint64_t v = 0;
    const char* e = s + std::strlen(s);
    const auto [p, ec] = std::from_chars(s, e, v);
    if (ec != std::errc() || p != e || v < lo || v > hi) return false;
    out = v;
    return true;
}

// Returns false when the caller should exit immediately with `rc`.
bool parse_args(int argc, char** argv, Options& o, int& rc) {
    static const option longopts[] = {
        {"source", required_argument, nullptr, 's'}, {"device", required_argument, nullptr, 'D'},
        {"period", required_argument, nullptr, 'p'}, {"listen", required_argument, nullptr, 'l'},
        {"history", required_argument, nullptr, 'H'}, {"workers", required_argument, nullptr, 'w'},
        {"shm", required_argument, nullptr, 'S'},    {"daemon", no_argument, nullptr, 'd'},
        {"pidfile", required_argument, nullptr, 'P'}, {"log-file", required_argument, nullptr, 'L'},
        {"verbose", no_argument, nullptr, 'v'},      {"version", no_argument, nullptr, 'V'},
        {"help", no_argument, nullptr, 'h'},         {nullptr, 0, nullptr, 0}};
    int c;
    std::uint64_t n = 0;
    while ((c = ::getopt_long(argc, argv, "s:D:p:l:H:w:S:dP:L:vVh", longopts, nullptr)) != -1) {
        switch (c) {
            case 's':
                o.source = optarg;
                if (o.source != "sim" && o.source != "device") {
                    std::fprintf(stderr, "--source must be 'sim' or 'device'\n");
                    rc = 1;
                    return false;
                }
                break;
            case 'D': o.device = optarg; break;
            case 'p':
                if (!parse_uint(optarg, kMinPeriodMs, kMaxPeriodMs, n)) {
                    std::fprintf(stderr, "--period must be in [%u,%u]\n", kMinPeriodMs, kMaxPeriodMs);
                    rc = 1;
                    return false;
                }
                o.period_ms = static_cast<std::uint32_t>(n);
                break;
            case 'l': o.listen = optarg; break;
            case 'H':
                if (!parse_uint(optarg, 16, 10'000'000, n)) {
                    std::fprintf(stderr, "--history must be in [16,10000000]\n");
                    rc = 1;
                    return false;
                }
                o.history = static_cast<std::size_t>(n);
                break;
            case 'w':
                if (!parse_uint(optarg, 1, 256, n)) {
                    std::fprintf(stderr, "--workers must be in [1,256]\n");
                    rc = 1;
                    return false;
                }
                o.workers = static_cast<std::size_t>(n);
                break;
            case 'S': o.shm = optarg; break;
            case 'd': o.daemon = true; break;
            case 'P': o.pidfile = optarg; break;
            case 'L': o.log_file = optarg; break;
            case 'v': o.verbose = true; break;
            case 'V':
                std::printf("sentineld %s\n", kVersion);
                rc = 0;
                return false;
            case 'h':
                usage(stdout, argv[0]);
                rc = 0;
                return false;
            default:
                usage(stderr, argv[0]);
                rc = 1;
                return false;
        }
    }
    if (optind != argc) {
        usage(stderr, argv[0]);
        rc = 1;
        return false;
    }
    if (!o.shm.empty() && o.shm[0] != '/') {
        std::fprintf(stderr, "--shm name must start with '/'\n");
        rc = 1;
        return false;
    }
    return true;
}

// "127.0.0.1:9090", "[::1]:9090", "localhost" is rejected (numeric hosts only, no DNS at startup).
bool split_host_port(const std::string& s, std::string& host, std::uint16_t& port) {
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    host = s.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    else if (host.find(':') != std::string::npos) return false;  // bare IPv6 must be bracketed
    const std::string p = s.substr(colon + 1);
    std::uint64_t v = 0;
    if (!parse_uint(p.c_str(), 0, 65535, v)) return false;
    port = static_cast<std::uint16_t>(v);
    return !host.empty();
}

ShmSnapshot make_snapshot(const SampleStore& store, const SampleSource& src, const ServerStats& ss) {
    ShmSnapshot snap{};
    snap.updated_unix_ms = realtime_ms();
    snap.pid = static_cast<std::uint64_t>(::getpid());
    snap.period_ms = src.period_ms();
    snap.samples_total = store.total();
    snap.clients = ss.clients;
    snap.subscribers = ss.subscribers;
    const auto window = store.last(100);
    if (!window.empty()) {
        const WindowStats ws = compute_stats(window);
        snap.window_count = ws.count;
        snap.temp_min_c = ws.temp_c.min;
        snap.temp_max_c = ws.temp_c.max;
        snap.temp_avg_c = ws.temp_c.mean;
        snap.hum_min_pct = ws.hum_pct.min;
        snap.hum_max_pct = ws.hum_pct.max;
        snap.hum_avg_pct = ws.hum_pct.mean;
        const Sample& l = window.back();
        snap.has_latest = 1;
        snap.latest_seq = l.seq;
        snap.latest_ts_ns = l.ts_ns;
        snap.latest_temp_c = sample_temp_c(l);
        snap.latest_hum_pct = sample_hum_pct(l);
    }
    return snap;
}

int run(const Options& opt) {
    std::string host;
    std::uint16_t port = 0;
    if (!split_host_port(opt.listen, host, port)) {
        SLOG_ERROR("invalid --listen '%s' (expected HOST:PORT with a numeric host)", opt.listen.c_str());
        return 1;
    }

    // ---- everything that can fail is done BEFORE daemonizing, so errors reach the terminal ----
    if (!opt.log_file.empty()) {
        if (!Logger::instance().open_file(opt.log_file)) {
            std::fprintf(stderr, "cannot open log file %s: %s\n", opt.log_file.c_str(), std::strerror(errno));
            return 1;
        }
    } else if (opt.daemon) {
        Logger::instance().enable_syslog("sentineld");
    }

    std::unique_ptr<PidFile> pidfile;
    if (!opt.pidfile.empty()) pidfile = std::make_unique<PidFile>(opt.pidfile);

    std::unique_ptr<SampleSource> source = opt.source == "device" ? make_device_source(opt.device)
                                                                  : make_simulated_source(opt.period_ms, opt.seed);
    source->open();
    if (opt.source == "device" && !source->set_period_ms(opt.period_ms))
        SLOG_WARN("could not set device period to %u ms; keeping %u ms", opt.period_ms, source->period_ms());

    SampleStore store(opt.history);

    ServerConfig sc;
    sc.bind_addr = host;
    sc.port = port;
    sc.worker_threads = opt.workers != 0 ? opt.workers : std::max<std::size_t>(2, std::thread::hardware_concurrency());
    Server server(sc, store, *source);
    server.start_listening();

    if (opt.daemon) daemonize();  // no threads exist yet: fork() is safe
    if (pidfile) pidfile->write_pid();

    // ---- signals: block them everywhere, consume them synchronously through a signalfd ----
    sigset_t mask;
    sigemptyset(&mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGUSR1}) sigaddset(&mask, sig);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) throw_errno("pthread_sigmask");
    UniqueFd sfd(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!sfd.valid()) throw_errno("signalfd");
    std::signal(SIGPIPE, SIG_IGN);

    std::unique_ptr<ShmPublisher> shm;
    if (!opt.shm.empty()) {
        try {
            shm = std::make_unique<ShmPublisher>(opt.shm);
        } catch (const std::exception& e) {
            SLOG_WARN("shared-memory export disabled: %s", e.what());
        }
    }

    Producer producer(*source, [&](const Sample& s) {
        store.push(s);
        server.publish(s);
    });

    std::atomic<bool> server_failed{false};
    std::thread net([&] {
        try {
            server.run();
        } catch (const std::exception& e) {
            SLOG_ERROR("server terminated: %s", e.what());
            server_failed.store(true);
        }
    });

    int exit_code = 0;
    try {
        producer.start();
        SLOG_INFO("sentineld %s running (pid %d, source=%s, period=%u ms, listen=%s:%u)", kVersion,
                  static_cast<int>(::getpid()), source->name().c_str(), source->period_ms(), host.c_str(),
                  static_cast<unsigned>(server.port()));

        bool running = true;
        while (running) {
            pollfd p{sfd.get(), POLLIN, 0};
            if (::poll(&p, 1, 1000) > 0) {
                signalfd_siginfo si{};
                while (::read(sfd.get(), &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
                    switch (si.ssi_signo) {
                        case SIGINT:
                        case SIGTERM:
                            SLOG_INFO("received %s, shutting down", si.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM");
                            running = false;
                            break;
                        case SIGHUP:
                            if (!Logger::instance().reopen()) SLOG_ERROR("log reopen failed");
                            else SLOG_INFO("log file reopened (SIGHUP)");
                            break;
                        case SIGUSR1: {
                            const ServerStats ss = server.stats();
                            SLOG_INFO("stats: samples=%llu clients=%zu subscribers=%zu accepted=%llu rejected=%llu "
                                      "requests=%llu slow_disconnects=%llu events_dropped=%llu",
                                      static_cast<unsigned long long>(store.total()), ss.clients, ss.subscribers,
                                      static_cast<unsigned long long>(ss.accepted),
                                      static_cast<unsigned long long>(ss.rejected),
                                      static_cast<unsigned long long>(ss.requests),
                                      static_cast<unsigned long long>(ss.slow_disconnects),
                                      static_cast<unsigned long long>(ss.events_dropped));
                            break;
                        }
                        default: break;
                    }
                }
            }
            if (producer.failed()) {
                SLOG_ERROR("sample source failed; exiting so the supervisor can restart us");
                exit_code = 2;
                running = false;
            }
            if (server_failed.load()) {
                exit_code = 2;
                running = false;
            }
            if (shm) shm->publish(make_snapshot(store, *source, server.stats()));
        }
    } catch (...) {
        server.stop();
        producer.stop();
        if (net.joinable()) net.join();
        throw;
    }

    server.stop();
    producer.stop();
    net.join();
    SLOG_INFO("sentineld stopped (total samples: %llu)", static_cast<unsigned long long>(store.total()));
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    int rc = 0;
    if (!parse_args(argc, argv, opt, rc)) return rc;
    Logger::instance().set_level(opt.verbose ? LogLevel::Debug : LogLevel::Info);
    try {
        return run(opt);
    } catch (const std::exception& e) {
        SLOG_ERROR("fatal: %s", e.what());
        return 2;
    }
}
