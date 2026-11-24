#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Boss Check private Say transport (version 1), the same style as InspectWire: bounded lines, decimal numbers, names as hex so
// whitespace cannot change framing. Mirrored byte for byte in RohanClient_Hook/shared/BossCheckProtocol.h.
//
// Client -> server (normal Say, consumed by the hook):
//   .bos1 list <nonce>
//   .bos1 move <nonce> <index>                 index = the row number from the list (0 based)
// Server -> client (0x1F04 with sender "[BossCheck]"):
//   BOS1 row <nonce> <index> <alive 0|1> <hex name>      one per configured boss, in list order
//   BOS1 done <nonce> <count>                             ends a list answer
//   BOS1 moved <nonce> <index> <code>                     answer to a move: 0 ok, 1 boss is dead, 2 disabled, 3 cooldown,
//                                                         4 bad index, 5 other region/instance, 6 busy or not in the world
//   BOS1 err <nonce> <code>                               1 disabled, 3 too many requests, 4 bad request
namespace BossWire {
inline constexpr std::size_t MaxReply = 160, MaxName = 32, MaxBosses = 32;
inline constexpr unsigned long long MinListGapMs = 1000;

inline std::vector<std::string_view> Words(std::string_view text) {
    std::vector<std::string_view> out;
    if (text.size() > 200) return out;
    while (!text.empty()) {
        const auto end = text.find(' ');
        const auto word = text.substr(0, end);
        if (word.empty() || out.size() >= 20) return {};
        out.push_back(word);
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
        if (text.empty()) return {};
    }
    return out;
}
template<class T> inline bool Number(std::string_view text, T& number) {
    if (text.empty() || text.front() == '+') return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), number);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
inline std::string Hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
// Boss names may contain spaces (printable ASCII 32..126).
inline bool Unhex(std::string_view text, std::string& output) {
    if (text.empty() || text.size() > MaxName * 2 || text.size() % 2) return false;
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned c = 0;
        const auto r = std::from_chars(text.data() + i, text.data() + i + 2, c, 16);
        if (r.ec != std::errc{} || r.ptr != text.data() + i + 2 || c < 32 || c > 126) return false;
        out += static_cast<char>(c);
    }
    output = std::move(out);
    return true;
}

struct Request {
    bool move = false;
    std::uint32_t nonce = 0;
    int index = 0;
};
inline std::string BuildList(std::uint32_t nonce) { return ".bos1 list " + std::to_string(nonce); }
inline std::string BuildMove(std::uint32_t nonce, int index) { return ".bos1 move " + std::to_string(nonce) + " " + std::to_string(index); }
inline bool ParseRequest(std::string_view text, Request& output) {
    if (text.size() > 64) return false;
    const auto w = Words(text);
    Request r;
    if (w.size() < 3 || w[0] != ".bos1" || !Number(w[2], r.nonce) || !r.nonce) return false;
    if (w[1] == "list" && w.size() == 3) { r.move = false; }
    else if (w[1] == "move" && w.size() == 4 && Number(w[3], r.index) && r.index >= 0 && r.index < static_cast<int>(MaxBosses)) { r.move = true; }
    else return false;
    output = r;
    return true;
}

inline std::string BuildRow(std::uint32_t nonce, int index, bool alive, std::string_view name) {
    return "BOS1 row " + std::to_string(nonce) + " " + std::to_string(index) + " " + (alive ? "1 " : "0 ") + Hex(name);
}
inline std::string BuildDone(std::uint32_t nonce, int count) { return "BOS1 done " + std::to_string(nonce) + " " + std::to_string(count); }
inline std::string BuildMoved(std::uint32_t nonce, int index, int code) {
    return "BOS1 moved " + std::to_string(nonce) + " " + std::to_string(index) + " " + std::to_string(code);
}
inline std::string BuildError(std::uint32_t nonce, int code) { return "BOS1 err " + std::to_string(nonce) + " " + std::to_string(code); }

struct Row { int index = 0; bool alive = false; std::string name; };
// What the window shows. `done` is set by the end line; `moved` >= 0 once a move answer arrived.
struct List {
    std::uint32_t nonce = 0;
    std::vector<Row> rows;
    bool done = false;
    int error = 0;
    int moved = -1;
    int movedIndex = -1;
};
// Applies one server line to `list`; false for anything that is not a well formed line of this request.
inline bool Apply(List& list, std::string_view text) {
    const auto w = Words(text);
    if (w.size() < 3 || w[0] != "BOS1") return false;
    std::uint32_t nonce = 0;
    if (!Number(w[2], nonce) || nonce != list.nonce) return false;
    if (w[1] == "row" && w.size() == 6) {
        Row row;
        int alive = 0;
        if (!Number(w[3], row.index) || row.index < 0 || row.index >= static_cast<int>(MaxBosses) ||
            !Number(w[4], alive) || (alive != 0 && alive != 1) || !Unhex(w[5], row.name)) return false;
        row.alive = alive == 1;
        for (auto& existing : list.rows) if (existing.index == row.index) { existing = row; return true; }
        if (list.rows.size() >= MaxBosses) return false;
        list.rows.push_back(row);
        return true;
    }
    if (w[1] == "done" && w.size() == 4) {
        int count = 0;
        if (!Number(w[3], count) || count < 0 || count > static_cast<int>(MaxBosses)) return false;
        list.done = true;
        return true;
    }
    if (w[1] == "moved" && w.size() == 5) {
        int index = 0, code = 0;
        if (!Number(w[3], index) || !Number(w[4], code) || index < 0 || code < 0 || code > 6) return false;
        list.movedIndex = index;
        list.moved = code;
        return true;
    }
    if (w[1] == "err" && w.size() == 4) {
        int code = 0;
        if (!Number(w[3], code) || code < 1 || code > 6) return false;
        list.error = code;
        return true;
    }
    return false;
}
}  // namespace BossWire
