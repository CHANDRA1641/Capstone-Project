// Sample model, JSON encoding and windowed statistics.
#pragma once

#include <sentinel/uapi.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sentinel {

using Sample = ::sentinel_sample;
static_assert(sizeof(Sample) == 24, "sentinel_sample ABI changed");

inline double sample_temp_c(const Sample& s) noexcept { return static_cast<double>(s.temp_mc) / 1000.0; }
inline double sample_hum_pct(const Sample& s) noexcept { return static_cast<double>(s.hum_mpct) / 1000.0; }

struct Summary {
    double min = 0, max = 0, mean = 0, median = 0, stddev = 0;
};

struct WindowStats {
    std::size_t count = 0;
    std::uint64_t lost = 0;      // missing sequence numbers inside the window
    std::uint64_t overruns = 0;  // samples carrying SENTINEL_FLAG_OVERRUN
    double span_s = 0;           // time between first and last sample
    double rate_hz = 0;          // observed sampling rate
    Summary temp_c;
    Summary hum_pct;
};

Summary summarize(std::vector<double> values);
WindowStats compute_stats(const std::vector<Sample>& window);

std::string to_json(const Sample& s);
std::string to_json(const Summary& s);
std::string to_json(const WindowStats& w);

}  // namespace sentinel
