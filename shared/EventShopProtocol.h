#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Versioned, bounded private Say/Shout transport. Numbers are decimal; names
// are hex so whitespace cannot change framing. No client-supplied prices.
namespace EventShopWire {
inline constexpr unsigned PageSize = 6;
inline constexpr const char* Categories[] = {"Equipment", "Consumables", "Materials", "Other"};
inline constexpr unsigned CategoryCount = 4;
inline constexpr unsigned SafeZoneRequired = 5;
inline constexpr std::int64_t MaxBalance = 9000000000000000LL;
inline constexpr std::size_t MaxReply = 213;
inline std::vector<std::string_view> Words(std::string_view text) {
    std::vector<std::string_view> out;
    if (text.size() > 256) return out;
    while (!text.empty()) {
        const auto end = text.find(' ');
        const auto word = text.substr(0, end);
        if (word.empty() || out.size() >= 16) return {};
        out.push_back(word);
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
        if (text.empty()) return {};
    }
    return out;
}
template<class T> inline bool Number(std::string_view text, T& number) {
    if (text.empty() || text.front() == '-' || text.front() == '+') return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), number);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
inline bool Guid(std::string_view text) {
    if (text.size() != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
        else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return text != "00000000-0000-0000-0000-000000000000";
}
inline std::string Hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
inline bool Unhex(std::string_view text, std::string& output, std::size_t maxBytes = 48) {
    if (text.empty() || maxBytes > 80 || text.size() > maxBytes * 2 || text.size() % 2) return false;
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned c = 0;
        const auto r = std::from_chars(text.data() + i, text.data() + i + 2, c, 16);
        if (r.ec != std::errc{} || r.ptr != text.data() + i + 2 || c < 32 || c > 126) return false;
        out += static_cast<char>(c);
    }
    output = std::move(out); return true;
}
enum class Action { List, Buy, Poll, Open };
struct Request {
    Action action = Action::List;
    std::uint32_t nonce = 0;
    unsigned page = 0;
    unsigned category = 1;
    int offer = 0;
    std::int64_t revision = 0;
    std::string key;
};
inline bool ParseRequest(std::string_view text, Request& output) {
    if (text.size() > 160) return false;
    const auto w = Words(text);
    Request r;
    if (w.size() < 3 || w[0] != ".eps1" || !Number(w[2], r.nonce) || !r.nonce) return false;
    if (w[1] == "list" && w.size() == 5 && Number(w[3], r.page) && r.page <= 9999 &&
        Number(w[4], r.category) && r.category >= 1 && r.category <= CategoryCount) r.action = Action::List;
    else if (w[1] == "poll" && w.size() == 3) r.action = Action::Poll;
    else if (w[1] == "open" && w.size() == 3) r.action = Action::Open;
    else if (w[1] == "buy" && w.size() == 6 && Guid(w[3]) && Number(w[4], r.offer) && r.offer > 0 &&
             Number(w[5], r.revision) && r.revision > 0) { r.action = Action::Buy; r.key = w[3]; }
    else return false;
    output = std::move(r); return true;
}
struct Offer {
    int id = 0, item = 0, quantity = 0, price = 0;
    std::int64_t revision = 0;
    std::string name;
    std::string description;
};
inline bool Valid(const Offer& o) {
    if (o.id <= 0 || o.item <= 0 || o.revision <= 0 || o.quantity < 1 || o.quantity > 255 ||
        o.price < 1 || o.price > 1000000000 || o.name.empty() || o.name.size() > 48 || o.description.size() > 80) return false;
    for (unsigned char c : o.name) if (c < 32 || c > 126) return false;
    for (unsigned char c : o.description) if (c < 32 || c > 126) return false;
    return true;
}
inline std::string Row(std::uint32_t nonce, unsigned index, const Offer& o) {
    return "EPS1 row " + std::to_string(nonce) + " " + std::to_string(index) + " " +
        std::to_string(o.id) + " " + std::to_string(o.revision) + " " + std::to_string(o.item) + " " +
        std::to_string(o.quantity) + " " + std::to_string(o.price) + " " + Hex(o.name);
}
inline std::string Detail(std::uint32_t nonce, unsigned index, const Offer& o) {
    return "EPS1 detail " + std::to_string(nonce) + " " + std::to_string(index) + " " +
        (o.description.empty() ? "-" : Hex(o.description));
}
}
