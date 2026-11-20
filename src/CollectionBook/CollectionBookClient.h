#pragma once
#include "../../shared/CollectionProtocol.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Client side of the Collection Book protocol (see shared/CollectionProtocol.h): a one-request-at-a-time queue, polling,
// reply parsing and the data the book window shows. No game or Windows dependencies, so it is unit tested on its own
// (tests/CollectionBookClientTests.cpp). The window code reads it and calls the Request* functions.
namespace CollectionBookClient {

struct SetView {
    int id = 0;
    unsigned category = 0;
    unsigned owned = 0;                 // bit n = slot n registered
    std::string name;
    unsigned items[CollectionWire::SlotsPerSet]{};
    std::vector<std::string> effect;    // filled by an `info` reply
    bool effectLoaded = false;
    std::vector<std::string> upgrade;   // the Upgrade tab text (fx index 1)
    bool upgradeLoaded = false;
    std::vector<std::string> rewards;   // the Rewards tab text (fx index 2)
    bool rewardsLoaded = false;
};

// Number of the entry's icon slots (runes of a set, or 1 for a rune) and how many of them are unlocked.
inline unsigned Slots(const SetView& set) { unsigned n = 0; for (unsigned i = 0; i < CollectionWire::SlotsPerSet; ++i) n += set.items[i] ? 1 : 0; return n; }
inline unsigned Unlocked(const SetView& set) { unsigned n = 0; for (unsigned i = 0; i < CollectionWire::SlotsPerSet; ++i) n += (set.items[i] && (set.owned & (1u << i))) ? 1 : 0; return n; }
inline bool IsRune(const SetView& set) { return set.id >= CollectionWire::RuneBase; }

// Every item the entry defines is unlocked.
inline bool Complete(const SetView& set) {
    bool any = false;
    for (unsigned i = 0; i < CollectionWire::SlotsPerSet; ++i) {
        if (!set.items[i]) continue;
        any = true;
        if (!(set.owned & (1u << i))) return false;
    }
    return any;
}
inline bool Active(const SetView& set) { return (set.owned & CollectionWire::ActiveBit) != 0; }   // equipped
inline unsigned Level(const SetView& set) { return (set.owned >> CollectionWire::LevelShift) & CollectionWire::MaxLevel; }
inline bool Owns(const SetView& set) { return (set.owned & CollectionWire::OwnBit) != 0; }       // activated

// Decompose tab: the decomposable items of the bag (5 per page), as the server counts them.
struct DecompItem { unsigned item = 0, count = 0; std::string name; };
struct DecompPage {
    unsigned page = 0, pages = 1, count = 0;
    std::vector<DecompItem> items;
    bool loaded = false;
};

// What one disassembly gave (shown in the results popup).
struct Reward { unsigned item = 0; int amount = 0; std::string name; };

struct Page {
    unsigned category = 1, page = 0, pages = 1, count = 0;
    std::vector<SetView> sets;          // at most CollectionWire::PageSize
    bool loaded = false;
};

class Client {
public:
    // Queue a request. Requests are sent one at a time, at least 600 ms apart (the server throttles per character).
    void RequestList(unsigned category, unsigned page);
    void RequestInfo(int setId);
    // Registers every item of the set that is not registered yet, one slot after the other; slots whose item is not in the
    // inventory are skipped by the server (error 8), so one call registers everything the player holds.
    void RequestRegisterAll(int setId);
    // Equips or unequips a completed set (the server checks completeness).
    void RequestEquip(int setId, bool equip);
    // Activates (owns) a rune set: the server takes its Crone cost once.
    void RequestActivate(int setId);
    // Upgrades an activated set by one level (the server takes the materials and Crones, then rolls).
    void RequestUpgrade(int setId);
    // Claims the next ready reward of an activated set (it arrives in the Event Inventory).
    void RequestClaim(int setId);
    // Decompose tab: the list of decomposable items in the bag, and taking apart `mode` 1 (one item) or 2 (thirty) of one kind.
    void RequestDList(unsigned page);
    void RequestDisassemble(unsigned item, unsigned mode);
    // Shows a message in the status line (used for button clicks that need no server request).
    void Notify(const std::string& text) { SetStatus(text); }
    void Reset();                       // new session: drop queue, data and in-flight request

    // Called every frame with a monotonic millisecond clock: sends the next request, polls, times out.
    void Tick(std::uint64_t now);
    // Lines the bridge must send as Say (already `.cbk1 ...`), oldest first.
    std::vector<std::string> TakeCommands();
    // One server line (`CBK1 ...`).
    void Receive(const std::string& line);

    const Page& CurrentPage() const { return page_; }
    const DecompPage& Decomp() const { return decomp_; }
    // True once for every finished disassembly that gave something; `out` receives its rewards.
    bool TakeRewards(std::vector<Reward>& out) {
        if (!rewardsReady_) return false;
        out = std::move(ready_);
        ready_.clear();
        rewardsReady_ = false;
        return true;
    }
    SetView* FindSet(int id);
    const std::string& Status() const { return status_; }
    bool Busy() const { return inFlight_.active || !queue_.empty(); }
    unsigned Version() const { return version_; }   // grows whenever page data, effects or the status text change

private:
    enum class Kind { List, Info, Register, Equip, Activate, Upgrade, Claim, DList, Disassemble };
    struct Req { Kind kind = Kind::List; unsigned category = 1, page = 0; int set = 0; unsigned slot = 0; bool batch = false; };
    struct InFlight { bool active = false; std::uint32_t nonce = 0; Req req; std::uint64_t sent = 0, lastPoll = 0;
                      unsigned expectedSets = 0, gotSets = 0, gotItems = 0; };
    struct Batch { unsigned attempts = 0, registered = 0, missing = 0, other = 0; bool open = false; };

    void Finish(std::uint64_t now);
    void Fail(const std::string& text, std::uint64_t now);
    void Touch() { ++version_; }
    void SetStatus(const std::string& text);
    void CloseBatch();
    std::uint32_t NextNonce();

    std::deque<Req> queue_;
    InFlight inFlight_;
    Page page_, building_;
    DecompPage decomp_, decompBuilding_;
    unsigned decompExpected_ = 0, disExpected_ = 0;
    std::vector<std::string> disGot_;
    std::vector<Reward> disRewards_, ready_;
    bool rewardsReady_ = false;
    std::vector<std::string> commands_;
    std::string status_;
    Batch batch_;
    std::uint64_t lastFinish_ = 0, now_ = 0;
    std::uint32_t nonce_ = 0;
    unsigned version_ = 0;
};

// The window code uses one shared instance (declared here, defined in CollectionBookClient.cpp).
Client& Shared();

// Optional diagnostics: every command sent, every server line received and every status change is passed to the logger.
using Logger = void (*)(const char*);
void SetLogger(Logger logger);
}
