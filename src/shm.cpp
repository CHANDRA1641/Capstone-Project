#include "shm.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstring>
#include <thread>
#include <type_traits>

namespace sentinel {
namespace {
constexpr std::uint32_t kMagic = 0x53454e54u;  // "SENT"
constexpr std::uint32_t kLayoutVersion = 1;
constexpr std::size_t kWords = sizeof(ShmSnapshot) / sizeof(std::uint64_t);
static_assert(sizeof(ShmSnapshot) % sizeof(std::uint64_t) == 0, "snapshot must be a whole number of words");
static_assert(std::is_trivially_copyable_v<ShmSnapshot>, "snapshot must be POD");
}  // namespace

// Payload is stored as atomic 64-bit words so concurrent access is well-defined
// (no data race in the C++ memory model) while staying wait-free for the writer.
struct ShmBlock {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t seq;  // even = stable, odd = write in progress
    std::uint32_t reserved;
    std::uint64_t words[kWords];
};

ShmPublisher::ShmPublisher(std::string name) : name_(std::move(name)) {
    const int raw = ::shm_open(name_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (raw < 0) throw_errno("shm_open " + name_);
    UniqueFd fd(raw);
    if (::fchmod(fd.get(), 0644) != 0) throw_errno("fchmod " + name_);  // readable regardless of umask
    if (::ftruncate(fd.get(), static_cast<off_t>(sizeof(ShmBlock))) != 0) throw_errno("ftruncate " + name_);
    void* p = ::mmap(nullptr, sizeof(ShmBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (p == MAP_FAILED) throw_errno("mmap " + name_);
    blk_ = static_cast<ShmBlock*>(p);
    std::memset(blk_, 0, sizeof *blk_);
    blk_->version = kLayoutVersion;
    __atomic_store_n(&blk_->magic, kMagic, __ATOMIC_RELEASE);
}

ShmPublisher::~ShmPublisher() {
    if (blk_ != nullptr) ::munmap(blk_, sizeof(ShmBlock));
    ::shm_unlink(name_.c_str());
}

void ShmPublisher::publish(const ShmSnapshot& snap) {
    std::uint64_t w[kWords];
    std::memcpy(w, &snap, sizeof snap);

    std::lock_guard<std::mutex> g(mu_);
    const std::uint32_t seq = __atomic_load_n(&blk_->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&blk_->seq, seq + 1, __ATOMIC_RELAXED);  // odd: update in progress
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (std::size_t i = 0; i < kWords; ++i) __atomic_store_n(&blk_->words[i], w[i], __ATOMIC_RELAXED);
    __atomic_store_n(&blk_->seq, seq + 2, __ATOMIC_RELEASE);  // even: stable
}

ShmReader::ShmReader(const std::string& name) {
    const int raw = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
    if (raw < 0) throw_errno("shm_open " + name + " (is sentineld running with shared memory enabled?)");
    UniqueFd fd(raw);
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) throw_errno("fstat " + name);
    if (static_cast<std::size_t>(st.st_size) < sizeof(ShmBlock)) throw std::runtime_error("shared memory segment too small");
    void* p = ::mmap(nullptr, sizeof(ShmBlock), PROT_READ, MAP_SHARED, fd.get(), 0);
    if (p == MAP_FAILED) throw_errno("mmap " + name);
    blk_ = static_cast<const ShmBlock*>(p);
    if (__atomic_load_n(&blk_->magic, __ATOMIC_ACQUIRE) != kMagic || blk_->version != kLayoutVersion) {
        ::munmap(const_cast<ShmBlock*>(blk_), sizeof(ShmBlock));
        blk_ = nullptr;
        throw std::runtime_error("shared memory segment has an unexpected format");
    }
}

ShmReader::~ShmReader() {
    if (blk_ != nullptr) ::munmap(const_cast<ShmBlock*>(blk_), sizeof(ShmBlock));
}

bool ShmReader::read(ShmSnapshot& out) const {
    for (int attempt = 0; attempt < 10000; ++attempt) {
        const std::uint32_t s1 = __atomic_load_n(&blk_->seq, __ATOMIC_ACQUIRE);
        if (s1 == 0) return false;  // never published
        if (s1 & 1u) {              // writer active
            std::this_thread::yield();
            continue;
        }
        std::uint64_t w[kWords];
        for (std::size_t i = 0; i < kWords; ++i) w[i] = __atomic_load_n(&blk_->words[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&blk_->seq, __ATOMIC_RELAXED) == s1) {
            std::memcpy(&out, w, sizeof out);
            return true;
        }
    }
    return false;
}

}  // namespace sentinel
