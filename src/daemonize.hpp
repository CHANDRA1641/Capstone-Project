// Classic SysV daemonisation and an flock()-based single-instance pidfile.
#pragma once

#include <string>

#include "util.hpp"

namespace sentinel {

// Double-fork, setsid, chdir("/"), umask, stdio -> /dev/null.
// MUST be called before any thread is created. Open file descriptors (sockets,
// pidfile locks) are intentionally preserved across the fork.
void daemonize();

class PidFile {
public:
    // Creates/locks the file. Throws std::runtime_error if another instance holds it.
    explicit PidFile(const std::string& path);
    ~PidFile();  // removes the file (only while we still hold the lock)
    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

    // Record getpid(). Call in the final (post-fork) process; the flock survives fork().
    void write_pid();

private:
    UniqueFd fd_;
    std::string path_;
};

}  // namespace sentinel
