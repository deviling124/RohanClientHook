#pragma once
// Wire format for the drop-buff countdown shown by the client overlay.
// The same file lives in RohanMapServ_Hook/shared and RohanClient_Hook/shared; keep them identical.
//
// The client asks with the local chat command ".dbf1" (never broadcast). The server answers with one
// server shout from "[DropBuff]" whose text is: "DBF1 <percent> <remainingSeconds> <paused>".
// percent 0 means no buff. paused is 1 while the timer is not running (safe zone).
#include <cstdint>
#include <string>
#include <string_view>

namespace DropBuffWire {
inline constexpr std::string_view Source = "[DropBuff]";
inline constexpr std::string_view PollCommand = ".dbf1";
inline constexpr std::string_view Prefix = "DBF1 ";
inline constexpr int MaxPercent = 1000;
inline constexpr unsigned MaxSeconds = 24 * 60 * 60;

struct State {
    int percent = 0;
    unsigned remainingSeconds = 0;
    bool paused = false;
    bool operator==(const State& other) const {
        return percent == other.percent && remainingSeconds == other.remainingSeconds && paused == other.paused;
    }
};

inline std::string Encode(const State& state) {
    const int percent = state.percent < 0 ? 0 : (state.percent > MaxPercent ? MaxPercent : state.percent);
    const unsigned seconds = state.remainingSeconds > MaxSeconds ? MaxSeconds : state.remainingSeconds;
    return std::string(Prefix) + std::to_string(percent) + " " + std::to_string(seconds) + " " +
        (state.paused ? "1" : "0");
}

namespace detail {
inline bool Number(std::string_view text, unsigned long& value) {
    if (text.empty() || text.size() > 6) return false;
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    return true;
}
}

// Strict parse: exactly three fields, in range. Anything else is rejected.
inline bool Parse(std::string_view text, State& out) {
    if (text.substr(0, Prefix.size()) != Prefix) return false;
    text.remove_prefix(Prefix.size());
    std::string_view fields[3];
    for (int i = 0; i < 3; ++i) {
        const auto split = text.find(' ');
        if (i < 2) {
            if (split == std::string_view::npos) return false;
            fields[i] = text.substr(0, split);
            text.remove_prefix(split + 1);
        } else {
            if (split != std::string_view::npos) return false;
            fields[i] = text;
        }
    }
    unsigned long percent = 0, seconds = 0, paused = 0;
    if (!detail::Number(fields[0], percent) || !detail::Number(fields[1], seconds) ||
        !detail::Number(fields[2], paused)) return false;
    if (percent > static_cast<unsigned long>(MaxPercent) || seconds > MaxSeconds || paused > 1) return false;
    out.percent = static_cast<int>(percent);
    out.remainingSeconds = static_cast<unsigned>(seconds);
    out.paused = paused == 1;
    return true;
}
}
