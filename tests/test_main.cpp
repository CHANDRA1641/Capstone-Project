// Self-contained test runner (no third-party framework): unit tests + socket-level integration tests.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <future>
#include <sstream>
#include <thread>
#include <vector>

#include "containers.hpp"
#include "daemonize.hpp"
#include "log.hpp"
#include "protocol.hpp"
#include "sample.hpp"
#include "server.hpp"
#include "shm.hpp"
#include "source.hpp"
#include "store.hpp"
#include "thread_pool.hpp"
#include "util.hpp"

using namespace sentinel;

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------
namespace {

struct TestAbort {};
struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
struct Registrar {
    Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
int g_failed_checks = 0;

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

#define TEST(name)                                   \
    static void name();                              \
    static Registrar registrar_##name(#name, &name); \
    static void name()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ++g_failed_checks;                                                           \
            std::fprintf(stderr, "    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto va_ = (a);                                                                       \
        const auto vb_ = (b);                                                                       \
        if (!(va_ == vb_)) {                                                                        \
            ++g_failed_checks;                                                                      \
            std::fprintf(stderr, "    CHECK_EQ failed: %s == %s  (%s vs %s)  (%s:%d)\n", #a, #b,    \
                         show(va_).c_str(), show(vb_).c_str(), __FILE__, __LINE__);                 \
        }                                                                                           \
    } while (0)

#define REQUIRE(cond)                                                                      \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++g_failed_checks;                                                             \
            std::fprintf(stderr, "    REQUIRE failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            throw TestAbort{};                                                             \
        }                                                                                  \
    } while (0)

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }
std::size_t count_of(const std::string& s, const std::string& p) {
    std::size_t n = 0, pos = 0;
    while ((pos = s.find(p, pos)) != std::string::npos) {
        ++n;
        pos += p.size();
    }
    return n;
}
long seq_of(const std::string& line) {
    const auto p = line.find("\"seq\":");
    return p == std::string::npos ? -1 : std::strtol(line.c_str() + p + 6, nullptr, 10);
}

Sample make_sample(std::uint32_t seq, std::int32_t temp_mc, std::int32_t hum_mpct, std::uint64_t ts_ns = 0, std::uint32_t flags = 0) {
    Sample s{};
    s.seq = seq;
    s.temp_mc = temp_mc;
    s.hum_mpct = hum_mpct;
    s.ts_ns = ts_ns;
    s.flags = flags;
    return s;
}

// ---------------------------------------------------------------------------
// Test client
// ---------------------------------------------------------------------------
class Client {
public:
    explicit Client(std::uint16_t port, int timeout_ms = 3000) {
        fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        REQUIRE(fd_.valid());
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        REQUIRE(::connect(fd_.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    void send(const std::string& s) {
        std::size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = ::send(fd_.get(), s.data() + off, s.size() - off, MSG_NOSIGNAL);
            REQUIRE(n > 0);
            off += static_cast<std::size_t>(n);
        }
    }
    bool read_line(std::string& line) {
        for (;;) {
            const auto nl = buf_.find('\n');
            if (nl != std::string::npos) {
                line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                return true;
            }
            char tmp[4096];
            const ssize_t n = ::recv(fd_.get(), tmp, sizeof tmp, 0);
            if (n > 0) {
                buf_.append(tmp, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
    }
    std::string line() {
        std::string l;
        return read_line(l) ? l : std::string("<no reply>");
    }
    std::string ask(const std::string& cmd) {
        send(cmd + "\n");
        return line();
    }
    // True when the server closed the connection (after we drained pending data).
    bool at_eof() {
        char tmp[4096];
        for (;;) {
            const ssize_t n = ::recv(fd_.get(), tmp, sizeof tmp, 0);
            if (n == 0) return true;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return false;
        }
    }
    void half_close() { ::shutdown(fd_.get(), SHUT_WR); }
    void reset_close() {  // abortive close (RST)
        linger lg{1, 0};
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        fd_.reset();
    }

private:
    UniqueFd fd_;
    std::string buf_;
};

// Full pipeline on an ephemeral port: simulator -> store -> server.
struct Fixture {
    SampleStore store{512};
    std::unique_ptr<SampleSource> src;
    std::unique_ptr<Server> server;
    std::unique_ptr<Producer> producer;
    std::thread net;

    explicit Fixture(std::uint32_t period_ms = 10, ServerConfig cfg = ServerConfig{}) {
        cfg.bind_addr = "127.0.0.1";
        cfg.port = 0;
        src = make_simulated_source(period_ms, 1234);
        src->open();
        server = std::make_unique<Server>(cfg, store, *src);
        server->start_listening();
        producer = std::make_unique<Producer>(*src, [this](const Sample& s) {
            store.push(s);
            server->publish(s);
        });
        net = std::thread([this] { server->run(); });
        producer->start();
    }
    ~Fixture() {
        server->stop();
        producer->stop();
        net.join();
    }
    std::uint16_t port() const { return server->port(); }
    bool wait_samples(std::size_t n) { return wait_until([&] { return store.size() >= n; }, 3000); }
};

}  // namespace

// ===========================================================================
// Unit tests
// ===========================================================================
TEST(ring_buffer_overwrites_oldest) {
    RingBuffer<int> rb(4);
    CHECK(rb.empty());
    CHECK(!rb.latest().has_value());
    for (int i = 1; i <= 6; ++i) rb.push(i);
    CHECK_EQ(rb.size(), std::size_t{4});
    CHECK_EQ(rb.overwritten(), std::uint64_t{2});
    CHECK_EQ(rb.latest().value(), 6);
    CHECK((rb.last(0) == std::vector<int>{3, 4, 5, 6}));
    CHECK((rb.last(2) == std::vector<int>{5, 6}));
    CHECK((rb.last(100) == std::vector<int>{3, 4, 5, 6}));
    rb.clear();
    CHECK(rb.empty());
    bool threw = false;
    try {
        RingBuffer<int> bad(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(bounded_queue_drops_when_full) {
    BoundedQueue<int> q(3);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(q.try_push(3));
    CHECK(!q.try_push(4));
    CHECK_EQ(q.dropped(), std::uint64_t{1});
    std::vector<int> out;
    CHECK_EQ(q.drain(out), std::size_t{3});
    CHECK((out == std::vector<int>{1, 2, 3}));
    CHECK_EQ(q.size(), std::size_t{0});
    CHECK(q.try_push(5));
}

TEST(thread_pool_runs_every_task_then_rejects) {
    std::atomic<int> counter{0};
    ThreadPool pool(4, 1000);
    for (int i = 0; i < 500; ++i) CHECK(pool.submit([&] { counter.fetch_add(1); }));
    pool.shutdown();  // drains the queue before joining
    CHECK_EQ(counter.load(), 500);
    CHECK(!pool.submit([] {}));
    pool.shutdown();  // idempotent
}

TEST(thread_pool_bounded_queue_and_exceptions) {
    std::promise<void> gate;
    std::shared_future<void> gate_f = gate.get_future().share();
    std::atomic<bool> started{false};
    std::atomic<int> done{0};
    {
        ThreadPool pool(1, 2);
        CHECK(pool.submit([&, gate_f] {
            started = true;
            gate_f.wait();
        }));
        REQUIRE(wait_until([&] { return started.load(); }, 2000));
        CHECK(pool.submit([&] { done++; }));
        CHECK(pool.submit([&] {
            done++;
            throw std::runtime_error("boom (expected in this test)");
        }));
        CHECK(!pool.submit([&] { done++; }));  // queue full -> rejected, not blocked
        gate.set_value();
        pool.shutdown();
    }
    CHECK_EQ(done.load(), 2);  // the throwing task must not kill the worker
}

TEST(protocol_accepts_valid_commands) {
    auto ok = [](const char* line, Cmd cmd, bool has_arg = false, std::uint32_t arg = 0) {
        const ParseResult r = parse_request(line);
        CHECK(r.ok);
        CHECK(r.req.cmd == cmd);
        CHECK_EQ(r.req.has_arg, has_arg);
        CHECK_EQ(r.req.arg, arg);
    };
    ok("PING", Cmd::Ping);
    ok("  ping  ", Cmd::Ping);
    ok("Latest", Cmd::Latest);
    ok("INFO", Cmd::Info);
    ok("subscribe", Cmd::Subscribe);
    ok("UNSUBSCRIBE", Cmd::Unsubscribe);
    ok("quit", Cmd::Quit);
    ok("STATS", Cmd::Stats);
    ok("STATS 25", Cmd::Stats, true, 25);
    ok("HISTORY 1000", Cmd::History, true, 1000);
    ok("set period 250", Cmd::SetPeriod, true, 250);
    ok("SET PERIOD 10", Cmd::SetPeriod, true, 10);
    ok("SET PERIOD 10000", Cmd::SetPeriod, true, 10000);
}

TEST(protocol_rejects_malformed_input) {
    auto bad = [](const char* line, const char* code) {
        const ParseResult r = parse_request(line);
        CHECK(!r.ok);
        CHECK_EQ(r.code, std::string(code));
        CHECK(!r.message.empty());
    };
    bad("", "BAD_COMMAND");
    bad("   ", "BAD_COMMAND");
    bad("FOO", "BAD_COMMAND");
    bad("PING extra", "BAD_ARGUMENT");
    bad("HISTORY", "BAD_ARGUMENT");
    bad("HISTORY 0", "BAD_ARGUMENT");
    bad("HISTORY 1001", "BAD_ARGUMENT");
    bad("HISTORY -5", "BAD_ARGUMENT");
    bad("HISTORY 12abc", "BAD_ARGUMENT");
    bad("HISTORY 99999999999999999999", "BAD_ARGUMENT");
    bad("STATS 1 2", "BAD_ARGUMENT");
    bad("SET", "BAD_ARGUMENT");
    bad("SET PERIOD", "BAD_ARGUMENT");
    bad("SET PERIOD 9", "BAD_ARGUMENT");
    bad("SET PERIOD 10001", "BAD_ARGUMENT");
    bad("SET SPEED 100", "BAD_ARGUMENT");
    bad("SET PERIOD 1e3", "BAD_ARGUMENT");
}

TEST(stats_match_hand_computed_values) {
    std::vector<Sample> w;
    for (std::uint32_t i = 0; i < 5; ++i)
        w.push_back(make_sample(i, static_cast<std::int32_t>(20000 + 1000 * i), 50000, 1000000000ull + i * 100000000ull));
    const WindowStats s = compute_stats(w);
    CHECK_EQ(s.count, std::size_t{5});
    CHECK_EQ(s.lost, std::uint64_t{0});
    CHECK(std::fabs(s.temp_c.min - 20.0) < 1e-9);
    CHECK(std::fabs(s.temp_c.max - 24.0) < 1e-9);
    CHECK(std::fabs(s.temp_c.mean - 22.0) < 1e-9);
    CHECK(std::fabs(s.temp_c.median - 22.0) < 1e-9);
    CHECK(std::fabs(s.temp_c.stddev - std::sqrt(2.0)) < 1e-9);
    CHECK(std::fabs(s.hum_pct.stddev) < 1e-9);
    CHECK(std::fabs(s.span_s - 0.4) < 1e-9);
    CHECK(std::fabs(s.rate_hz - 10.0) < 1e-9);
}

TEST(stats_edge_cases) {
    CHECK_EQ(compute_stats({}).count, std::size_t{0});

    // even-sized window -> median is the mean of the two middle values
    CHECK(std::fabs(summarize({1, 2, 3, 4}).median - 2.5) < 1e-12);

    // gaps in sequence numbers are counted as lost samples
    const WindowStats gap = compute_stats({make_sample(0, 0, 0, 1), make_sample(1, 0, 0, 2), make_sample(5, 0, 0, 3)});
    CHECK_EQ(gap.lost, std::uint64_t{3});

    // 32-bit wrap-around must not look like a huge loss
    const WindowStats wrap = compute_stats({make_sample(0xFFFFFFFEu, 0, 0, 1), make_sample(0xFFFFFFFFu, 0, 0, 2),
                                            make_sample(0u, 0, 0, 3), make_sample(1u, 0, 0, 4)});
    CHECK_EQ(wrap.lost, std::uint64_t{0});

    const WindowStats ov = compute_stats({make_sample(0, 0, 0, 1, 0), make_sample(1, 0, 0, 2, SENTINEL_FLAG_OVERRUN)});
    CHECK_EQ(ov.overruns, std::uint64_t{1});
}

TEST(json_encoding_is_exact) {
    CHECK_EQ(to_json(make_sample(7, 21500, 40250, 123, 1)),
             std::string("{\"seq\":7,\"ts_ns\":123,\"temp_c\":21.500,\"hum_pct\":40.250,\"flags\":1}"));
    CHECK_EQ(json_escape("a\"b\\c\n"), std::string("a\\\"b\\\\c\\n"));
    CHECK_EQ(ok_reply("{}"), std::string("OK {}\n"));
    CHECK_EQ(err_reply("X", "why"), std::string("ERR X why\n"));
}

TEST(store_keeps_most_recent_window) {
    SampleStore st(3);
    CHECK(!st.latest().has_value());
    for (std::uint32_t i = 0; i < 5; ++i) st.push(make_sample(i, 0, 0));
    CHECK_EQ(st.size(), std::size_t{3});
    CHECK_EQ(st.total(), std::uint64_t{5});
    CHECK_EQ(st.latest()->seq, 4u);
    const auto v = st.last(0);
    REQUIRE(v.size() == 3);
    CHECK_EQ(v.front().seq, 2u);
}

TEST(simulated_source_produces_valid_monotonic_samples) {
    auto src = make_simulated_source(10, 42);
    src->open();
    UniqueFd stop(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    REQUIRE(stop.valid());

    Sample prev{};
    for (int i = 0; i < 6; ++i) {
        Sample s{};
        REQUIRE(src->next(s, stop.get()));
        CHECK(s.temp_mc >= 15000 && s.temp_mc <= 35000);
        CHECK(s.hum_mpct >= 20000 && s.hum_mpct <= 80000);
        if (i > 0) {
            CHECK(s.seq > prev.seq);
            CHECK(s.ts_ns > prev.ts_ns);
        }
        prev = s;
    }
    CHECK(!src->set_period_ms(5));
    CHECK(!src->set_period_ms(10001));
    CHECK(src->set_period_ms(20));
    CHECK_EQ(src->period_ms(), 20u);

    eventfd_signal(stop.get());
    Sample s{};
    CHECK(!src->next(s, stop.get()));  // a stop request interrupts the blocking wait
}

TEST(producer_delivers_and_stops_cleanly) {
    auto src = make_simulated_source(10, 7);
    src->open();
    std::atomic<int> n{0};
    Producer p(*src, [&](const Sample&) { n.fetch_add(1); });
    p.start();
    CHECK(wait_until([&] { return n.load() >= 5; }, 3000));
    p.stop();
    const int after = n.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(n.load(), after);  // nothing is delivered after stop()
    CHECK(!p.failed());
    p.stop();  // idempotent
}

TEST(device_source_reports_missing_device) {
    auto dev = make_device_source("/nonexistent/sentinel-test-device");
    bool threw = false;
    try {
        dev->open();
    } catch (const std::system_error& e) {
        threw = true;
        CHECK(contains(e.what(), "sentinel_drv"));  // actionable hint
    }
    CHECK(threw);
}

TEST(shm_roundtrip_and_error_paths) {
    const std::string name = "/sentinel_test_rt_" + std::to_string(::getpid());
    bool threw = false;
    try {
        ShmReader missing(name);
    } catch (const std::system_error&) {
        threw = true;
    }
    CHECK(threw);

    ShmPublisher pub(name);
    ShmReader reader(name);
    ShmSnapshot s{};
    CHECK(!reader.read(s));  // nothing published yet

    ShmSnapshot in{};
    in.pid = 4242;
    in.samples_total = 99;
    in.temp_avg_c = 21.5;
    in.has_latest = 1;
    in.latest_seq = 7;
    pub.publish(in);
    REQUIRE(reader.read(s));
    CHECK_EQ(s.pid, std::uint64_t{4242});
    CHECK_EQ(s.samples_total, std::uint64_t{99});
    CHECK(std::fabs(s.temp_avg_c - 21.5) < 1e-12);
    CHECK_EQ(s.latest_seq, std::uint64_t{7});
}

TEST(shm_seqlock_never_returns_torn_snapshots) {
    const std::string name = "/sentinel_test_torn_" + std::to_string(::getpid());
    ShmPublisher pub(name);
    ShmReader reader(name);
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        for (std::uint64_t k = 1; !stop.load(); ++k) {
            ShmSnapshot s{};
            s.pid = s.period_ms = s.samples_total = s.clients = s.subscribers = s.window_count = k;
            s.temp_min_c = s.temp_max_c = s.temp_avg_c = static_cast<double>(k);
            s.hum_min_pct = s.hum_max_pct = s.hum_avg_pct = static_cast<double>(k);
            s.updated_unix_ms = s.has_latest = s.latest_seq = s.latest_ts_ns = k;
            s.latest_temp_c = s.latest_hum_pct = static_cast<double>(k);
            pub.publish(s);
        }
    });

    int good = 0, torn = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        ShmSnapshot s{};
        if (!reader.read(s)) continue;
        const bool consistent = s.pid == s.samples_total && s.pid == s.latest_ts_ns && s.pid == s.window_count &&
                                s.temp_avg_c == static_cast<double>(s.pid) && s.latest_hum_pct == static_cast<double>(s.pid);
        consistent ? ++good : ++torn;
    }
    stop = true;
    writer.join();
    CHECK(good > 100);
    CHECK_EQ(torn, 0);
}

TEST(pidfile_enforces_single_instance) {
    const std::string path = "/tmp/sentinel_test_" + std::to_string(::getpid()) + ".pid";
    {
        PidFile first(path);
        first.write_pid();
        bool threw = false;
        try {
            PidFile second(path);
        } catch (const std::runtime_error& e) {
            threw = true;
            CHECK(contains(e.what(), "already running"));
        }
        CHECK(threw);
        FILE* f = std::fopen(path.c_str(), "r");
        REQUIRE(f != nullptr);
        int pid = 0;
        CHECK_EQ(std::fscanf(f, "%d", &pid), 1);
        std::fclose(f);
        CHECK_EQ(pid, static_cast<int>(::getpid()));
    }
    PidFile again(path);  // released and removed by the destructor above
}

// ===========================================================================
// Integration tests: real sockets against the real server
// ===========================================================================
TEST(server_answers_ping_and_info) {
    Fixture fx;
    Client c(fx.port());
    CHECK_EQ(c.ask("PING"), std::string("OK {\"pong\":true}"));
    const std::string info = c.ask("info");
    CHECK(starts_with(info, "OK {"));
    CHECK(contains(info, "\"source\":\"simulated\""));
    CHECK(contains(info, "\"clients\":1"));
    CHECK(contains(info, std::string("\"version\":\"") + kVersion + "\""));
}

TEST(server_serves_latest_stats_and_history) {
    Fixture fx;
    REQUIRE(fx.wait_samples(6));
    Client c(fx.port());

    const std::string latest = c.ask("LATEST");
    CHECK(starts_with(latest, "OK {\"seq\":"));
    CHECK(contains(latest, "\"temp_c\":"));

    const std::string stats = c.ask("STATS 5");
    CHECK(starts_with(stats, "OK {\"count\":5,"));
    CHECK(contains(stats, "\"temp_c\":{\"min\":"));
    CHECK(contains(stats, "\"hum_pct\":{"));

    const std::string hist = c.ask("HISTORY 3");
    CHECK(starts_with(hist, "OK {\"count\":3,\"samples\":["));
    CHECK_EQ(count_of(hist, "\"seq\":"), std::size_t{3});
}

TEST(server_reports_errors_and_applies_settings) {
    Fixture fx;
    Client c(fx.port());
    CHECK(starts_with(c.ask("FOO"), "ERR BAD_COMMAND"));
    CHECK(starts_with(c.ask("HISTORY 0"), "ERR BAD_ARGUMENT"));
    CHECK(starts_with(c.ask("SET PERIOD 1"), "ERR BAD_ARGUMENT"));
    CHECK_EQ(c.ask("SET PERIOD 50"), std::string("OK {\"period_ms\":50}"));
    CHECK(contains(c.ask("INFO"), "\"period_ms\":50"));
}

TEST(server_returns_no_data_before_first_sample) {
    Fixture fx(10000);  // first sample only after 10 s
    Client c(fx.port());
    CHECK(starts_with(c.ask("LATEST"), "ERR NO_DATA"));
    CHECK(starts_with(c.ask("STATS"), "ERR NO_DATA"));
    CHECK_EQ(c.ask("HISTORY 5"), std::string("OK {\"count\":0,\"samples\":[]}"));
}

TEST(server_keeps_pipelined_replies_in_order) {
    Fixture fx;
    REQUIRE(fx.wait_samples(3));
    Client c(fx.port());
    c.send("STATS\nPING\nSTATS 2\nPING\nHISTORY 2\nPING\n");  // slow (pool) and fast (inline) commands interleaved
    CHECK(starts_with(c.line(), "OK {\"count\":"));
    CHECK_EQ(c.line(), std::string("OK {\"pong\":true}"));
    CHECK(starts_with(c.line(), "OK {\"count\":2,"));
    CHECK_EQ(c.line(), std::string("OK {\"pong\":true}"));
    CHECK(starts_with(c.line(), "OK {\"count\":2,\"samples\""));
    CHECK_EQ(c.line(), std::string("OK {\"pong\":true}"));
}

TEST(server_streams_events_to_subscribers) {
    Fixture fx;
    Client c(fx.port());
    CHECK_EQ(c.ask("SUBSCRIBE"), std::string("OK {\"subscribed\":true}"));
    long prev = -1;
    for (int i = 0; i < 5; ++i) {
        const std::string l = c.line();
        REQUIRE(starts_with(l, "EVT {"));
        const long seq = seq_of(l);
        CHECK(seq > prev);
        prev = seq;
    }
    c.send("UNSUBSCRIBE\n");
    std::string l;
    bool got_ack = false;
    for (int i = 0; i < 50 && c.read_line(l); ++i) {  // in-flight EVT lines may precede the ack
        if (starts_with(l, "OK")) {
            got_ack = (l == "OK {\"subscribed\":false}");
            break;
        }
    }
    CHECK(got_ack);
    CHECK(wait_until([&] { return fx.server->stats().subscribers == 0; }, 1000));
}

TEST(server_answers_clients_that_half_close) {
    Fixture fx;
    Client c(fx.port());
    c.send("PING\nPING");  // last command has no newline, then FIN (like `printf PING | nc`)
    c.half_close();
    CHECK_EQ(c.line(), std::string("OK {\"pong\":true}"));
    CHECK_EQ(c.line(), std::string("OK {\"pong\":true}"));
    CHECK(c.at_eof());
}

TEST(server_quit_closes_connection) {
    Fixture fx;
    Client c(fx.port());
    CHECK_EQ(c.ask("QUIT"), std::string("OK {\"bye\":true}"));
    CHECK(c.at_eof());
}

TEST(server_rejects_oversized_request_lines) {
    Fixture fx;
    Client c(fx.port());
    c.send(std::string(2000, 'A'));  // no newline: an unterminated flood
    CHECK(starts_with(c.line(), "ERR TOO_LONG"));
    CHECK(c.at_eof());
    Client ok(fx.port());  // the server itself is unaffected
    CHECK_EQ(ok.ask("PING"), std::string("OK {\"pong\":true}"));
}

TEST(server_rejects_pipelining_floods) {
    ServerConfig cfg;
    cfg.max_pending_lines = 8;
    Fixture fx(10, cfg);
    Client c(fx.port());
    std::string flood;
    for (int i = 0; i < 200; ++i) flood += "HISTORY 1\n";
    c.send(flood);
    std::string l;
    bool saw_reject = false;
    while (c.read_line(l))
        if (starts_with(l, "ERR TOO_MANY")) saw_reject = true;
    CHECK(saw_reject);
}

TEST(server_enforces_client_limit) {
    ServerConfig cfg;
    cfg.max_clients = 2;
    Fixture fx(10, cfg);
    Client a(fx.port()), b(fx.port());
    CHECK_EQ(a.ask("PING"), std::string("OK {\"pong\":true}"));
    CHECK_EQ(b.ask("PING"), std::string("OK {\"pong\":true}"));
    Client third(fx.port());
    CHECK(third.at_eof());  // accepted by the kernel, then closed by the server
    CHECK(wait_until([&] { return fx.server->stats().rejected == 1; }, 1000));
    CHECK_EQ(a.ask("PING"), std::string("OK {\"pong\":true}"));  // existing clients keep working
}

TEST(server_disconnects_slow_consumers) {
    ServerConfig cfg;
    cfg.max_out_bytes = 1000;  // a 1000-sample HISTORY reply is ~90 KB
    Fixture fx(10, cfg);
    REQUIRE(fx.wait_samples(50));
    Client c(fx.port());
    c.send("HISTORY 50\n");
    CHECK(c.at_eof());
    CHECK(wait_until([&] { return fx.server->stats().slow_disconnects == 1; }, 1000));
}

TEST(server_closes_idle_clients_but_keeps_subscribers) {
    ServerConfig cfg;
    cfg.idle_timeout_s = 1;
    Fixture fx(10, cfg);
    Client idle(fx.port(), 5000);
    Client sub(fx.port(), 5000);
    CHECK_EQ(sub.ask("SUBSCRIBE"), std::string("OK {\"subscribed\":true}"));
    CHECK(idle.at_eof());  // dropped by the idle sweep
    std::string l = sub.line();
    CHECK(starts_with(l, "EVT {"));  // still streaming
}

TEST(server_survives_abrupt_disconnects) {
    Fixture fx;
    for (int i = 0; i < 5; ++i) {
        Client c(fx.port());
        CHECK(starts_with(c.ask("SUBSCRIBE"), "OK"));
        c.reset_close();  // RST while samples are flowing
    }
    CHECK(wait_until([&] { return fx.server->stats().clients == 0; }, 2000));
    Client c(fx.port());
    CHECK_EQ(c.ask("PING"), std::string("OK {\"pong\":true}"));
    CHECK_EQ(fx.server->stats().subscribers, std::size_t{0});
}

TEST(server_handles_many_concurrent_clients) {
    Fixture fx;
    REQUIRE(fx.wait_samples(5));
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 16; ++t) {
        ts.emplace_back([&] {
            try {
                Client c(fx.port(), 5000);
                for (int i = 0; i < 25; ++i) {
                    if (c.ask("PING") != "OK {\"pong\":true}") ++failures;
                    if (!starts_with(c.ask("STATS 5"), "OK {\"count\":")) ++failures;
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK_EQ(failures.load(), 0);
    CHECK(fx.server->stats().requests >= 16u * 50u);
}

// ===========================================================================
int main(int argc, char** argv) {
    Logger::instance().set_level(LogLevel::Warn);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;
    for (const Case& tc : registry()) {
        if (filter && !contains(tc.name, filter)) continue;
        ++run;
        const int before = g_failed_checks;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            tc.fn();
        } catch (const TestAbort&) {
        } catch (const std::exception& e) {
            ++g_failed_checks;
            std::fprintf(stderr, "    unexpected exception: %s\n", e.what());
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const bool ok = g_failed_checks == before;
        if (!ok) ++failed;
        std::printf("[%s] %-52s %5lld ms\n", ok ? " OK " : "FAIL", tc.name, static_cast<long long>(ms));
        std::fflush(stdout);
    }
    std::printf("\n%d tests, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
