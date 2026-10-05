#include "protocol.hpp"

#include <cctype>
#include <charconv>
#include <vector>

namespace sentinel {
namespace {

std::vector<std::string_view> tokenize(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

bool parse_u32(std::string_view s, std::uint32_t& out) {
    const char* b = s.data();
    const char* e = s.data() + s.size();
    const auto [p, ec] = std::from_chars(b, e, out);
    return ec == std::errc() && p == e;
}

ParseResult fail(const char* code, std::string msg) {
    ParseResult r;
    r.code = code;
    r.message = std::move(msg);
    return r;
}

ParseResult success(Cmd c, bool has_arg = false, std::uint32_t arg = 0) {
    ParseResult r;
    r.ok = true;
    r.req.cmd = c;
    r.req.has_arg = has_arg;
    r.req.arg = arg;
    return r;
}

}  // namespace

ParseResult parse_request(std::string_view line) {
    const auto tok = tokenize(line);
    if (tok.empty()) return fail("BAD_COMMAND", "empty command");
    const std::string cmd = upper(tok[0]);

    auto no_args = [&](Cmd c) -> ParseResult {
        if (tok.size() != 1) return fail("BAD_ARGUMENT", cmd + " takes no arguments");
        return success(c);
    };

    if (cmd == "PING") return no_args(Cmd::Ping);
    if (cmd == "LATEST") return no_args(Cmd::Latest);
    if (cmd == "INFO") return no_args(Cmd::Info);
    if (cmd == "SUBSCRIBE") return no_args(Cmd::Subscribe);
    if (cmd == "UNSUBSCRIBE") return no_args(Cmd::Unsubscribe);
    if (cmd == "QUIT") return no_args(Cmd::Quit);

    if (cmd == "STATS" || cmd == "HISTORY") {
        const bool is_stats = (cmd == "STATS");
        const Cmd c = is_stats ? Cmd::Stats : Cmd::History;
        if (tok.size() == 1 && is_stats) return success(c);
        if (tok.size() != 2) return fail("BAD_ARGUMENT", "usage: " + cmd + (is_stats ? " [n]" : " n"));
        std::uint32_t n = 0;
        if (!parse_u32(tok[1], n) || n < 1 || n > kMaxHistory)
            return fail("BAD_ARGUMENT", "n must be an integer in [1," + std::to_string(kMaxHistory) + "]");
        return success(c, true, n);
    }

    if (cmd == "SET") {
        if (tok.size() != 3 || upper(tok[1]) != "PERIOD") return fail("BAD_ARGUMENT", "usage: SET PERIOD <ms>");
        std::uint32_t ms = 0;
        if (!parse_u32(tok[2], ms) || ms < kMinPeriodMs || ms > kMaxPeriodMs)
            return fail("BAD_ARGUMENT", "period must be an integer in [" + std::to_string(kMinPeriodMs) + "," +
                                            std::to_string(kMaxPeriodMs) + "] ms");
        return success(Cmd::SetPeriod, true, ms);
    }

    return fail("BAD_COMMAND", "unknown command");
}

std::string ok_reply(std::string_view json) {
    std::string s = "OK ";
    s.append(json);
    s.push_back('\n');
    return s;
}

std::string err_reply(std::string_view code, std::string_view message) {
    std::string s = "ERR ";
    s.append(code);
    s.push_back(' ');
    s.append(message);
    s.push_back('\n');
    return s;
}

std::string event_line(const Sample& s) { return "EVT " + to_json(s) + "\n"; }

}  // namespace sentinel
