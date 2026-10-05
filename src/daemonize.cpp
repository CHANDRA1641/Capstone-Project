#include "daemonize.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <cstdio>
#include <stdexcept>

namespace sentinel {

void daemonize() {
    pid_t pid = ::fork();
    if (pid < 0) throw_errno("fork");
    if (pid > 0) ::_exit(0);  // first parent

    if (::setsid() < 0) throw_errno("setsid");  // new session, no controlling terminal

    pid = ::fork();  // second fork: can never re-acquire a controlling terminal
    if (pid < 0) throw_errno("fork");
    if (pid > 0) ::_exit(0);

    ::umask(027);
    if (::chdir("/") != 0) throw_errno("chdir");

    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull < 0) throw_errno("open /dev/null");
    for (int fd = 0; fd <= 2; ++fd)
        if (::dup2(devnull, fd) < 0) throw_errno("dup2");
    if (devnull > 2) ::close(devnull);
}

PidFile::PidFile(const std::string& path) : path_(path) {
    const int raw = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (raw < 0) throw_errno("open pidfile " + path);
    fd_.reset(raw);
    if (::flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) throw std::runtime_error("another instance is already running (pidfile " + path + ")");
        throw_errno("flock " + path);
    }
}

PidFile::~PidFile() {
    if (fd_.valid()) ::unlink(path_.c_str());
}

void PidFile::write_pid() {
    if (::ftruncate(fd_.get(), 0) != 0) throw_errno("ftruncate pidfile");
    const std::string s = std::to_string(::getpid()) + "\n";
    if (::pwrite(fd_.get(), s.data(), s.size(), 0) != static_cast<ssize_t>(s.size())) throw_errno("write pidfile");
}

}  // namespace sentinel
