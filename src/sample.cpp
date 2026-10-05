#include "sample.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util.hpp"

namespace sentinel {

Summary summarize(std::vector<double> v) {
    Summary s;
    if (v.empty()) return s;

    const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
    s.min = *mn;
    s.max = *mx;
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());

    const double sq = std::accumulate(v.begin(), v.end(), 0.0, [&](double acc, double x) {
        const double d = x - s.mean;
        return acc + d * d;
    });
    s.stddev = std::sqrt(sq / static_cast<double>(v.size()));

    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    s.median = (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    return s;
}

WindowStats compute_stats(const std::vector<Sample>& w) {
    WindowStats out;
    out.count = w.size();
    if (w.empty()) return out;

    std::vector<double> t, h;
    t.reserve(w.size());
    h.reserve(w.size());
    for (const Sample& s : w) {
        t.push_back(sample_temp_c(s));
        h.push_back(sample_hum_pct(s));
        if (s.flags & SENTINEL_FLAG_OVERRUN) ++out.overruns;
    }
    out.temp_c = summarize(std::move(t));
    out.hum_pct = summarize(std::move(h));

    // Unsigned 32-bit subtraction is wrap-around safe.
    const std::uint32_t span = w.back().seq - w.front().seq;
    const std::uint64_t expected = static_cast<std::uint64_t>(span) + 1;
    out.lost = expected > w.size() ? expected - w.size() : 0;

    if (w.size() > 1 && w.back().ts_ns > w.front().ts_ns) {
        out.span_s = static_cast<double>(w.back().ts_ns - w.front().ts_ns) / 1e9;
        out.rate_hz = static_cast<double>(w.size() - 1) / out.span_s;
    }
    return out;
}

std::string to_json(const Sample& s) {
    return strfmt("{\"seq\":%u,\"ts_ns\":%llu,\"temp_c\":%.3f,\"hum_pct\":%.3f,\"flags\":%u}", s.seq,
                  static_cast<unsigned long long>(s.ts_ns), sample_temp_c(s), sample_hum_pct(s), s.flags);
}

std::string to_json(const Summary& s) {
    return strfmt("{\"min\":%.3f,\"max\":%.3f,\"mean\":%.3f,\"median\":%.3f,\"stddev\":%.3f}", s.min, s.max, s.mean,
                  s.median, s.stddev);
}

std::string to_json(const WindowStats& w) {
    return strfmt("{\"count\":%zu,\"lost\":%llu,\"overruns\":%llu,\"span_s\":%.3f,\"rate_hz\":%.3f,\"temp_c\":%s,"
                  "\"hum_pct\":%s}",
                  w.count, static_cast<unsigned long long>(w.lost), static_cast<unsigned long long>(w.overruns),
                  w.span_s, w.rate_hz, to_json(w.temp_c).c_str(), to_json(w.hum_pct).c_str());
}

}  // namespace sentinel
