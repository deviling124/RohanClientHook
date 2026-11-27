#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// Identical in RohanMapServ_Hook\shared and RohanClient_Hook\shared.
// The server tells a client which gates are closed in its instance, so the client can stop the player at
// the wall. It uses the event-notice envelope on packet 0x1F0A with marker kind 3; a client without this
// feature rejects the unknown marker and ignores the packet.
// Text: "<name>|<ax>|<az>|<bx>|<bz>" = closed (repeated while closed), "<name>|off" = open.
namespace EventGateWire {
inline constexpr std::size_t HeaderSize = 13, MaxText = 120, MaxName = 16, MaxGates = 16;
inline constexpr unsigned char Marker[] = {'R','O','H','E','V','T',3,0,0,0};
inline constexpr unsigned RefreshMs = 2000;
inline constexpr unsigned ExpiryMs = 7000;     // a client drops a wall the server stopped repeating
// The client blocks walking closer than this to the gate. The server moves out anyone closer than
// ServerBand, which is smaller so a player stopped by the client is left alone.
inline constexpr float ClientBand = 100.0f, ServerBand = 70.0f;

struct Message {
    std::string name;
    bool closed = false;
    float ax = 0, az = 0, bx = 0, bz = 0;
};

// Distance on the ground from (x, z) to the straight piece a-b.
inline float SegmentDistance(float x, float z, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az, squared = dx * dx + dz * dz;
    float t = squared > 0 ? ((x - ax) * dx + (z - az) * dz) / squared : 0.0f;
    t = t < 0 ? 0.0f : t > 1 ? 1.0f : t;
    return std::hypot(x - (ax + dx * t), z - (az + dz * t));
}

inline bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > MaxName) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

inline bool Encode(const Message& message, unsigned char* out, std::size_t capacity, std::size_t& written) {
    written = 0;
    if (!out || !ValidName(message.name)) return false;
    char text[MaxText + 1];
    const int length = message.closed
        ? std::snprintf(text, sizeof text, "%s|%.0f|%.0f|%.0f|%.0f", message.name.c_str(),
                        message.ax, message.az, message.bx, message.bz)
        : std::snprintf(text, sizeof text, "%s|off", message.name.c_str());
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

// True when the body carries the gate marker, whether or not the rest is valid.
inline bool IsGatePacket(const unsigned char* body, std::size_t bytes) {
    return body && bytes >= HeaderSize && body[0] == 0 && !std::memcmp(body + 3, Marker, sizeof Marker);
}

inline bool Decode(const unsigned char* body, std::size_t bytes, Message& message) {
    message = {};
    if (!IsGatePacket(body, bytes)) return false;
    std::uint16_t size = 0;
    std::memcpy(&size, body + 1, 2);
    if (!size || size > MaxText || bytes != HeaderSize + size) return false;
    std::string_view rest(reinterpret_cast<const char*>(body + HeaderSize), size);
    const auto bar = rest.find('|');
    if (bar == std::string_view::npos || !ValidName(rest.substr(0, bar))) return false;
    message.name = std::string(rest.substr(0, bar));
    rest.remove_prefix(bar + 1);
    if (rest == "off") return true;
    float* values[] = {&message.ax, &message.az, &message.bx, &message.bz};
    for (int i = 0; i < 4; ++i) {
        const auto end = rest.find('|');
        if ((end == std::string_view::npos) != (i == 3)) return false;
        const std::string number(rest.substr(0, end));
        char* stop = nullptr;
        *values[i] = std::strtof(number.c_str(), &stop);
        if (number.empty() || *stop || !std::isfinite(*values[i])) return false;
        if (i < 3) rest.remove_prefix(end + 1);
    }
    message.closed = true;
    return true;
}
}
