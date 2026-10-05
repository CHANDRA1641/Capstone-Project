// Single-threaded epoll event loop + worker pool TCP server.
//
//  * One I/O thread owns every socket (no per-connection locks).
//  * Cheap commands run inline; STATS/HISTORY run on the pool and their replies
//    return to the loop through a completion queue + eventfd.
//  * Replies on one connection are strictly ordered even with pipelining.
//  * Slow consumers, oversized lines, pipelining floods and idle peers are cut off.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "containers.hpp"
#include "protocol.hpp"
#include "sample.hpp"
#include "source.hpp"
#include "store.hpp"
#include "thread_pool.hpp"
#include "util.hpp"

namespace sentinel {

struct ServerConfig {
    std::string bind_addr = "127.0.0.1";
    std::uint16_t port = 9090;  // 0 = pick an ephemeral port (see Server::port())
    std::size_t max_clients = 256;
    std::size_t worker_threads = 2;
    std::size_t max_line_bytes = 512;
    std::size_t max_out_bytes = 1u << 20;  // per-client unsent data before we drop the client
    std::size_t max_pending_lines = 64;
    std::size_t event_queue_depth = 4096;
    int idle_timeout_s = 300;
};

struct ServerStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t requests = 0;
    std::uint64_t slow_disconnects = 0;
    std::uint64_t events_dropped = 0;
    std::size_t clients = 0;
    std::size_t subscribers = 0;
};

class Server {
public:
    // `store` and `source` must outlive the Server.
    Server(ServerConfig cfg, SampleStore& store, SampleSource& source);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start_listening();  // bind + listen (throws). No threads are created here.
    std::uint16_t port() const noexcept { return bound_port_; }

    void run();  // event loop; returns after stop()
    void stop() noexcept;  // thread-safe

    void publish(const Sample& s);  // thread-safe; fan-out to SUBSCRIBEd clients
    ServerStats stats() const;

private:
    struct Conn;
    struct Completion {
        std::uint64_t id;
        std::string reply;
    };

    void accept_clients();
    void handle_conn_event(std::uint64_t id, std::uint32_t events);
    void handle_wake();
    bool read_input(Conn& c);
    void extract_lines(Conn& c);
    void execute(Conn& c, const std::string& line);
    void dispatch(Conn& c, const Request& req);
    std::string compute_reply(const Request& req) const;
    std::string info_json() const;

    void deliver(Conn& c, std::string_view data);
    bool flush(Conn& c);
    bool pump(Conn& c);  // returns false if the connection was closed
    void update_interest(Conn& c);
    void close_conn(std::uint64_t id);
    void sweep_idle(std::uint64_t now);
    void ep_add(int fd, std::uint64_t id, std::uint32_t events);

    ServerConfig cfg_;
    SampleStore& store_;
    SampleSource& source_;

    UniqueFd listen_fd_, epoll_fd_, wake_fd_, spare_fd_;
    std::uint16_t bound_port_ = 0;
    std::uint64_t started_ms_ = 0;
    std::atomic<bool> stop_{false};

    std::uint64_t next_id_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;

    BoundedQueue<Sample> events_;
    std::mutex comp_mu_;
    std::vector<Completion> completions_;

    std::atomic<std::uint64_t> accepted_{0}, rejected_{0}, requests_{0}, slow_drops_{0};
    std::atomic<std::size_t> n_clients_{0}, n_subs_{0};

    std::unique_ptr<ThreadPool> pool_;  // keep LAST: destroyed (joined) before anything workers touch
};

}  // namespace sentinel
