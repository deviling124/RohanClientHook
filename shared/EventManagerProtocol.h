#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// Identical in RohanMapServ_Hook\shared and RohanClient_Hook\shared.
// Event Manager: the server tells a client that an event is about to start, so the client shows the join
// popup. It uses the event-notice envelope on packet 0x1F0A with marker kind 4; a client without this
// feature rejects the unknown marker and ignores the packet.
// Text: "<id>|<seconds left>|<state>|<event name>" = open (repeated until the start), "<id>|off" = close.
// The client answers with the Say command ".evm1 join <id>".
namespace EventManagerWire {
inline constexpr std::size_t HeaderSize = 13, MaxText = 80, MaxName = 40, MaxInvites = 4;
inline constexpr unsigned char Marker[] = {'R','O','H','E','V','T',4,0,0,0};
inline constexpr unsigned RefreshMs = 2000;
inline constexpr unsigned ExpiryMs = 7000;     // a client drops a popup the server stopped repeating
inline constexpr unsigned MaxSeconds = 3600;
inline constexpr std::string_view Command = ".evm1", JoinPrefix = ".evm1 join ";

enum class State { Asking = 0, Joined = 1, WrongPlace = 2, Failed = 3, Dead = 4 };

struct Message {
    std::uint32_t id = 0;
    bool open = false;
    unsigned seconds = 0;
    State state = State::Asking;
    std::string name;
};

inline bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > MaxName) return false;
    for (const unsigned char c : name) if (c < 0x20 || c >= 0x7F || c == '|') return false;
    return true;
}

// Digits only, at most `max`.
inline bool ParseNumber(std::string_view text, std::uint64_t max, std::uint32_t& value) {
    if (text.empty() || text.size() > 10) return false;
    std::uint64_t number = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        number = number * 10 + static_cast<unsigned>(c - '0');
    }
    if (number > max) return false;
    value = static_cast<std::uint32_t>(number);
    return true;
}

inline bool Encode(const Message& message, unsigned char* out, std::size_t capacity, std::size_t& written) {
    written = 0;
    if (!out || !message.id) return false;
    if (message.open && (!ValidName(message.name) || message.seconds > MaxSeconds)) return false;
    char text[MaxText + 1];
    const int length = message.open
        ? std::snprintf(text, sizeof text, "%u|%u|%d|%s", message.id, message.seconds,
                        static_cast<int>(message.state), message.name.c_str())
        : std::snprintf(text, sizeof text, "%u|off", message.id);
    if (length <= 0 || static_cast<std::size_t>(length) > MaxText ||
        capacity < HeaderSize + static_cast<std::size_t>(length)) return false;
    out[0] = 0;
    const auto size = static_cast<std::uint16_t>(length);
    std::memcpy(out + 1, &size, 2);
    std::memcpy(out + 3, Marker, sizeof Marker);
    std::memcpy(out + HeaderSize, text, size);
    written = HeaderSize + size;
    return true;
}

// True when the body carries the Event Manager marker, whether or not the rest is valid.
inline bool IsInvitePacket(const unsigned char* body, std::size_t bytes) {
    return body && bytes >= HeaderSize && body[0] == 0 && !std::memcmp(body + 3, Marker, sizeof Marker);
}

inline bool Decode(const unsigned char* body, std::size_t bytes, Message& message) {
    message = {};
    if (!IsInvitePacket(body, bytes)) return false;
    std::uint16_t size = 0;
    std::memcpy(&size, body + 1, 2);
    if (!size || size > MaxText || bytes != HeaderSize + size) return false;
    std::string_view rest(reinterpret_cast<const char*>(body + HeaderSize), size);
    const auto next = [&rest](std::string_view& part) {
        const auto bar = rest.find('|');
        if (bar == std::string_view::npos) return false;
        part = rest.substr(0, bar);
        rest.remove_prefix(bar + 1);
        return true;
    };
    std::string_view part;
    if (!next(part) || !ParseNumber(part, 0xFFFFFFFFull, message.id) || !message.id) return false;
    if (rest == "off") return true;
    std::uint32_t state = 0;
    if (!next(part) || !ParseNumber(part, MaxSeconds, message.seconds)) return false;
    if (!next(part) || !ParseNumber(part, static_cast<std::uint64_t>(State::Dead), state)) return false;
    if (!ValidName(rest)) return false;
    message.state = static_cast<State>(state);
    message.name = std::string(rest);
    message.open = true;
    return true;
}

// ".evm1 join <id>"
inline bool ParseJoin(std::string_view text, std::uint32_t& id) {
    id = 0;
    return text.substr(0, JoinPrefix.size()) == JoinPrefix &&
        ParseNumber(text.substr(JoinPrefix.size()), 0xFFFFFFFFull, id) && id != 0;
}
}
