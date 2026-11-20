#pragma once
#include "../../shared/CollectionBinFormat.h"
#include "../../shared/CollectionProtocol.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

// The client's own copy of the Collection Book catalog: read from data\cb_*.bin (written by tools\CollectionBinTool on the server from
// the YAML catalog, edited with the bin editor) and turned into the lists and texts the window shows. The server only sends what a
// character owns (`.cbk1 mine`) and does the actions; nothing about names, costs or texts travels over the network any more.
// The text composition follows what the server's SQL catalog and Policy.h produced before, so both look the same.
// No game or Windows dependencies (unit tested in tests/CollectionBookClientTests.cpp).
namespace CollectionBookClient {

class LocalCatalog {
public:
    struct Entry {
        int id = 0;                    // set id, or CollectionWire::RuneBase + rune id
        unsigned category = 1;         // 1 rune sets, 2 runes
        std::string name;
        unsigned items[CollectionWire::SlotsPerSet]{};   // icon items (a rune is its own slot 0)
    };

    // Reads the five files; false with `error` when one is unreadable or the references do not fit (CollectionBin::Validate).
    bool Load(const std::string& dir, std::string& error) {
        CollectionBin::Catalog c;
        if (!CollectionBin::Read(dir, c, error)) return false;
        const auto problems = CollectionBin::Validate(c);
        if (!problems.empty()) { error = problems.front() + (problems.size() > 1 ? " (+" + std::to_string(problems.size() - 1) + " more)" : ""); return false; }
        if (c.sets.empty() && c.runes.empty()) { error = "the Collection Book files are empty"; return false; }
        bin_ = std::move(c);
        entries_.clear();
        for (const auto& s : bin_.sets) {
            Entry e;
            e.id = s.id; e.category = 1; e.name = s.name;
            for (unsigned i = 0; i < CollectionWire::SlotsPerSet; ++i) if (s.runes[i]) e.items[i] = static_cast<unsigned>(IconOf(s.runes[i]));
            entries_.push_back(std::move(e));
        }
        for (const auto& r : bin_.runes) {
            Entry e;
            e.id = CollectionWire::RuneBase + r.id; e.category = 2; e.name = r.itemName.empty() ? r.name : r.itemName;
            e.items[0] = static_cast<unsigned>(r.icon ? r.icon : r.item);
            entries_.push_back(std::move(e));
        }
        loaded_ = true;
        return true;
    }
    bool Loaded() const { return loaded_; }

    // The entries of one page (CollectionWire::PageSize per page) of a category; `page` is clamped to the last page.
    std::vector<Entry> PageOf(unsigned category, unsigned& page, unsigned& pages, unsigned& count) const {
        std::vector<const Entry*> all;
        for (const auto& e : entries_) if (e.category == category) all.push_back(&e);
        count = static_cast<unsigned>(all.size());
        pages = (std::max)(1u, (count + CollectionWire::PageSize - 1) / CollectionWire::PageSize);
        page = (std::min)(page, pages - 1);
        std::vector<Entry> out;
        for (unsigned i = 0; i < CollectionWire::PageSize; ++i) {
            const unsigned at = page * CollectionWire::PageSize + i;
            if (at >= count) break;
            out.push_back(*all[at]);
        }
        return out;
    }

    // [Activation Cost] / [Requirements] / [Bonus Reward] of a set, or [Requirement] / [Reward] of a rune.
    std::vector<std::string> Effect(int id) const {
        if (id >= CollectionWire::RuneBase) {
            const auto* r = FindRune(id - CollectionWire::RuneBase);
            if (!r) return {};
            return Wrapped("[Requirement]\n" + std::to_string(r->item) + " : " + r->itemName + "\n\n[Reward]\nUnlocked " + r->name + " (Rune ID: " + std::to_string(r->id) + ")");
        }
        const auto* set = FindSet(id);
        if (!set) return {};
        std::string text = "[Activation Cost]\n" + Grouped(set->crones) + " Crones\n\n[Requirements]";
        for (int i = 0; i < 6; ++i) {
            if (!set->runes[i]) continue;
            const auto* r = FindRune(set->runes[i]);
            text += "\n" + (r ? r->name : std::string("?")) + " (Rune ID: " + std::to_string(set->runes[i]) + ")";
        }
        text += "\n\n[Bonus Reward]\n";
        std::string bonus;
        for (int pieces = 1; pieces <= 6; ++pieces) {
            std::string line;
            for (int kind = 1; kind <= 4; ++kind)
                for (const auto& b : bin_.bonuses) {
                    if (b.setId != id || b.count != pieces || b.kind != kind) continue;
                    if (!line.empty()) line += ", ";
                    line += std::string(KindName(kind)) + " +" + std::to_string(b.value);
                    if (b.perLevel > 0) line += " (+" + std::to_string(b.perLevel) + (b.divisor > 1 ? " per " + std::to_string(b.divisor) + " Lv)" : std::string(" per Lv)"));
                }
            if (line.empty()) continue;
            if (!bonus.empty()) bonus += "\n";
            bonus += std::to_string(pieces) + (pieces == 1 ? " Rune: " : " Runes: ") + line;
        }
        text += bonus.empty() ? "(none)" : bonus;
        if (text.size() > 1000) text.resize(1000);
        return Wrapped(text);
    }

    // The refine section for a character at `level` with `unlocked` runes.
    std::vector<std::string> Upgrade(int id, unsigned level, unsigned unlocked) const {
        if (id >= CollectionWire::RuneBase) return {"[Refine Level]", "Runes cannot be refined."};
        if (!AnyLevel(id)) return {"[Refine Level]", "This set has no refine levels."};
        std::vector<std::string> out{"[Refine Level]", "Current level: +" + std::to_string(level)};
        const CollectionBin::Level* next = FindLevel(id, static_cast<int>(level) + 1);
        if (!next) { out.push_back("Maximum level reached."); return out; }
        out.push_back("Next level: +" + std::to_string(next->level));
        out.push_back("Success rate: " + std::to_string(next->rate / 1000) + "." + std::to_string((next->rate % 1000) / 100) + "%");
        if (next->pity > 0) out.push_back("Each failure raises the next chance.");
        int gain[5]{};
        for (const auto& b : bin_.bonuses) {
            if (b.setId != id || b.perLevel == 0 || b.count > static_cast<int>(unlocked) || b.kind < 1 || b.kind > 4) continue;
            const int divisor = b.divisor < 1 ? 1 : b.divisor;
            gain[b.kind] += (next->level / divisor - (next->level - 1) / divisor) * b.perLevel;
        }
        std::string stats;
        for (int kind = 1; kind <= 4; ++kind) {
            if (!gain[kind]) continue;
            if (!stats.empty()) stats += ", ";
            stats += std::string(KindName(kind)) + (gain[kind] > 0 ? " +" : " ") + std::to_string(gain[kind]);
        }
        out.push_back("Stats gained: " + (stats.empty() ? std::string("none") : stats));
        out.push_back("");
        out.push_back("Cost");
        if (next->crones > 0) out.push_back(Grouped(next->crones) + " Crones");
        for (const auto& item : next->items) if (item.item && item.amount > 0) out.push_back(item.name + " x" + std::to_string(item.amount));
        Clip(out);
        return out;
    }

    // The Rewards tab: two lines per tier (state and tier, then the item).
    std::vector<std::string> Rewards(int id, unsigned mask, unsigned claimedBits) const {
        if (id >= CollectionWire::RuneBase) return {"Runes have no rewards."};
        const auto* set = FindSet(id);
        std::vector<const CollectionBin::Reward*> tiers;
        for (const auto& r : bin_.rewards) if (r.setId == id) tiers.push_back(&r);
        std::sort(tiers.begin(), tiers.end(), [](const CollectionBin::Reward* a, const CollectionBin::Reward* b) { return a->tier < b->tier; });
        if (!set || tiers.empty()) return {"No rewards for this set."};
        unsigned slots = 0, have = 0;
        for (unsigned i = 0; i < CollectionWire::SlotsPerSet; ++i) {
            if (!set->runes[i]) continue;
            ++slots;
            if (mask & (1u << i)) ++have;
        }
        const bool owned = (mask & CollectionWire::OwnBit) != 0;
        std::vector<std::string> out;
        for (const auto* reward : tiers) {
            std::string label;
            bool ready = false;
            if (reward->tier == 1) { label = "Activation"; ready = owned; }
            else if (reward->tier == 7) { label = "Completion"; ready = slots > 0 && have == slots; }
            else { label = std::to_string(reward->tier) + " Runes"; ready = have >= static_cast<unsigned>(reward->tier); }
            const std::string state = (claimedBits & (1u << reward->tier)) ? "[Claimed] " : (owned && ready ? "[Ready] " : "[Locked] ");
            out.push_back(state + label);
            out.push_back(reward->name + " x" + std::to_string(reward->amount));
        }
        Clip(out);
        return out;
    }

private:
    static const char* KindName(int kind) {
        switch (kind) { case 1: return "VIT"; case 2: return "STR"; case 3: return "DEX"; case 4: return "AGI"; default: return "?"; }
    }
    static std::string Grouped(long long value) {
        const std::string digits = std::to_string(value < 0 ? 0 : value);
        std::string out;
        for (std::size_t i = 0; i < digits.size(); ++i) {
            if (i && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return out;
    }
    static std::vector<std::string> Wrapped(const std::string& text) { return CollectionWire::SplitEffect(text); }
    static void Clip(std::vector<std::string>& lines) {
        for (auto& line : lines) if (line.size() > CollectionWire::MaxLine) line.resize(CollectionWire::MaxLine);
        if (lines.size() > CollectionWire::MaxEffectLines) lines.resize(CollectionWire::MaxEffectLines);
    }
    const CollectionBin::Rune* FindRune(int id) const {
        for (const auto& r : bin_.runes) if (r.id == id) return &r;
        return nullptr;
    }
    const CollectionBin::Set* FindSet(int id) const {
        for (const auto& s : bin_.sets) if (s.id == id) return &s;
        return nullptr;
    }
    int IconOf(int runeId) const {
        const auto* r = FindRune(runeId);
        return r ? (r->icon ? r->icon : r->item) : 0;
    }
    bool AnyLevel(int id) const {
        for (const auto& l : bin_.levels) if (l.setId == id) return true;
        return false;
    }
    const CollectionBin::Level* FindLevel(int id, int level) const {
        for (const auto& l : bin_.levels) if (l.setId == id && l.level == level) return &l;
        return nullptr;
    }

    CollectionBin::Catalog bin_;
    std::vector<Entry> entries_;
    bool loaded_ = false;
};

}
