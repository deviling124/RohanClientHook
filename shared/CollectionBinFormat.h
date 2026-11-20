#pragma once
// The Collection Book client data files (data\cb_*.bin): the display side of the catalog, read by the client at startup so that lists,
// names, costs and texts need no server round trip. Same style as the game's own tables: back-to-back fixed-size records, little-endian,
// strings as fixed-size NUL-padded ASCII/Big5. The server keeps the authoritative catalog (YAML); tools\CollectionBinTool writes these
// files from it and tools\CollectionBinEditor edits them. Mirrored in RohanClient_Hook\shared\CollectionBinFormat.h.
//
//   cb_rune.bin    108 bytes  ID i32, Item i32, Icon i32, Name[48], ItemName[48]
//   cb_set.bin      84 bytes  ID i32, Name[48], Rune0..Rune5 i32, Crones i64
//   cb_bonus.bin    24 bytes  SetID, UnlockedCount, Kind (1 VIT 2 STR 3 DEX 4 AGI), Value, LevelDivisor, PerLevel  (6 x i32)
//   cb_level.bin   216 bytes  SetID i32, Level i32, Crones i64, Rate i32, Pity i32, then 4 x { Item i32, Amount i32, Name[40] }
//   cb_reward.bin   56 bytes  SetID i32, Tier i32, Item i32, Amount i32, Name[40]
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace CollectionBin {

constexpr std::size_t RuneRecord = 108, SetRecord = 84, BonusRecord = 24, LevelRecord = 216, RewardRecord = 56;
constexpr int LevelItems = 4;

struct Rune { int id = 0; int item = 0; int icon = 0; std::string name, itemName; };
struct Set { int id = 0; std::string name; int runes[6]{}; long long crones = 0; };
struct Bonus { int setId = 0, count = 0, kind = 0, value = 0, divisor = 1, perLevel = 0; };
struct LevelItem { int item = 0, amount = 0; std::string name; };
struct Level { int setId = 0, level = 0; long long crones = 0; int rate = 0, pity = 0; LevelItem items[LevelItems]; };
struct Reward { int setId = 0, tier = 0, item = 0, amount = 0; std::string name; };

struct Catalog {
    std::vector<Rune> runes;
    std::vector<Set> sets;
    std::vector<Bonus> bonuses;
    std::vector<Level> levels;
    std::vector<Reward> rewards;
};

namespace detail {
inline void PutInt(std::string& out, int v) { out.append(reinterpret_cast<const char*>(&v), 4); }
inline void PutLong(std::string& out, long long v) { out.append(reinterpret_cast<const char*>(&v), 8); }
inline void PutText(std::string& out, const std::string& s, std::size_t size) {
    std::string field(size, '\0');
    std::memcpy(&field[0], s.data(), s.size() < size - 1 ? s.size() : size - 1);  // always NUL terminated
    out += field;
}
inline int GetInt(const char*& p) { int v; std::memcpy(&v, p, 4); p += 4; return v; }
inline long long GetLong(const char*& p) { long long v; std::memcpy(&v, p, 8); p += 8; return v; }
inline std::string GetText(const char*& p, std::size_t size) {
    std::size_t n = 0;
    while (n < size && p[n]) ++n;
    std::string s(p, n);
    p += size;
    return s;
}
inline bool ReadAll(const std::string& path, std::string& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
inline bool WriteAll(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}
}  // namespace detail

inline std::string SerializeRunes(const Catalog& c) {
    std::string out;
    for (const auto& r : c.runes) { detail::PutInt(out, r.id); detail::PutInt(out, r.item); detail::PutInt(out, r.icon); detail::PutText(out, r.name, 48); detail::PutText(out, r.itemName, 48); }
    return out;
}
inline std::string SerializeSets(const Catalog& c) {
    std::string out;
    for (const auto& s : c.sets) { detail::PutInt(out, s.id); detail::PutText(out, s.name, 48); for (int i = 0; i < 6; ++i) detail::PutInt(out, s.runes[i]); detail::PutLong(out, s.crones); }
    return out;
}
inline std::string SerializeBonuses(const Catalog& c) {
    std::string out;
    for (const auto& b : c.bonuses) { detail::PutInt(out, b.setId); detail::PutInt(out, b.count); detail::PutInt(out, b.kind); detail::PutInt(out, b.value); detail::PutInt(out, b.divisor); detail::PutInt(out, b.perLevel); }
    return out;
}
inline std::string SerializeLevels(const Catalog& c) {
    std::string out;
    for (const auto& l : c.levels) {
        detail::PutInt(out, l.setId); detail::PutInt(out, l.level); detail::PutLong(out, l.crones); detail::PutInt(out, l.rate); detail::PutInt(out, l.pity);
        for (int i = 0; i < LevelItems; ++i) { detail::PutInt(out, l.items[i].item); detail::PutInt(out, l.items[i].amount); detail::PutText(out, l.items[i].name, 40); }
    }
    return out;
}
inline std::string SerializeRewards(const Catalog& c) {
    std::string out;
    for (const auto& r : c.rewards) { detail::PutInt(out, r.setId); detail::PutInt(out, r.tier); detail::PutInt(out, r.item); detail::PutInt(out, r.amount); detail::PutText(out, r.name, 40); }
    return out;
}

// Writes the five files into `dir` (which must exist).
inline bool Write(const std::string& dir, const Catalog& c) {
    return detail::WriteAll(dir + "/cb_rune.bin", SerializeRunes(c)) && detail::WriteAll(dir + "/cb_set.bin", SerializeSets(c)) &&
           detail::WriteAll(dir + "/cb_bonus.bin", SerializeBonuses(c)) && detail::WriteAll(dir + "/cb_level.bin", SerializeLevels(c)) &&
           detail::WriteAll(dir + "/cb_reward.bin", SerializeRewards(c));
}

// Reads the five files from `dir`. A missing file is an empty table; a file whose size is not a whole number of records is an error
// (`error` names it). The rune and set files are required to be present.
inline bool Read(const std::string& dir, Catalog& out, std::string& error) {
    Catalog c;
    std::string data;
    const auto check = [&](const char* name, std::size_t record, bool required) {
        data.clear();
        if (!detail::ReadAll(dir + "/" + name, data)) { if (required) error = std::string(name) + " not found"; return !required; }
        if (data.size() % record) { error = std::string(name) + " has a size that is not a multiple of " + std::to_string(record); return false; }
        return true;
    };
    if (!check("cb_rune.bin", RuneRecord, true)) return false;
    for (const char* p = data.data(); p < data.data() + data.size();) {
        Rune r; r.id = detail::GetInt(p); r.item = detail::GetInt(p); r.icon = detail::GetInt(p); r.name = detail::GetText(p, 48); r.itemName = detail::GetText(p, 48);
        c.runes.push_back(std::move(r));
    }
    if (!check("cb_set.bin", SetRecord, true)) return false;
    for (const char* p = data.data(); p < data.data() + data.size();) {
        Set s; s.id = detail::GetInt(p); s.name = detail::GetText(p, 48); for (int i = 0; i < 6; ++i) s.runes[i] = detail::GetInt(p); s.crones = detail::GetLong(p);
        c.sets.push_back(std::move(s));
    }
    if (!check("cb_bonus.bin", BonusRecord, false)) return false;
    for (const char* p = data.data(); p < data.data() + data.size();) {
        Bonus b; b.setId = detail::GetInt(p); b.count = detail::GetInt(p); b.kind = detail::GetInt(p); b.value = detail::GetInt(p); b.divisor = detail::GetInt(p); b.perLevel = detail::GetInt(p);
        c.bonuses.push_back(b);
    }
    if (!check("cb_level.bin", LevelRecord, false)) return false;
    for (const char* p = data.data(); p < data.data() + data.size();) {
        Level l; l.setId = detail::GetInt(p); l.level = detail::GetInt(p); l.crones = detail::GetLong(p); l.rate = detail::GetInt(p); l.pity = detail::GetInt(p);
        for (int i = 0; i < LevelItems; ++i) { l.items[i].item = detail::GetInt(p); l.items[i].amount = detail::GetInt(p); l.items[i].name = detail::GetText(p, 40); }
        c.levels.push_back(std::move(l));
    }
    if (!check("cb_reward.bin", RewardRecord, false)) return false;
    for (const char* p = data.data(); p < data.data() + data.size();) {
        Reward r; r.setId = detail::GetInt(p); r.tier = detail::GetInt(p); r.item = detail::GetInt(p); r.amount = detail::GetInt(p); r.name = detail::GetText(p, 40);
        c.rewards.push_back(std::move(r));
    }
    out = std::move(c);
    return true;
}

// The references and ranges the files must respect (the same rules the server catalog loader enforces). Returns one line per problem.
inline std::vector<std::string> Validate(const Catalog& c) {
    std::vector<std::string> p;
    std::set<int> runeIds, runeItems, setIds;
    for (const auto& r : c.runes) {
        if (r.id < 1 || r.id > 900000) p.push_back("cb_rune: ID " + std::to_string(r.id) + " must be between 1 and 900000");
        if (!runeIds.insert(r.id).second) p.push_back("cb_rune: ID " + std::to_string(r.id) + " is used twice");
        if (r.item <= 0) p.push_back("cb_rune: rune " + std::to_string(r.id) + " needs an Item");
        else if (!runeItems.insert(r.item).second) p.push_back("cb_rune: item " + std::to_string(r.item) + " unlocks more than one rune");
        if (r.name.empty()) p.push_back("cb_rune: rune " + std::to_string(r.id) + " needs a Name");
    }
    for (const auto& s : c.sets) {
        if (s.id < 1 || s.id > 999999) p.push_back("cb_set: ID " + std::to_string(s.id) + " must be between 1 and 999999");
        if (!setIds.insert(s.id).second) p.push_back("cb_set: ID " + std::to_string(s.id) + " is used twice");
        std::set<int> used;
        int filled = 0;
        for (int i = 0; i < 6; ++i) {
            if (!s.runes[i]) continue;
            ++filled;
            if (!runeIds.count(s.runes[i])) p.push_back("cb_set: set " + std::to_string(s.id) + " slot " + std::to_string(i) + " uses rune " + std::to_string(s.runes[i]) + ", which is not in cb_rune");
            if (!used.insert(s.runes[i]).second) p.push_back("cb_set: set " + std::to_string(s.id) + " lists rune " + std::to_string(s.runes[i]) + " twice");
        }
        if (!filled) p.push_back("cb_set: set " + std::to_string(s.id) + " has no rune");
        if (s.name.empty()) p.push_back("cb_set: set " + std::to_string(s.id) + " needs a Name");
    }
    for (const auto& b : c.bonuses) {
        if (!setIds.count(b.setId)) p.push_back("cb_bonus: set " + std::to_string(b.setId) + " is not in cb_set");
        if (b.count < 1 || b.count > 6) p.push_back("cb_bonus: set " + std::to_string(b.setId) + ": UnlockedCount must be 1 to 6");
        if (b.kind < 1 || b.kind > 4) p.push_back("cb_bonus: set " + std::to_string(b.setId) + ": Kind must be 1 VIT, 2 STR, 3 DEX or 4 AGI");
        if (b.divisor < 1) p.push_back("cb_bonus: set " + std::to_string(b.setId) + ": LevelDivisor must be at least 1");
    }
    std::set<std::pair<int, int>> levels, tiers;
    for (const auto& l : c.levels) {
        if (!setIds.count(l.setId)) p.push_back("cb_level: set " + std::to_string(l.setId) + " is not in cb_set");
        if (l.level < 1 || l.level > 15) p.push_back("cb_level: set " + std::to_string(l.setId) + ": Level must be 1 to 15");
        if (!levels.insert({l.setId, l.level}).second) p.push_back("cb_level: set " + std::to_string(l.setId) + " level " + std::to_string(l.level) + " is listed twice");
        if (l.rate < 0 || l.rate > 100000) p.push_back("cb_level: set " + std::to_string(l.setId) + " level " + std::to_string(l.level) + ": Rate must be 0 to 100000");
    }
    for (const auto& r : c.rewards) {
        if (!setIds.count(r.setId)) p.push_back("cb_reward: set " + std::to_string(r.setId) + " is not in cb_set");
        if (r.tier < 1 || r.tier > 7) p.push_back("cb_reward: set " + std::to_string(r.setId) + ": Tier must be 1 (activation), 2 to 6 or 7 (completion)");
        if (!tiers.insert({r.setId, r.tier}).second) p.push_back("cb_reward: set " + std::to_string(r.setId) + " tier " + std::to_string(r.tier) + " is listed twice");
    }
    return p;
}

}  // namespace CollectionBin
