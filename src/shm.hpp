// Lock-free statistics export over POSIX shared memory (seqlock).
//
// One writer (the daemon), any number of read-only readers in other processes.
// Readers never take a lock, so a crashed or stalled reader can never block the
// daemon, and readers only need read permission on /dev/shm/<name>.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "util.hpp"

namespace sentinel {

// Plain-old-data; every member is 8 bytes so the struct has no padding.
struct ShmSnapshot {
    std::uint64_t updated_unix_ms;
    std::uint64_t pid;
    std::uint64_t period_ms;
    std::uint64_t samples_total;
    std::uint64_t clients;
    std::uint64_t subscribers;
    std::uint64_t window_count;
    double temp_min_c, temp_max_c, temp_avg_c;
    double hum_min_pct, hum_max_pct, hum_avg_pct;
    std::uint64_t has_latest;
    std::uint64_t latest_seq;
    std::uint64_t latest_ts_ns;
    double latest_temp_c;
    double latest_hum_pct;
};

struct ShmBlock;  // opaque layout, defined in shm.cpp

class ShmPublisher {
public:
    explicit ShmPublisher(std::string name);  // e.g. "/sentinel_stats"
    ~ShmPublisher();                          // unmaps and unlinks the segment
    ShmPublisher(const ShmPublisher&) = delete;
    ShmPublisher& operator=(const ShmPublisher&) = delete;

    void publish(const ShmSnapshot& snap);

private:
    std::string name_;
    ShmBlock* blk_ = nullptr;
    std::mutex mu_;  // serialises writers (normally only one)
};

class ShmReader {
public:
    explicit ShmReader(const std::string& name);  // throws if the segment is missing/invalid
    ~ShmReader();
    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    // True and fills `out` with a consistent snapshot; false if nothing has been
    // published yet or the writer is stuck mid-update.
    bool read(ShmSnapshot& out) const;

private:
    const ShmBlock* blk_ = nullptr;
};

}  // namespace sentinel
