#pragma once
#include "InspectProtocol.h"   // Words, Number, Hex, UnhexText

// Costume Effect private Say transport (version 3: the effects sent are those of the selected costume). Mirrored byte for byte in RohanClient_Hook/shared/CostumeEffectProtocol.h.
//
// Client -> server (normal Say, consumed by the hook):   .cos1 list <nonce> <selected item id, 0 = any>
//                                                        .cos1 make <nonce> <costume item id> <result costume type>
// Server -> client (0x1F04 with sender "[Costume]"), a list request is answered in order:
//   COS1 head <nonce> <ticket type> <tickets held> <selected item id> <its base costume type> <recipes> <copies>
//        selected = the asked-for copy, else the first usable one; 0 and 0 when the bags hold no listed costume
//   COS1 recipe <nonce> <index> <result type> <hex name>                    one per effect of the selected costume
//   COS1 copy <nonce> <index> <item id> <costume type> <status>     the base costume or one that already has an effect;
//                                                          status: 0 usable, 1 has patterns, 2 has other options
//   COS1 done <nonce>
// A make request (or a list that cannot be answered) gets:  COS1 result <nonce> <code> <result type>
namespace CostumeWire {
inline constexpr std::size_t MaxLine = 160, MaxRecipes = 5, MaxCopies = 8, MaxName = 32;
inline constexpr std::string_view AskPrefix = ".cos1 ";
inline constexpr std::string_view ReplyPrefix = "COS1 ";
inline constexpr std::string_view Source = "[Costume]";

enum Code {
    Done = 0, NoTicket = 1, NoEffect = 2, NoCostume = 3, HasPatterns = 4, HasOptions = 5, InventoryFull = 6,
    Database = 7, KioskOpen = 8, Busy = 9, DeliveryFailed = 10, Disabled = 11, Failed = 12, SameEffect = 13, LastCode = 13
};
enum CopyStatus { Usable = 0, Patterns = 1, Options = 2 };

struct Request {
    bool make = false;
    std::uint32_t nonce = 0;
    int itemId = 0;           // make: the costume to change; list: the selected copy, 0 = any
    unsigned effect = 0;      // the result costume type
};
inline std::string BuildList(std::uint32_t nonce, int itemId = 0) {
    return ".cos1 list " + std::to_string(nonce) + " " + std::to_string(itemId);
}
inline std::string BuildMake(std::uint32_t nonce, int itemId, unsigned effect) {
    return ".cos1 make " + std::to_string(nonce) + " " + std::to_string(itemId) + " " + std::to_string(effect);
}
inline bool ParseRequest(std::string_view text, Request& output) {
    if (text.size() > MaxLine) return false;
    const auto w = InspectWire::Words(text);
    Request r;
    if (w.size() < 3 || w[0] != ".cos1" || !InspectWire::Number(w[2], r.nonce) || !r.nonce) return false;
    if (w[1] == "list") {
        if (w.size() != 4 || !InspectWire::Number(w[3], r.itemId) || r.itemId < 0) return false;
        output = r;
        return true;
    }
    if (w[1] != "make" || w.size() != 5 || !InspectWire::Number(w[3], r.itemId) || r.itemId <= 0 ||
        !InspectWire::Number(w[4], r.effect) || !r.effect) return false;
    r.make = true;
    output = r;
    return true;
}

struct Recipe {
    unsigned result = 0;
    std::string name;
};
struct Copy {
    int itemId = 0;
    unsigned type = 0;
    int status = Usable;
};
struct State {
    std::uint32_t nonce = 0;
    unsigned ticket = 0, tickets = 0;
    int selected = 0;
    unsigned base = 0;
    std::size_t recipeCount = 0, copyCount = 0;
    std::vector<Recipe> recipes;
    std::vector<Copy> copies;
};
inline std::vector<std::string> BuildState(const State& s) {
    const std::string n = std::to_string(s.nonce);
    std::vector<std::string> out;
    const std::size_t recipes = s.recipes.size() < MaxRecipes ? s.recipes.size() : MaxRecipes;
    const std::size_t copies = s.copies.size() < MaxCopies ? s.copies.size() : MaxCopies;
    out.push_back("COS1 head " + n + " " + std::to_string(s.ticket) + " " + std::to_string(s.tickets) + " " + std::to_string(s.selected) +
                  " " + std::to_string(s.base) + " " + std::to_string(recipes) + " " + std::to_string(copies));
    for (std::size_t i = 0; i < recipes; ++i) {
        const auto& r = s.recipes[i];
        out.push_back("COS1 recipe " + n + " " + std::to_string(i) + " " + std::to_string(r.result) + " " +
                      InspectWire::Hex(r.name.substr(0, MaxName)));
    }
    for (std::size_t i = 0; i < copies; ++i)
        out.push_back("COS1 copy " + n + " " + std::to_string(i) + " " + std::to_string(s.copies[i].itemId) + " " +
                      std::to_string(s.copies[i].type) + " " + std::to_string(s.copies[i].status));
    out.push_back("COS1 done " + n);
    return out;
}
inline std::string BuildResult(std::uint32_t nonce, int code, unsigned result = 0) {
    return "COS1 result " + std::to_string(nonce) + " " + std::to_string(code) + " " + std::to_string(result);
}

// Client: collects the lines of one list answer. Lines must arrive in order. Returns true when `line` completed the state.
inline bool ApplyState(State& s, std::string_view line) {
    if (line.size() > MaxLine) return false;
    const auto w = InspectWire::Words(line);
    std::uint32_t nonce = 0;
    if (w.size() < 3 || w[0] != "COS1" || !InspectWire::Number(w[2], nonce) || nonce != s.nonce) return false;
    if (w[1] == "head" && w.size() == 9) {
        State next;
        next.nonce = nonce;
        if (!InspectWire::Number(w[3], next.ticket) || !InspectWire::Number(w[4], next.tickets) || !InspectWire::Number(w[5], next.selected) ||
            !InspectWire::Number(w[6], next.base) || !InspectWire::Number(w[7], next.recipeCount) ||
            !InspectWire::Number(w[8], next.copyCount) || next.selected < 0 ||
            next.recipeCount > MaxRecipes || next.copyCount > MaxCopies) return false;
        s = std::move(next);
        return false;
    }
    std::size_t index = 0;
    if (w[1] == "recipe" && w.size() == 6) {
        Recipe r;
        if (!InspectWire::Number(w[3], index) || index != s.recipes.size() || index >= s.recipeCount ||
            !InspectWire::Number(w[4], r.result) || !r.result || !InspectWire::UnhexText(w[5], r.name, MaxName)) return false;
        s.recipes.push_back(std::move(r));
        return false;
    }
    if (w[1] == "copy" && w.size() == 7) {
        Copy c;
        if (!InspectWire::Number(w[3], index) || index != s.copies.size() || index >= s.copyCount ||
            !InspectWire::Number(w[4], c.itemId) || !InspectWire::Number(w[5], c.type) || !c.type ||
            !InspectWire::Number(w[6], c.status) || c.status < 0 || c.status > Options) return false;
        s.copies.push_back(c);
        return false;
    }
    return w[1] == "done" && w.size() == 3 && s.ticket && s.recipes.size() == s.recipeCount && s.copies.size() == s.copyCount;
}
inline bool ParseResult(std::string_view line, std::uint32_t& nonce, int& code, unsigned& result) {
    const auto w = InspectWire::Words(line);
    return w.size() == 5 && w[0] == "COS1" && w[1] == "result" && InspectWire::Number(w[2], nonce) && InspectWire::Number(w[3], code) &&
           code >= 0 && code <= LastCode && InspectWire::Number(w[4], result);
}
}  // namespace CostumeWire
