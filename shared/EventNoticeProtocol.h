#pragma once
#include <cstddef>
#include <cstdint>
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
