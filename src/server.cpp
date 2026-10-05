#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <cstring>
#include <stdexcept>

#include "log.hpp"

namespace sentinel {
namespace {

constexpr std::uint64_t kListenId = 0;
constexpr std::uint64_t kWakeId = 1;
constexpr std::uint64_t kFirstConnId = 2;

std::string format_peer(const sockaddr_storage& ss) {
    char host[INET6_ADDRSTRLEN] = "?";
    std::uint16_t port = 0;
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
        port = ntohs(a->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        ::inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
        port = ntohs(a->sin6_port);
    }
    return strfmt("%s:%u", host, static_cast<unsigned>(port));
}

}  // namespace

struct Server::Conn {
    std::uint64_t id = 0;
    UniqueFd fd;
    std::string peer;
    std::string in;                  // bytes received, not yet split into lines
    std::deque<std::string> pending; // complete request lines waiting to run
    std::string out;                 // bytes waiting to be sent
    std::size_t out_off = 0;
    bool busy = false;        // a worker owns the next reply
    bool subscribed = false;
    bool read_eof = false;    // peer shut down its write side
    bool closing = false;     // close once `out` is flushed
    std::uint32_t cur_events = 0;
    std::uint64_t last_active_ms = 0;
};

Server::Server(ServerConfig cfg, SampleStore& store, SampleSource& source)
    : cfg_(std::move(cfg)), store_(store), source_(source), next_id_(kFirstConnId), events_(cfg_.event_queue_depth) {}

Server::~Server() {
    stop();
    pool_.reset();
}

void Server::ep_add(int fd, std::uint64_t id, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = id;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) throw_errno("epoll_ctl(ADD)");
}

void Server::start_listening() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(cfg_.port);
    const int rc = ::getaddrinfo(cfg_.bind_addr.c_str(), port_s.c_str(), &hints, &res);
    if (rc != 0) throw std::runtime_error("invalid listen address '" + cfg_.bind_addr + "': " + ::gai_strerror(rc));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(res, ::freeaddrinfo);

    int last_err = EINVAL;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol));
        if (!fd.valid()) {
            last_err = errno;
            continue;
        }
        const int one = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ai->ai_family == AF_INET6) ::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
        if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(fd.get(), 128) != 0) {
            last_err = errno;
            continue;
        }
        listen_fd_ = std::move(fd);
        break;
    }
    if (!listen_fd_.valid()) throw_errno("cannot listen on " + cfg_.bind_addr + ":" + port_s, last_err);

    sockaddr_storage ss{};
    socklen_t sl = sizeof ss;
    if (::getsockname(listen_fd_.get(), reinterpret_cast<sockaddr*>(&ss), &sl) == 0) {
        bound_port_ = ntohs(ss.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                                     : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    }

    epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd_.valid()) throw_errno("epoll_create1");
    wake_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_fd_.valid()) throw_errno("eventfd");
    spare_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));  // reserve for EMFILE recovery
    ep_add(listen_fd_.get(), kListenId, EPOLLIN);
    ep_add(wake_fd_.get(), kWakeId, EPOLLIN);
    started_ms_ = mono_ms();
    SLOG_INFO("listening on %s:%u", cfg_.bind_addr.c_str(), static_cast<unsigned>(bound_port_));
}

void Server::stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if (wake_fd_.valid()) eventfd_signal(wake_fd_.get());
}

void Server::publish(const Sample& s) {
    if (n_subs_.load(std::memory_order_relaxed) == 0) return;
    events_.try_push(s);  // drops (and counts) if the loop cannot keep up
    if (wake_fd_.valid()) eventfd_signal(wake_fd_.get());
}

ServerStats Server::stats() const {
    ServerStats s;
    s.accepted = accepted_.load();
    s.rejected = rejected_.load();
    s.requests = requests_.load();
    s.slow_disconnects = slow_drops_.load();
    s.events_dropped = events_.dropped();
    s.clients = n_clients_.load();
    s.subscribers = n_subs_.load();
    return s;
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------
void Server::run() {
    if (!listen_fd_.valid()) throw std::logic_error("Server::start_listening() must be called before run()");
    pool_ = std::make_unique<ThreadPool>(cfg_.worker_threads, 256);  // created here: after any daemonize()

    std::vector<epoll_event> evs(128);
    std::uint64_t last_sweep = mono_ms();
    while (!stop_.load(std::memory_order_acquire)) {
        const int n = ::epoll_wait(epoll_fd_.get(), evs.data(), static_cast<int>(evs.size()), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            SLOG_ERROR("epoll_wait: %s", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            const std::uint64_t id = evs[static_cast<std::size_t>(i)].data.u64;
            const std::uint32_t ev = evs[static_cast<std::size_t>(i)].events;
            if (id == kListenId) {
                accept_clients();
            } else if (id == kWakeId) {
                eventfd_drain(wake_fd_.get());
                handle_wake();
            } else {
                handle_conn_event(id, ev);
            }
        }
        const std::uint64_t now = mono_ms();
        if (now - last_sweep >= 1000) {
            sweep_idle(now);
            last_sweep = now;
        }
    }

    pool_.reset();  // finish in-flight work and join workers before touching shared state
    conns_.clear();
    n_clients_.store(0);
    n_subs_.store(0);
    SLOG_INFO("server loop stopped");
}

void Server::accept_clients() {
    for (;;) {
        sockaddr_storage ss{};
        socklen_t sl = sizeof ss;
        const int raw = ::accept4(listen_fd_.get(), reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (raw < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EMFILE || errno == ENFILE) {
                // Out of descriptors: free the spare one, accept-and-close to drain the backlog, re-reserve.
                spare_fd_.reset();
                const int victim = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
                if (victim >= 0) ::close(victim);
                spare_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                rejected_.fetch_add(1);
                SLOG_WARN("out of file descriptors; connection refused");
                return;
            }
            if (errno == ECONNABORTED) continue;
            SLOG_ERROR("accept4: %s", std::strerror(errno));
            return;
        }
        UniqueFd fd(raw);
        if (conns_.size() >= cfg_.max_clients) {
            rejected_.fetch_add(1);
            SLOG_WARN("max clients (%zu) reached; rejecting %s", cfg_.max_clients, format_peer(ss).c_str());
            continue;  // `fd` closes here
        }
        const int one = 1;
        ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        auto c = std::make_unique<Conn>();
        c->id = next_id_++;
        c->peer = format_peer(ss);
        c->last_active_ms = mono_ms();
        c->cur_events = EPOLLIN;
        const int cfd = fd.get();
        const std::uint64_t id = c->id;
        c->fd = std::move(fd);
        ep_add(cfd, id, EPOLLIN);
        SLOG_DEBUG("client %llu connected from %s", static_cast<unsigned long long>(id), c->peer.c_str());
        conns_.emplace(id, std::move(c));
        accepted_.fetch_add(1);
        n_clients_.store(conns_.size());
    }
}

void Server::handle_conn_event(std::uint64_t id, std::uint32_t events) {
    const auto it = conns_.find(id);
    if (it == conns_.end()) return;
    Conn& c = *it->second;

    if (events & EPOLLERR) {
        close_conn(id);
        return;
    }
    if (events & EPOLLIN) {
        if (!read_input(c)) {
            close_conn(id);
            return;
        }
        c.last_active_ms = mono_ms();
    } else if (events & EPOLLHUP) {
        close_conn(id);
        return;
    }
    pump(c);  // also services EPOLLOUT
}

void Server::handle_wake() {
    // 1) replies produced by workers
    std::vector<Completion> done;
    {
        std::lock_guard<std::mutex> g(comp_mu_);
        done.swap(completions_);
    }
    for (Completion& d : done) {
        const auto it = conns_.find(d.id);
        if (it == conns_.end()) continue;  // client left while the worker was busy
        Conn& c = *it->second;
        c.busy = false;
        deliver(c, d.reply);
        pump(c);
    }

    // 2) fresh samples for subscribers
    std::vector<Sample> batch;
    events_.drain(batch);
    if (batch.empty() || n_subs_.load() == 0) return;
    std::string payload;
    payload.reserve(batch.size() * 110);
    for (const Sample& s : batch) payload += event_line(s);

    std::vector<std::uint64_t> ids;
    for (const auto& kv : conns_)
        if (kv.second->subscribed) ids.push_back(kv.first);
    for (const std::uint64_t id : ids) {
        const auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        deliver(*it->second, payload);
        pump(*it->second);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
bool Server::read_input(Conn& c) {
    char buf[4096];
    for (int round = 0; round < 8; ++round) {  // bounded work per event keeps the loop fair
        const ssize_t n = ::recv(c.fd.get(), buf, sizeof buf, 0);
        if (n > 0) {
            if (!c.closing) c.in.append(buf, static_cast<std::size_t>(n));
            extract_lines(c);
            continue;
        }
        if (n == 0) {
            c.read_eof = true;
            if (!c.in.empty() && !c.closing) {  // final command without trailing newline
                c.pending.push_back(c.in);
                c.in.clear();
            }
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    return true;
}

void Server::extract_lines(Conn& c) {
    if (c.closing) {
        c.in.clear();
        return;
    }
    std::size_t consumed = 0;
    for (;;) {
        const std::size_t nl = c.in.find('\n', consumed);
        if (nl == std::string::npos) break;
        std::string line = c.in.substr(consumed, nl - consumed);
        consumed = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > cfg_.max_line_bytes || c.pending.size() >= cfg_.max_pending_lines) {
            deliver(c, err_reply(line.size() > cfg_.max_line_bytes ? "TOO_LONG" : "TOO_MANY", "request rejected; closing"));
            c.closing = true;
            c.pending.clear();
            c.in.clear();
            return;
        }
        if (!line.empty()) c.pending.push_back(std::move(line));
    }
    c.in.erase(0, consumed);
    if (c.in.size() > cfg_.max_line_bytes) {  // unterminated and already too long
        deliver(c, err_reply("TOO_LONG", "request line too long; closing"));
        c.closing = true;
        c.pending.clear();
        c.in.clear();
    }
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------
void Server::execute(Conn& c, const std::string& line) {
    requests_.fetch_add(1);
    const ParseResult pr = parse_request(line);
    if (!pr.ok) {
        deliver(c, err_reply(pr.code, pr.message));
        return;
    }
    switch (pr.req.cmd) {
        case Cmd::Ping:
            deliver(c, ok_reply("{\"pong\":true}"));
            break;
        case Cmd::Latest: {
            const auto s = store_.latest();
            deliver(c, s ? ok_reply(to_json(*s)) : err_reply("NO_DATA", "no samples yet"));
            break;
        }
        case Cmd::Info:
            deliver(c, ok_reply(info_json()));
            break;
        case Cmd::Subscribe:
            if (!c.subscribed) {
                c.subscribed = true;
                n_subs_.fetch_add(1);
            }
            deliver(c, ok_reply("{\"subscribed\":true}"));
            break;
        case Cmd::Unsubscribe:
            if (c.subscribed) {
                c.subscribed = false;
                n_subs_.fetch_sub(1);
            }
            deliver(c, ok_reply("{\"subscribed\":false}"));
            break;
        case Cmd::SetPeriod:
            if (source_.set_period_ms(pr.req.arg)) {
                SLOG_INFO("client %s set sampling period to %u ms", c.peer.c_str(), pr.req.arg);
                deliver(c, ok_reply(strfmt("{\"period_ms\":%u}", pr.req.arg)));
            } else {
                deliver(c, err_reply("FAILED", "source rejected the requested period"));
            }
            break;
        case Cmd::Quit:
            deliver(c, ok_reply("{\"bye\":true}"));
            c.closing = true;
            c.pending.clear();
            break;
        case Cmd::Stats:
        case Cmd::History:
            dispatch(c, pr.req);
            break;
    }
}

void Server::dispatch(Conn& c, const Request& req) {
    const std::uint64_t id = c.id;
    const bool queued = pool_ && pool_->submit([this, id, req] {
        std::string reply = compute_reply(req);
        {
            std::lock_guard<std::mutex> g(comp_mu_);
            completions_.push_back(Completion{id, std::move(reply)});
        }
        eventfd_signal(wake_fd_.get());
    });
    if (queued) c.busy = true;
    else deliver(c, err_reply("BUSY", "server overloaded; retry later"));
}

std::string Server::compute_reply(const Request& req) const {
    if (req.cmd == Cmd::Stats) {
        const auto window = store_.last(req.has_arg ? req.arg : 0);
        if (window.empty()) return err_reply("NO_DATA", "no samples yet");
        return ok_reply(to_json(compute_stats(window)));
    }
    const auto window = store_.last(req.arg);
    std::string json = strfmt("{\"count\":%zu,\"samples\":[", window.size());
    for (std::size_t i = 0; i < window.size(); ++i) {
        if (i) json.push_back(',');
        json += to_json(window[i]);
    }
    json += "]}";
    return ok_reply(json);
}

std::string Server::info_json() const {
    const ServerStats s = stats();
    return strfmt(
        "{\"version\":\"%s\",\"source\":\"%s\",\"period_ms\":%u,\"uptime_s\":%llu,\"clients\":%zu,"
        "\"subscribers\":%zu,\"history_size\":%zu,\"history_capacity\":%zu,\"samples_total\":%llu,"
        "\"requests\":%llu,\"events_dropped\":%llu}",
        kVersion, json_escape(source_.name()).c_str(), source_.period_ms(),
        static_cast<unsigned long long>((mono_ms() - started_ms_) / 1000), s.clients, s.subscribers, store_.size(),
        store_.capacity(), static_cast<unsigned long long>(store_.total()), static_cast<unsigned long long>(s.requests),
        static_cast<unsigned long long>(s.events_dropped));
}

// ---------------------------------------------------------------------------
// Output, connection state machine, housekeeping
// ---------------------------------------------------------------------------
void Server::deliver(Conn& c, std::string_view data) {
    if (c.closing) return;
    if (c.out.size() - c.out_off + data.size() > cfg_.max_out_bytes) {
        slow_drops_.fetch_add(1);
        SLOG_WARN("client %s is too slow (output backlog > %zu bytes); disconnecting", c.peer.c_str(), cfg_.max_out_bytes);
        c.out.clear();
        c.out_off = 0;
        c.closing = true;
        c.pending.clear();
        return;
    }
    c.out.append(data);
}

bool Server::flush(Conn& c) {
    while (c.out_off < c.out.size()) {
        const ssize_t n = ::send(c.fd.get(), c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c.out_off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;
    }
    if (c.out_off == c.out.size()) {
        c.out.clear();
        c.out_off = 0;
    } else if (c.out_off > 65536) {
        c.out.erase(0, c.out_off);
        c.out_off = 0;
    }
    return true;
}

bool Server::pump(Conn& c) {
    while (!c.busy && !c.closing && !c.pending.empty()) {
        std::string line = std::move(c.pending.front());
        c.pending.pop_front();
        execute(c, line);
    }
    if (!flush(c)) {
        close_conn(c.id);
        return false;
    }
    const bool finished = c.closing || (c.read_eof && c.pending.empty() && !c.subscribed);
    if (finished && c.out.empty() && !c.busy) {
        close_conn(c.id);
        return false;
    }
    update_interest(c);
    return true;
}

void Server::update_interest(Conn& c) {
    std::uint32_t want = 0;
    if (!c.read_eof && !c.closing) want |= EPOLLIN;
    if (!c.out.empty()) want |= EPOLLOUT;
    if (want == c.cur_events) return;
    epoll_event ev{};
    ev.events = want;
    ev.data.u64 = c.id;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, c.fd.get(), &ev) == 0) c.cur_events = want;
}

void Server::close_conn(std::uint64_t id) {
    const auto it = conns_.find(id);
    if (it == conns_.end()) return;
    if (it->second->subscribed) n_subs_.fetch_sub(1);
    SLOG_DEBUG("client %llu (%s) disconnected", static_cast<unsigned long long>(id), it->second->peer.c_str());
    ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, it->second->fd.get(), nullptr);
    conns_.erase(it);  // UniqueFd closes the socket
    n_clients_.store(conns_.size());
}

void Server::sweep_idle(std::uint64_t now) {
    if (cfg_.idle_timeout_s <= 0) return;
    const std::uint64_t limit = static_cast<std::uint64_t>(cfg_.idle_timeout_s) * 1000ull;
    std::vector<std::uint64_t> victims;
    for (const auto& kv : conns_) {
        const Conn& c = *kv.second;
        if (!c.subscribed && !c.busy && now - c.last_active_ms > limit) victims.push_back(kv.first);
    }
    for (const std::uint64_t id : victims) {
        SLOG_INFO("closing idle client %llu", static_cast<unsigned long long>(id));
        close_conn(id);
    }
}

}  // namespace sentinel
