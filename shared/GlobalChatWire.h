#ifndef ROHAN_GLOBAL_CHAT_WIRE_H
#define ROHAN_GLOBAL_CHAT_WIRE_H
#include <cstdint>
#include <cstring>
#include <string_view>

// Mirrored in both hook projects. A marked Shout keeps the server-owned
// character ID, full native name field and plain chat text.
namespace GlobalChatWire {
enum class Channel : std::uint16_t { None, World, Trade };
inline constexpr unsigned Opcode = 0x1F04;
inline constexpr std::size_t HeaderSize = 43, MaxText = 160, MaxName = 20;
inline constexpr std::uint32_t Magic = 0x48434752; // RGCH on the wire
inline constexpr std::uint32_t Version = 1;

constexpr bool Known(Channel channel) { return channel == Channel::World || channel == Channel::Trade; }
inline const char* Label(Channel channel) { return channel == Channel::World ? "World" : "Trade"; }
inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool Plain(std::string_view value) {
    if (value.empty()) return false;
    bool nonSpace = false;
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7F) return false;
        nonSpace |= c != ' ';
    }
    return nonSpace;
}
inline Channel Parse(std::string_view line, std::string_view& message) {
    message = {};
    if (line.size() < 6 || line[0] != '.' ||
        (line.size() > 6 && !IsSpace(line[6]))) return Channel::None;
    char verb[5];
    for (std::size_t i = 0; i < sizeof verb; ++i) {
        char c = line[i + 1];
        verb[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    const std::string_view name(verb, sizeof verb);
    if (name != "world" && name != "trade") return Channel::None;
    line.remove_prefix(6);
    while (!line.empty() && IsSpace(line.front())) line.remove_prefix(1);
    while (!line.empty() && IsSpace(line.back())) line.remove_suffix(1);
    message = line;
    return name == "world" ? Channel::World : Channel::Trade;
}
inline std::size_t Encode(Channel channel, std::uint32_t sender, std::string_view name,
                          std::string_view message, unsigned char* out, std::size_t capacity) {
    if (!Known(channel) || !sender || name.size() > MaxName || message.size() > MaxText ||
        !Plain(name) || !Plain(message) || !out || capacity < HeaderSize + message.size()) return 0;
    const auto kind = static_cast<std::uint16_t>(channel);
    const auto length = static_cast<std::uint16_t>(message.size());
    std::memset(out, 0, HeaderSize);
    std::memcpy(out, &sender, sizeof sender);
    std::memcpy(out + 4, name.data(), name.size());
    std::memcpy(out + 26, &Magic, sizeof Magic);
    std::memcpy(out + 30, &Version, sizeof Version);
    std::memcpy(out + 34, &kind, sizeof kind);
    std::memcpy(out + 36, &length, sizeof length);
    std::memcpy(out + HeaderSize, message.data(), message.size());
    return HeaderSize + message.size();
}

struct Message {
    Channel channel = Channel::None;
    std::uint32_t sender = 0;
    std::string_view name;
    std::string_view text;
};
enum class DecodeResult { Unmarked, Invalid, Message };
inline DecodeResult Decode(const unsigned char* body, std::size_t bytes, Message& message) {
    message = {};
    if (!body || bytes < 30) return DecodeResult::Unmarked;
    std::uint32_t marker = 0;
    std::memcpy(&marker, body + 26, sizeof marker);
    if (marker != Magic) return DecodeResult::Unmarked;
    if (bytes < HeaderSize) return DecodeResult::Invalid;
    std::uint32_t version = 0, sender = 0;
    std::uint16_t kind = 0, length = 0;
    std::memcpy(&sender, body, sizeof sender);
    std::memcpy(&version, body + 30, sizeof version);
    std::memcpy(&kind, body + 34, sizeof kind);
    std::memcpy(&length, body + 36, sizeof length);
    const auto channel = static_cast<Channel>(kind);
    if (!sender || version != Version || !Known(channel) || length > MaxText ||
        bytes != HeaderSize + length || body[25] || body[38] || body[39] ||
        body[40] || body[41] || body[42]) return DecodeResult::Invalid;
    const auto* name = reinterpret_cast<const char*>(body + 4);
    const auto* end = static_cast<const char*>(std::memchr(name, 0, MaxName + 1));
    if (!end) return DecodeResult::Invalid;
    const std::string_view nameView(name, static_cast<std::size_t>(end - name));
    const std::string_view text(reinterpret_cast<const char*>(body + HeaderSize), length);
    if (!Plain(nameView) || !Plain(text)) return DecodeResult::Invalid;
    message = {channel, sender, nameView, text};
    return DecodeResult::Message;
}
}
#endif
