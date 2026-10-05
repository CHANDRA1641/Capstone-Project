// Line-oriented text protocol: parsing of requests and formatting of replies.
//
//   request : COMMAND [ARGS]\n            (case-insensitive, max 512 bytes)
//   reply   : OK <json>\n | ERR <CODE> <message>\n
//   push    : EVT <json>\n                (only after SUBSCRIBE)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sample.hpp"

namespace sentinel {

inline constexpr std::uint32_t kMaxHistory = 1000;
inline constexpr std::uint32_t kMinPeriodMs = SENTINEL_PERIOD_MIN_MS;
inline constexpr std::uint32_t kMaxPeriodMs = SENTINEL_PERIOD_MAX_MS;

enum class Cmd { Ping, Latest, Stats, History, Info, Subscribe, Unsubscribe, SetPeriod, Quit };

struct Request {
    Cmd cmd = Cmd::Ping;
    std::uint32_t arg = 0;
    bool has_arg = false;
};

struct ParseResult {
    bool ok = false;
    Request req;
    std::string code;     // set when !ok
    std::string message;  // set when !ok
};

ParseResult parse_request(std::string_view line);

std::string ok_reply(std::string_view json);
std::string err_reply(std::string_view code, std::string_view message);
std::string event_line(const Sample& s);

}  // namespace sentinel
