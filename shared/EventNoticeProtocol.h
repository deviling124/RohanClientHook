#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

// Mirrored from RohanMapServ_Hook/src/CustomEvents/NoticeProtocol.h.
// The stock mode-0 notice remains a readable fallback on older clients.
namespace EventNoticeWire {
inline constexpr std::size_t HeaderSize = 13, MaxText = 160;
inline constexpr unsigned char Magic[] = {'R','O','H','E','V','T'};
inline constexpr unsigned char Marker[] = {'R','O','H','E','V','T',1,0,0,0};
inline constexpr std::string_view Prefix = "[Event]: ";
enum class Result { Unrelated, Invalid, Event };

inline bool ValidText(std::string_view text) {
    if (text.size() <= Prefix.size() || text.size() > MaxText || text.substr(0, Prefix.size()) != Prefix) return false;
    for (const unsigned char c : text) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
inline bool Encode(std::string_view text, unsigned char* out, std::size_t capacity, std::size_t& written) {
    written = 0;
    if (!out || capacity < HeaderSize + text.size() || !ValidText(text)) return false;
    out[0] = 0; // Native system notice; all extension data is in its ten metadata bytes.
    const auto length = static_cast<std::uint16_t>(text.size());
    std::memcpy(out + 1, &length, 2);
    std::memcpy(out + 3, Marker, sizeof Marker);
    std::memcpy(out + HeaderSize, text.data(), text.size());
    written = HeaderSize + text.size();
    return true;
}
// Disguise Event: show `name` for monster type `monsterId` instead of its real name ("=" restores it).
// Same envelope as an event notice, marker kind 2, text "<monster id>|<NAME>".
// Older clients reject the unknown marker and ignore the packet.
inline constexpr unsigned char NameMarker[] = {'R','O','H','E','V','T',2,0,0,0};
inline constexpr std::size_t MaxOverrideName = 40;
inline bool EncodeNameOverride(std::uint32_t monsterId, std::string_view name, unsigned char* out,
                               std::size_t capacity, std::size_t& written) {
    written = 0;
    if (!out || !monsterId || name.empty() || name.size() > MaxOverrideName) return false;
    for (const unsigned char c : name) if (c < 0x20 || c >= 0x7F || c == '|') return false;
    char text[MaxText + 1];
    const int length = std::snprintf(text, sizeof text, "%u|%.*s", monsterId, static_cast<int>(name.size()), name.data());
    if (length <= 0 || static_cast<std::size_t>(length) > MaxText || capacity < HeaderSize + static_cast<std::size_t>(length)) return false;
    out[0] = 0;
    const auto size = static_cast<std::uint16_t>(length);
    std::memcpy(out + 1, &size, 2);
    std::memcpy(out + 3, NameMarker, sizeof NameMarker);
    std::memcpy(out + HeaderSize, text, size);
    written = HeaderSize + size;
    return true;
}
inline bool DecodeNameOverride(const unsigned char* body, std::size_t bytes, std::uint32_t& monsterId, std::string_view& name) {
    monsterId = 0; name = {};
    if (!body || bytes < HeaderSize || body[0] != 0 || std::memcmp(body + 3, NameMarker, sizeof NameMarker)) return false;
    std::uint16_t size = 0;
    std::memcpy(&size, body + 1, 2);
    if (size < 3 || size > MaxText || bytes != HeaderSize + size) return false;
    const std::string_view text(reinterpret_cast<const char*>(body + HeaderSize), size);
    const auto bar = text.find('|');
    if (bar == std::string_view::npos || bar == 0 || bar > 10 || bar + 1 >= text.size()) return false;
    std::uint64_t id = 0;
    for (std::size_t i = 0; i < bar; ++i) { if (text[i] < '0' || text[i] > '9') return false; id = id * 10 + (text[i] - '0'); }
    const auto label = text.substr(bar + 1);
    if (!id || id > 0xFFFFFFFFull || label.size() > MaxOverrideName) return false;
    for (const unsigned char c : label) if (c < 0x20 || c >= 0x7F) return false;
    monsterId = static_cast<std::uint32_t>(id); name = label;
    return true;
}
inline Result Decode(const unsigned char* body, std::size_t bytes, std::string_view& text) {
    text = {};
    if (!body || bytes < 3 + sizeof Magic || std::memcmp(body + 3, Magic, sizeof Magic)) return Result::Unrelated;
    if (bytes < HeaderSize || body[0] != 0 || std::memcmp(body + 3, Marker, sizeof Marker)) return Result::Invalid;
    std::uint16_t length = 0;
    std::memcpy(&length, body + 1, 2);
    if (length > MaxText || bytes != HeaderSize + length) return Result::Invalid;
    const std::string_view candidate(reinterpret_cast<const char*>(body + HeaderSize), length);
    if (!ValidText(candidate)) return Result::Invalid;
    text = candidate;
    return Result::Event;
}
}
