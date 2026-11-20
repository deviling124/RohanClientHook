#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Collection Book private Say/Shout transport (version 1), the same style as EventShopWire: bounded lines, decimal numbers,
// names and text as hex so whitespace cannot change framing. Mirrored in RohanClient_Hook/shared/CollectionProtocol.h.
//
// Client -> server (normal Say, consumed by the hook):   .cbk1 list <nonce> <category> <page>
//                                                        .cbk1 info <nonce> <setId>
//                                                        .cbk1 reg <nonce> <setId> <slot>
//                                                        .cbk1 eq <nonce> <setId> <0|1>     (equip an owned set for free, 0 unequips; one equipped set at a time)
//                                                        .cbk1 act <nonce> <setId>          (activate = own a rune set, the server takes its Crone cost once)
//                                                        .cbk1 up <nonce> <setId>           (upgrade an activated set by one level: materials and Crones are taken first)
//                                                        .cbk1 claim <nonce> <setId>        (claim the next ready reward of an activated set)
//                                                        .cbk1 dlist <nonce> <page>         (the decomposable items in the bag, 5 per page)
//                                                        .cbk1 dis <nonce> <itemType> <1|2>   (take apart 1 or 30 of that item)
//                                                        .cbk1 poll <nonce>
// Server -> client (0x1F04 with sender "[CollectionBook]"):
//   CBK1 wait <nonce>
//   CBK1 state <nonce> <category> <page> <pages> <countInCategory>   (a page is 5 sets; then `set` + `items` per set)
//   CBK1 set <nonce> <index> <setId> <category> <ownedMask> <hex name>
//   CBK1 items <nonce> <index> <item0> .. <item5>
//   CBK1 fx <nonce> <index> <line> <lines> <hex text>      (index 0 = effect text, 1 = upgrade text, 2 = rewards text; `lines` = lines of that text)
//   CBK1 reg <nonce> <result> <setId> <slot> <ownedMask>      result 0 = registered, 1..6 see sql/CollectionBook.sql
//   CBK1 eq <nonce> <result> <setId> <active> <ownedMask>     (also the answer to `act`) result 0 = done, 1 unknown set, 2 no rune unlocked (act) / set not owned (eq), 3 already in that state, 5 not enough Crones
//   (ownedMask: bits 0-5 runes unlocked, bit 6 = equipped, bit 7 = activated/owned, bits 8-11 = upgrade level)
//   CBK1 up <nonce> <result> <setId> <ownedMask>      result 0 = upgraded, 1 the roll failed, 2 no upgrades, 3 max level, 4 not activated,
//                                                             5 not enough Crones, 7 missing materials
//   (ownedMask bits 8-11 = the set's upgrade level 0..15)
//   CBK1 claim <nonce> <result> <setId> <tier> <claimedBits>   result 0 = granted (to the Event Inventory), 1 unknown set, 2 not activated,
//                                                             3 nothing ready, 6 Event Inventory full; tier 1 activation, 2-6 runes, 7 completion
//   CBK1 dstate <nonce> <page> <pages> <count>        then `CBK1 ditem <nonce> <index> <itemType> <count> <hex name>` per row (answer of dlist)
//   CBK1 dis <nonce> <result> <itemType> <mode> <n>   then `CBK1 dgot <nonce> <index> <amount> <itemType> <hex name>` for each of the n rewards;
//                                                     result 0 done, 1 not decomposable, 2 that mode is not available, 7 not enough items,
//                                                     6 rewards could not be stored (Event Inventory full)
//   CBK1 error <nonce> <code>                                 1 disabled, 2 internal, 3 busy, 4 bad request, 7 not loaded,
//                                                             8 item not in inventory, 9 close your kiosk first
namespace CollectionWire {
inline constexpr std::size_t MaxReply = 213;
// Entry ids: rune sets use their SetId (below RuneBase), runes are listed as RuneBase + RuneId (Collection tab).
inline constexpr int RuneBase = 1000000;
inline constexpr unsigned SlotsPerSet = 6, ActiveBit = 1u << SlotsPerSet, OwnBit = 1u << (SlotsPerSet + 1), LevelShift = SlotsPerSet + 2, MaxLevel = 15,
                                MaskLimit = 1u << (LevelShift + 4), PageSize = 5, MaxPages = 1000, MaxEffectLines = 16, MaxLine = 46, MaxName = 48;

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
inline std::string Hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
inline bool Unhex(std::string_view text, std::string& output, std::size_t maxBytes = 48) {
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

enum class Action { List, Info, Register, Poll, Equip, Activate, Upgrade, Claim, DList, Disassemble };
struct Request {
    Action action = Action::List;
    std::uint32_t nonce = 0;
    int set = 0;
    unsigned slot = 0;
    unsigned category = 0;
    unsigned page = 0;
};
inline bool ParseRequest(std::string_view text, Request& output) {
    if (text.size() > 96) return false;
    const auto w = Words(text);
    Request r;
    if (w.size() < 3 || w[0] != ".cbk1" || !Number(w[2], r.nonce) || !r.nonce) return false;
    if (w[1] == "list" && w.size() == 5 && Number(w[3], r.category) && r.category >= 1 && r.category <= 8 &&
        Number(w[4], r.page) && r.page < MaxPages) r.action = Action::List;
    else if (w[1] == "info" && w.size() == 4 && Number(w[3], r.set) && r.set > 0) r.action = Action::Info;
    else if (w[1] == "poll" && w.size() == 3) r.action = Action::Poll;
    else if (w[1] == "reg" && w.size() == 5 && Number(w[3], r.set) && r.set > 0 && Number(w[4], r.slot) &&
             r.slot < SlotsPerSet) r.action = Action::Register;
    else if (w[1] == "eq" && w.size() == 5 && Number(w[3], r.set) && r.set > 0 && Number(w[4], r.slot) && r.slot < 2)
        r.action = Action::Equip;  // slot carries the flag: 1 equip (activate) a rune set, 0 unequip it
    else if (w[1] == "act" && w.size() == 4 && Number(w[3], r.set) && r.set > 0) r.action = Action::Activate;
    else if (w[1] == "up" && w.size() == 4 && Number(w[3], r.set) && r.set > 0) r.action = Action::Upgrade;
    else if (w[1] == "claim" && w.size() == 4 && Number(w[3], r.set) && r.set > 0) r.action = Action::Claim;
    else if (w[1] == "dlist" && w.size() == 4 && Number(w[3], r.page) && r.page < MaxPages) r.action = Action::DList;
    else if (w[1] == "dis" && w.size() == 5 && Number(w[3], r.set) && r.set > 0 && Number(w[4], r.slot) && (r.slot == 1 || r.slot == 2)) r.action = Action::Disassemble;
    else return false;
    output = r;
    return true;
}

struct Set {
    int id = 0;
    unsigned category = 1;
    unsigned owned = 0;  // bit n = rune/slot n unlocked, bit 6 (ActiveBit) = set activated
    long long cost = 0;  // activation price in Crones (server side only, not sent)
    std::string name;
    std::vector<std::string> effect;  // display lines
    unsigned items[SlotsPerSet]{};
};
inline bool Printable(std::string_view s) {
    for (unsigned char c : s) if (c < 32 || c > 126) return false;
    return true;
}
inline bool Valid(const Set& s) {
    if (s.id <= 0 || s.category < 1 || s.category > 8 || s.name.empty() || s.name.size() > MaxName || !Printable(s.name) ||
        s.effect.size() > MaxEffectLines || s.owned >= MaskLimit) return false;
    for (const auto& line : s.effect) if (line.size() > MaxLine || !Printable(line)) return false;
    return true;
}
// Splits administrator text (lines separated by '\n') into display lines of at most MaxLine characters, wrapping at spaces.
// Non-printable characters become spaces; at most MaxEffectLines lines are kept.
inline std::vector<std::string> SplitEffect(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    auto flush = [&] { if (lines.size() < MaxEffectLines) lines.push_back(current); current.clear(); };
    std::string word;
    auto addWord = [&] {
        if (word.empty()) return;
        while (word.size() > MaxLine) { if (!current.empty()) flush(); current = word.substr(0, MaxLine); flush(); word.erase(0, MaxLine); }
        if (!current.empty() && current.size() + 1 + word.size() > MaxLine) flush();
        if (!current.empty()) current += ' ';
        current += word;
        word.clear();
    };
    for (unsigned char c : text) {
        if (c == '\n') { addWord(); flush(); }
        else if (c == ' ' || c < 32 || c > 126) addWord();
        else word += static_cast<char>(c);
    }
    addWord();
    if (!current.empty()) flush();
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

inline std::string N(std::uint64_t v) { return std::to_string(v); }
inline std::string Wait(std::uint32_t nonce) { return "CBK1 wait " + N(nonce); }
inline std::string Error(std::uint32_t nonce, int code) { return "CBK1 error " + N(nonce) + " " + std::to_string(code); }
inline std::string State(std::uint32_t nonce, unsigned category, unsigned page, unsigned pages, unsigned count) {
    return "CBK1 state " + N(nonce) + " " + N(category) + " " + N(page) + " " + N(pages) + " " + N(count);
}
inline std::string SetRow(std::uint32_t nonce, unsigned index, const Set& s) {
    return "CBK1 set " + N(nonce) + " " + N(index) + " " + std::to_string(s.id) + " " + N(s.category) + " " + N(s.owned) + " " +
           Hex(s.name);
}
inline std::string ItemsRow(std::uint32_t nonce, unsigned index, const Set& s) {
    std::string out = "CBK1 items " + N(nonce) + " " + N(index);
    for (unsigned i = 0; i < SlotsPerSet; ++i) out += " " + N(s.items[i]);
    return out;
}
inline std::string FxRow(std::uint32_t nonce, unsigned index, unsigned line, unsigned lines, const std::string& text) {
    return "CBK1 fx " + N(nonce) + " " + N(index) + " " + N(line) + " " + N(lines) + " " + (text.empty() ? std::string("-") : Hex(text));
}
inline std::string EquipResult(std::uint32_t nonce, int result, int set, unsigned active, unsigned owned) {
    return "CBK1 eq " + N(nonce) + " " + std::to_string(result) + " " + std::to_string(set) + " " + N(active) + " " + N(owned);
}
inline std::string UpResult(std::uint32_t nonce, int result, int set, unsigned owned) {
    return "CBK1 up " + N(nonce) + " " + std::to_string(result) + " " + std::to_string(set) + " " + N(owned);
}
inline std::string ClaimResult(std::uint32_t nonce, int result, int set, int tier, unsigned bits) {
    return "CBK1 claim " + N(nonce) + " " + std::to_string(result) + " " + std::to_string(set) + " " + std::to_string(tier) + " " + N(bits);
}
inline std::string DState(std::uint32_t nonce, unsigned page, unsigned pages, unsigned count) {
    return "CBK1 dstate " + N(nonce) + " " + N(page) + " " + N(pages) + " " + N(count);
}
inline std::string DItem(std::uint32_t nonce, unsigned index, unsigned item, unsigned count, const std::string& name) {
    return "CBK1 ditem " + N(nonce) + " " + N(index) + " " + N(item) + " " + N(count) + " " + Hex(name);
}
inline std::string DisResult(std::uint32_t nonce, int result, unsigned item, unsigned mode, unsigned n) {
    return "CBK1 dis " + N(nonce) + " " + std::to_string(result) + " " + N(item) + " " + N(mode) + " " + N(n);
}
inline std::string DGot(std::uint32_t nonce, unsigned index, int amount, unsigned item, const std::string& name) {
    return "CBK1 dgot " + N(nonce) + " " + N(index) + " " + std::to_string(amount) + " " + N(item) + " " + Hex(name);
}
inline std::string RegResult(std::uint32_t nonce, int result, int set, unsigned slot, unsigned owned) {
    return "CBK1 reg " + N(nonce) + " " + std::to_string(result) + " " + std::to_string(set) + " " + N(slot) + " " + N(owned);
}
}
