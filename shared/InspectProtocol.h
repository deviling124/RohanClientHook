#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Inspect player private Say transport (version 1), the same style as CollectionWire: bounded lines, decimal numbers, the name as
// hex so whitespace cannot change framing. Mirrored byte for byte in RohanClient_Hook/shared/InspectProtocol.h.
//
// Client -> server (normal Say, consumed by the hook):   .ins1 view <nonce> <hex name>
// Server -> client (0x1F04 with sender "[Inspect]"), one request is answered by these lines in order:
//   INS1 head <nonce> <level> <hex name>
//   INS1 attr <nonce> <STR> <VIT> <INT> <DEX> <PSY> <AGI>
//   INS1 stat <nonce> <melee> <ranged> <magic> <physDef> <magicDef> <accuracy> <evasion>
//   INS1 vit <nonce> <maxHp> <hpRecovery> <maxMp> <mpRecovery> <totalResist> <mobility> <attackSpeed> <maxWeight>
//   INS1 res <nonce> <fire> <ice> <dark> <light>
//   INS1 eq <nonce> <firstSlot> <type> x7                 (five times: slots 0..6, 7..13, 14..20, 21..27, 28..34; type 0 = empty)
//   INS1 info <nonce> <characterType> <dbType> <guildGrade> <titleId> <hex guild name | 0>
//                       characterType = the live player's type (class and race bits), dbType = TCharacter.ctype_id, guildGrade = TNGuildMember.grade (-1 = no guild),
//                       titleId = TCharacterStatus.id_char_title, guild name = TNGuild.name (0 = none). All zeros when the database could not be read.
//   INS1 rec <nonce> <slot> <122 hex digits>   the item's native 61-byte record (SerializeItem 0x0068B5C0: type, id, 14 key/value pairs, ...) of every
//                       equipped item, so the client can show the real tooltip with enhancements and options. Optional, sent before `done`.
//   INS1 done <nonce>
//   INS1 error <nonce> <code>     1 disabled, 2 player not found, 3 too many requests, 4 bad request, 5 hidden by the player/server, 6 busy
// Equipment slots are the server's inventory-0 slots: 0 head, 1 top, 2 bottom, 3 hand, 4 leg, 5 weapon, 6 shield, 7 glasses, 8 earring,
// 9..12 accessories, 13 arrow; the others (costumes and more) are sent as they are, 35 slots in all (29 exist, the rest is 0).
namespace InspectWire {
inline constexpr std::size_t MaxReply = 160, MaxName = 16, SlotCount = 35, RecordSize = 61;
inline constexpr unsigned long long MinRequestGapMs = 1000;

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
inline bool Unhex(std::string_view text, std::string& output, std::size_t maxBytes = MaxName) {
    if (text.empty() || text.size() > maxBytes * 2 || text.size() % 2) return false;
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned c = 0;
        const auto r = std::from_chars(text.data() + i, text.data() + i + 2, c, 16);
        if (r.ec != std::errc{} || r.ptr != text.data() + i + 2 || c < 33 || c > 126) return false;
        out += static_cast<char>(c);
    }
    output = std::move(out);
    return true;
}

struct Request {
    std::uint32_t nonce = 0;
    std::string name;
};
inline std::string BuildRequest(std::uint32_t nonce, std::string_view name) {
    return ".ins1 view " + std::to_string(nonce) + " " + Hex(name);
}
inline bool ParseRequest(std::string_view text, Request& output) {
    if (text.size() > 96) return false;
    const auto w = Words(text);
    Request r;
    if (w.size() != 4 || w[0] != ".ins1" || w[1] != "view" || !Number(w[2], r.nonce) || !r.nonce || !Unhex(w[3], r.name)) return false;
    output = std::move(r);
    return true;
}

// Everything the window shows about one character.
struct Profile {
    std::uint32_t nonce = 0;
    int error = 0;           // 0 = none
    bool done = false;
    unsigned parts = 0;      // bit n = line n arrived (head, attr, stat, vit, res, the five eq lines, info)
    int level = 0;
    std::string name;
    int attr[6]{};           // STR VIT INT DEX PSY AGI
    int stat[7]{};           // melee ranged magic physDef magicDef accuracy evasion
    int vit[8]{};            // maxHp hpRecovery maxMp mpRecovery totalResist mobility attackSpeed maxWeight
    int res[4]{};            // fire ice dark light
    unsigned eq[SlotCount]{};
    unsigned type = 0;       // live character type (class and race bits)
    int dbType = 0, guildGrade = -1;
    unsigned title = 0;
    std::string guild;       // empty = no guild
    std::string rec[SlotCount];   // the native item record of a slot (RecordSize bytes) or empty
};

inline std::string Join(const char* tag, std::uint32_t nonce, const int* values, std::size_t count) {
    std::string out = std::string("INS1 ") + tag + " " + std::to_string(nonce);
    for (std::size_t i = 0; i < count; ++i) out += " " + std::to_string(values[i]);
    return out;
}
// The lines the server sends for a profile (error == 0).
inline std::vector<std::string> BuildReplies(const Profile& p) {
    std::vector<std::string> out;
    out.push_back("INS1 head " + std::to_string(p.nonce) + " " + std::to_string(p.level) + " " + Hex(p.name));
    out.push_back(Join("attr", p.nonce, p.attr, 6));
    out.push_back(Join("stat", p.nonce, p.stat, 7));
    out.push_back(Join("vit", p.nonce, p.vit, 8));
    out.push_back(Join("res", p.nonce, p.res, 4));
    for (std::size_t first = 0; first < SlotCount; first += 7) {
        std::string line = "INS1 eq " + std::to_string(p.nonce) + " " + std::to_string(first);
        for (std::size_t i = 0; i < 7; ++i) line += " " + std::to_string(p.eq[first + i]);
        out.push_back(std::move(line));
    }
    out.push_back("INS1 info " + std::to_string(p.nonce) + " " + std::to_string(p.type) + " " + std::to_string(p.dbType) + " " + std::to_string(p.guildGrade) +
                  " " + std::to_string(p.title) + " " + (p.guild.empty() ? std::string("0") : Hex(p.guild)));
    for (std::size_t slot = 0; slot < SlotCount; ++slot)
        if (p.rec[slot].size() == RecordSize)
            out.push_back("INS1 rec " + std::to_string(p.nonce) + " " + std::to_string(slot) + " " + Hex(p.rec[slot]));
    out.push_back("INS1 done " + std::to_string(p.nonce));
    return out;
}
// Like Unhex, but spaces are allowed (guild names).
inline bool UnhexText(std::string_view text, std::string& output, std::size_t maxBytes = 24) {
    if (text.empty() || text.size() > maxBytes * 2 || text.size() % 2) return false;
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
inline std::string BuildError(std::uint32_t nonce, int code) {
    return "INS1 error " + std::to_string(nonce) + " " + std::to_string(code);
}

template<std::size_t N> inline bool ReadInts(const std::vector<std::string_view>& w, std::size_t from, int (&out)[N]) {
    if (w.size() != from + N) return false;
    int values[N]{};
    for (std::size_t i = 0; i < N; ++i) if (!Number(w[from + i], values[i])) return false;
    for (std::size_t i = 0; i < N; ++i) out[i] = values[i];
    return true;
}
// Client: applies one reply line to `p`. A line of another nonce (an older request) is ignored; returns true when the line was ours and valid.
inline bool Apply(Profile& p, std::string_view line) {
    if (line.size() > MaxReply) return false;
    const auto w = Words(line);
    std::uint32_t nonce = 0;
    if (w.size() < 3 || w[0] != "INS1" || !Number(w[2], nonce) || nonce != p.nonce) return false;
    if (w[1] == "head" && w.size() == 5) {
        int level = 0;
        std::string name;
        if (!Number(w[3], level) || !Unhex(w[4], name)) return false;
        p.level = level;
        p.name = std::move(name);
        p.parts |= 1;
        return true;
    }
    if (w[1] == "attr" && ReadInts(w, 3, p.attr)) { p.parts |= 2; return true; }
    if (w[1] == "stat" && ReadInts(w, 3, p.stat)) { p.parts |= 4; return true; }
    if (w[1] == "vit" && ReadInts(w, 3, p.vit)) { p.parts |= 8; return true; }
    if (w[1] == "res" && ReadInts(w, 3, p.res)) { p.parts |= 16; return true; }
    if (w[1] == "eq" && w.size() == 11) {
        unsigned first = 0;
        unsigned types[7]{};
        if (!Number(w[3], first) || first % 7 || first >= SlotCount) return false;
        for (std::size_t i = 0; i < 7; ++i) if (!Number(w[4 + i], types[i])) return false;
        for (std::size_t i = 0; i < 7; ++i) p.eq[first + i] = types[i];
        p.parts |= 32u << (first / 7);
        return true;
    }
    if (w[1] == "info" && w.size() == 8) {
        unsigned type = 0, title = 0;
        int dbType = 0, grade = 0;
        std::string guild;
        if (!Number(w[3], type) || !Number(w[4], dbType) || !Number(w[5], grade) || !Number(w[6], title)) return false;
        if (w[7] != "0" && !UnhexText(w[7], guild)) return false;
        p.type = type; p.dbType = dbType; p.guildGrade = grade; p.title = title; p.guild = std::move(guild);
        p.parts |= 1024;
        return true;
    }
    if (w[1] == "rec" && w.size() == 5) {
        unsigned slot = 0;
        if (!Number(w[3], slot) || slot >= SlotCount || w[4].size() != RecordSize * 2) return false;
        std::string bytes;
        bytes.reserve(RecordSize);
        for (std::size_t i = 0; i < w[4].size(); i += 2) {
            unsigned c = 0;
            const auto r = std::from_chars(w[4].data() + i, w[4].data() + i + 2, c, 16);
            if (r.ec != std::errc{} || r.ptr != w[4].data() + i + 2) return false;
            bytes += static_cast<char>(c);
        }
        p.rec[slot] = std::move(bytes);
        return true;
    }
    if (w[1] == "done" && w.size() == 3) { p.done = true; return true; }
    if (w[1] == "error" && w.size() == 4) {
        int code = 0;
        if (!Number(w[3], code) || code < 1 || code > 9) return false;
        p.error = code;
        p.done = true;
        return true;
    }
    return false;
}
inline bool Complete(const Profile& p) { return p.done && !p.error && p.parts == 2047; }
}  // namespace InspectWire
