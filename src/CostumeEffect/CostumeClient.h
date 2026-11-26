#pragma once
#include "../../shared/CostumeEffectProtocol.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>

// What the Costume Effect window knows and may do. No drawing here, so Bridge.cpp and the tests can use it by itself.
class CostumeClient {
public:
    // The bag item in the window's slot, as the client knew it when it was dropped in. itemId 0 = the slot is empty.
    struct Item {
        int itemId = 0;
        unsigned type = 0;
        int bag = 0, slot = 0;
    };
    static constexpr ULONGLONG TimeoutMs = 10000;
    static constexpr ULONGLONG RetryGapMs = 700;      // the server takes one request each half second

    static CostumeClient& Shared() { static CostumeClient client; return client; }

    void Open(ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        visible = true;
        status.clear();
        copy = 0;
        effect = 0;
        wanted = 0;
        placed = {};
        confirming = false;
        Reload(now);
    }
    void Close() { std::lock_guard<std::mutex> guard(lock); visible = false; confirming = false; placed = {}; }
    void Reset() {
        std::lock_guard<std::mutex> guard(lock);
        visible = loaded = loading = combining = confirming = false;
        state = {};
        incoming = {};
        commands.clear();
        status.clear();
        copy = 0;
        effect = 0;
        reloadAt = 0;
        wanted = 0;
        placed = {};
    }
    bool Visible() const { std::lock_guard<std::mutex> guard(lock); return visible; }

    void Receive(const std::string& line, ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        std::uint32_t replyNonce = 0;
        int code = 0;
        unsigned result = 0;
        if (CostumeWire::ParseResult(line, replyNonce, code, result)) {
            if (replyNonce != nonce || (!loading && !combining)) return;
            const bool wasCombining = combining;
            loading = combining = confirming = false;
            status = Message(code);
            if (wasCombining) {                       // show what is left either way, keeping the message
                copy = 0;
                effect = 0;
                wanted = 0;
                placed = {};
                reloadAt = now + RetryGapMs;
            }
            return;
        }
        if (!loading || !CostumeWire::ApplyState(incoming, line)) return;
        state = incoming;
        loading = false;
        loaded = true;
        // Only the costume in the slot can be changed, and only once the server accepted it.
        copy = placed.itemId && state.selected == placed.itemId && FindUsable(placed.itemId) ? placed.itemId : 0;
        if (!Recipe(effect) || effect == CopyType(copy)) effect = 0;
        if (placed.itemId && !copy) {
            const CostumeWire::Copy* listed = nullptr;
            for (const auto& c : state.copies)
                if (c.itemId == placed.itemId) listed = &c;
            if (!listed) {
                status = "That item can't have a Costume Effect.";
                placed = {};
            } else {                                     // a costume that can be looked at but not changed
                status = Message(listed->status == CostumeWire::Patterns ? CostumeWire::HasPatterns : CostumeWire::HasOptions);
            }
        }
    }
    void Tick(ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        if (reloadAt && now >= reloadAt) {
            reloadAt = 0;
            if (visible) Reload(now);
        }
        if ((loading || combining) && now - sentAt > TimeoutMs) {
            status = combining ? "No answer. Check your Event Inventory before trying again."
                               : "No answer from the server. Is Costume Effect enabled?";
            loading = combining = confirming = false;
            loaded = false;
            placed = {};
        }
    }
    std::vector<std::string> TakeCommands() {
        std::lock_guard<std::mutex> guard(lock);
        std::vector<std::string> out;
        out.swap(commands);
        return out;
    }

    // ---- The player's choices ----
    // A bag item dropped on the slot. The server is asked whether it is a listed costume and which effects it has.
    bool Drop(const Item& item, ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        if (!visible || combining || item.itemId <= 0 || !item.type) return false;
        placed = item;
        wanted = item.itemId;
        copy = 0;
        effect = 0;
        confirming = false;
        status.clear();
        loaded = false;
        loading = true;
        sentAt = now;
        incoming = {};
        ++nonce;                                         // an answer still on its way is for the previous slot content
        // Not sooner than the server takes another request.
        reloadAt = now > listSentAt + RetryGapMs ? now : listSentAt + RetryGapMs;
        return true;
    }
    // Takes the costume out of the slot.
    void Remove() {
        std::lock_guard<std::mutex> guard(lock);
        if (combining) return;
        placed = {};
        wanted = 0;
        copy = 0;
        effect = 0;
        confirming = false;
        status.clear();
    }
    // The << and >> buttons. Position 0 is "no change", then the server's effects in order, wrapping around.
    // The effect the chosen costume already has is skipped.
    void StepEffect(int direction) {
        std::lock_guard<std::mutex> guard(lock);
        if (combining || !loaded || !copy || state.recipes.empty()) return;
        const int count = static_cast<int>(state.recipes.size()) + 1;
        int next = EffectPosition();
        do {
            next = (next + (direction < 0 ? count - 1 : 1)) % count;
            effect = next ? state.recipes[static_cast<std::size_t>(next - 1)].result : 0;
        } while (next && effect == CopyType(copy));
        confirming = false;
        status.clear();
    }
    bool CanCombine() const { std::lock_guard<std::mutex> guard(lock); return Ready(); }
    // The first press asks for confirmation; the second sends the request.
    bool Combine(ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        if (!Ready()) return false;
        if (!confirming) { confirming = true; return false; }
        confirming = false;
        combining = true;
        sentAt = now;
        status = "Combining...";
        commands.push_back(CostumeWire::BuildMake(++nonce, copy, effect));
        return true;
    }
    void Refresh(ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        if (!combining && !loading) { status.clear(); Reload(now); }
    }

    struct View {
        bool loaded = false, loading = false, combining = false, confirming = false, ready = false;
        CostumeWire::State state;
        int copy = 0;
        unsigned copyType = 0;         // the chosen costume's item type, 0 = no usable costume
        unsigned effect = 0;           // the chosen result costume type, 0 = none
        int position = 0;              // 0 = none, else 1..number of effects
        std::string status;
        Item placed;                   // what the slot holds
    };
    View Snapshot() const {
        std::lock_guard<std::mutex> guard(lock);
        return {loaded, loading, combining, confirming, Ready(), state, copy, CopyType(copy), effect, EffectPosition(), status, placed};
    }
    static const char* Message(int code) {
        using namespace CostumeWire;
        switch (code) {
        case Done: return "Done. Your new costume is in the Event Inventory.";
        case NoTicket: return "You need a Costume Effect Ticket in your bag.";
        case NoEffect: return "That effect is not available.";
        case NoCostume: return "That costume is not in your bag. Unequip it first.";
        case HasPatterns: return "This costume has socketed patterns and can't be used.";
        case HasOptions: return "This costume has other options or a time limit and can't be used.";
        case InventoryFull: return "Your Event Inventory is full.";
        case Database: return "The server could not check the costume. Nothing was used.";
        case KioskOpen: return "Close your shop first.";
        case Busy: return "Busy. Try again.";
        case DeliveryFailed: return "The items were used but delivery failed. Please contact a GM.";
        case Disabled: return "Costume Effect is turned off.";
        case SameEffect: return "This costume already has that effect.";
        default: return "The combine could not finish. Check your bag and Event Inventory.";
        }
    }
private:
    void Reload(ULONGLONG now) {
        loading = true;
        combining = confirming = false;
        sentAt = now;
        incoming = {};
        incoming.nonce = ++nonce;
        listSentAt = now;
        loaded = false;
        commands.push_back(CostumeWire::BuildList(nonce, wanted));
    }
    bool FindUsable(int itemId) const {
        for (const auto& c : state.copies)
            if (itemId && c.itemId == itemId && c.status == CostumeWire::Usable) return true;
        return false;
    }
    unsigned CopyType(int itemId) const {
        for (const auto& c : state.copies)
            if (itemId && c.itemId == itemId) return c.type;
        return 0;
    }
    const CostumeWire::Recipe* Recipe(unsigned type) const {
        for (const auto& r : state.recipes)
            if (type && r.result == type) return &r;
        return nullptr;
    }
    int EffectPosition() const {
        for (std::size_t i = 0; i < state.recipes.size(); ++i)
            if (effect && state.recipes[i].result == effect) return static_cast<int>(i) + 1;
        return 0;
    }
    bool Ready() const {
        return visible && loaded && !loading && !combining && state.tickets && FindUsable(copy) && Recipe(effect) && effect != CopyType(copy);
    }
    mutable std::mutex lock;
    bool visible = false, loaded = false, loading = false, combining = false, confirming = false;
    CostumeWire::State state, incoming;
    int copy = 0, wanted = 0;             // wanted: the copy whose effects the next list should bring
    ULONGLONG listSentAt = 0;
    Item placed;
    unsigned effect = 0;
    std::uint32_t nonce = static_cast<std::uint32_t>(GetTickCount() & 0xffffff);
    ULONGLONG sentAt = 0, reloadAt = 0;
    std::string status;
    std::vector<std::string> commands;
};
