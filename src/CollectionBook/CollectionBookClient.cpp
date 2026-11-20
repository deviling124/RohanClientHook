#include "CollectionBookClient.h"
#include <algorithm>
#include <chrono>

namespace CollectionBookClient {
namespace {
Logger logger = nullptr;
unsigned logged = 0;
void Log(const std::string& text) {
    if (logger && logged < 400) { ++logged; logger(text.c_str()); }
}
constexpr std::uint64_t MinGap = 250, PollEvery = 250, Timeout = 10000;

std::string ErrorText(int code) {
    switch (code) {
    case 1: return "The Collection Book is disabled on this server.";
    case 3: return "The server is busy. Try again in a moment.";
    case 4: return "The server did not understand the request.";
    case 7: return "Please close and reopen the book.";
    case 8: return "You do not have that item.";
    case 9: return "Close your kiosk first.";
    case 10: return "Enhanced items cannot be used here: use an unmodified item.";
    default: return "Server error. Please try again later.";
    }
}
}

void SetLogger(Logger value) { logger = value; logged = 0; }

Client& Shared() {
    static Client instance;
    return instance;
}

std::uint32_t Client::NextNonce() {
    // Start every run at a different point of the sequence: the server remembers finished requests for two minutes and a client
    // restarted inside that time must not repeat nonces of its previous run.
    if (!nonce_) nonce_ = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
    nonce_ = nonce_ * 1664525u + 1013904223u;
    if (!nonce_) nonce_ = 1;
    return nonce_;
}

void Client::SetStatus(const std::string& text) {
    if (status_ == text) return;
    Log("cbk status: " + text);
    status_ = text;
    Touch();
}

void Client::Reset() {
    queue_.clear();
    inFlight_ = {};
    page_ = {};
    building_ = {};
    decomp_ = {};
    decompBuilding_ = {};
    decompExpected_ = disExpected_ = 0;
    disGot_.clear();
    disRewards_.clear();
    ready_.clear();
    rewardsReady_ = false;
    commands_.clear();
    batch_ = {};
    status_.clear();
    lastFinish_ = 0;
    Touch();
}

void Client::RequestList(unsigned category, unsigned page) {
    // A newer list replaces an older queued one: the player only cares about the page they asked for last.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::List; }), queue_.end());
    Req r;
    r.kind = Kind::List;
    r.category = category;
    r.page = page;
    queue_.push_back(r);
    SetStatus("Loading...");
}

void Client::RequestInfo(int setId) {
    for (const auto& r : queue_) if (r.kind == Kind::Info && r.set == setId) return;
    Req r;
    r.kind = Kind::Info;
    r.set = setId;
    queue_.push_back(r);
}

SetView* Client::FindSet(int id) {
    for (auto& s : page_.sets) if (s.id == id) return &s;
    return nullptr;
}

void Client::RequestRegisterAll(int setId) {
    const SetView* set = FindSet(setId);
    if (!set) return;
    if (batch_.open || std::any_of(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::Register; }) ||
        (inFlight_.active && inFlight_.req.kind == Kind::Register)) return;  // one registration run at a time
    batch_ = {};
    for (unsigned slot = 0; slot < CollectionWire::SlotsPerSet; ++slot) {
        if (!set->items[slot] || (set->owned & (1u << slot))) continue;
        Req r;
        r.kind = Kind::Register;
        r.set = setId;
        r.slot = slot;
        r.batch = true;
        queue_.push_back(r);
        ++batch_.attempts;
    }
    batch_.open = batch_.attempts > 0;
    SetStatus(batch_.open ? "Registering..." : "Everything in this set is already registered.");
}

void Client::RequestEquip(int setId, bool equip) {
    if (!FindSet(setId)) return;
    if (std::any_of(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::Equip; }) ||
        (inFlight_.active && inFlight_.req.kind == Kind::Equip)) return;
    Req r;
    r.kind = Kind::Equip;
    r.set = setId;
    r.slot = equip ? 1 : 0;
    queue_.push_back(r);
    SetStatus(equip ? "Equipping..." : "Unequipping...");
}

void Client::RequestDList(unsigned page) {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::DList; }), queue_.end());
    Req r;
    r.kind = Kind::DList;
    r.page = page;
    queue_.push_back(r);
}

void Client::RequestDisassemble(unsigned item, unsigned mode) {
    const auto busy = [](const Req& r) { return r.kind == Kind::Disassemble; };
    if (!item || (mode != 1 && mode != 2) || std::any_of(queue_.begin(), queue_.end(), busy) || (inFlight_.active && busy(inFlight_.req))) return;
    Req r;
    r.kind = Kind::Disassemble;
    r.set = static_cast<int>(item);
    r.slot = mode;
    queue_.push_back(r);
    SetStatus("Disassembling...");
}

void Client::RequestClaim(int setId) {
    if (!FindSet(setId)) return;
    const auto claim = [](const Req& r) { return r.kind == Kind::Claim; };
    if (std::any_of(queue_.begin(), queue_.end(), claim) || (inFlight_.active && claim(inFlight_.req))) return;
    Req r;
    r.kind = Kind::Claim;
    r.set = setId;
    queue_.push_back(r);
    SetStatus("Claiming...");
}

void Client::RequestUpgrade(int setId) {
    if (!FindSet(setId)) return;
    const auto change = [](const Req& r) { return r.kind == Kind::Activate || r.kind == Kind::Equip || r.kind == Kind::Upgrade; };
    if (std::any_of(queue_.begin(), queue_.end(), change) || (inFlight_.active && change(inFlight_.req))) return;
    Req r;
    r.kind = Kind::Upgrade;
    r.set = setId;
    queue_.push_back(r);
    SetStatus("Upgrading...");
}

void Client::RequestActivate(int setId) {
    if (!FindSet(setId)) return;
    if (std::any_of(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::Activate || r.kind == Kind::Equip; }) ||
        (inFlight_.active && (inFlight_.req.kind == Kind::Activate || inFlight_.req.kind == Kind::Equip))) return;
    Req r;
    r.kind = Kind::Activate;
    r.set = setId;
    queue_.push_back(r);
    SetStatus("Activating...");
}

void Client::CloseBatch() {
    if (!batch_.open) return;
    std::string text = "Registered " + std::to_string(batch_.registered) + " of " + std::to_string(batch_.attempts) + " items";
    if (batch_.missing) text += " (" + std::to_string(batch_.missing) + " not in your inventory)";
    if (batch_.other && !batch_.registered) text = "Could not register. Please try again.";
    SetStatus(text + (batch_.registered || batch_.other ? "." : ""));
    batch_ = {};
}

void Client::Finish(std::uint64_t now) {
    inFlight_ = {};
    lastFinish_ = now;
    if (batch_.open && std::none_of(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::Register; })) CloseBatch();
}

void Client::Fail(const std::string& text, std::uint64_t now) {
    const bool wasRegister = inFlight_.active && inFlight_.req.kind == Kind::Register;
    if (wasRegister) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Req& r) { return r.kind == Kind::Register; }), queue_.end());
        batch_ = {};
    }
    inFlight_ = {};
    lastFinish_ = now;
    SetStatus(text);
}

void Client::Tick(std::uint64_t now) {
    now_ = now;
    if (inFlight_.active) {
        if (now - inFlight_.sent > Timeout) { Fail("No answer from the server.", now); return; }
        if (now - inFlight_.lastPoll >= PollEvery) {
            inFlight_.lastPoll = now;
            commands_.push_back(".cbk1 poll " + std::to_string(inFlight_.nonce));
        }
        return;
    }
    if (queue_.empty() || (lastFinish_ && now - lastFinish_ < MinGap)) return;
    const Req r = queue_.front();
    queue_.pop_front();
    inFlight_ = {};
    inFlight_.active = true;
    inFlight_.nonce = NextNonce();
    inFlight_.req = r;
    inFlight_.sent = inFlight_.lastPoll = now;
    const std::string n = std::to_string(inFlight_.nonce);
    switch (r.kind) {
    case Kind::List: commands_.push_back(".cbk1 list " + n + " " + std::to_string(r.category) + " " + std::to_string(r.page)); break;
    case Kind::Info: commands_.push_back(".cbk1 info " + n + " " + std::to_string(r.set)); break;
    case Kind::DList: commands_.push_back(".cbk1 dlist " + n + " " + std::to_string(r.page)); break;
    case Kind::Disassemble: commands_.push_back(".cbk1 dis " + n + " " + std::to_string(r.set) + " " + std::to_string(r.slot)); break;
    case Kind::Claim: commands_.push_back(".cbk1 claim " + n + " " + std::to_string(r.set)); break;
    case Kind::Upgrade: commands_.push_back(".cbk1 up " + n + " " + std::to_string(r.set)); break;
    case Kind::Activate: commands_.push_back(".cbk1 act " + n + " " + std::to_string(r.set)); break;
    case Kind::Equip: commands_.push_back(".cbk1 eq " + n + " " + std::to_string(r.set) + " " + std::to_string(r.slot)); break;
    case Kind::Register:
        commands_.push_back(".cbk1 reg " + n + " " + std::to_string(r.set) + " " + std::to_string(r.slot));
        break;
    }
}

std::vector<std::string> Client::TakeCommands() {
    std::vector<std::string> out;
    out.swap(commands_);
    for (const auto& command : out) Log("cbk send: " + command);
    return out;
}

void Client::Receive(const std::string& line) {
    Log("cbk recv: " + line);
    const auto w = CollectionWire::Words(line);
    std::uint32_t nonce = 0;
    if (w.size() < 3 || w[0] != "CBK1" || !CollectionWire::Number(w[2], nonce) || !inFlight_.active || nonce != inFlight_.nonce) return;
    const auto kind = w[1];
    if (kind == "wait") return;
    if (kind == "error") {
        int code = 0;
        if (w.size() == 4 && CollectionWire::Number(w[3], code)) {
            const bool batchRun = batch_.open && inFlight_.req.kind == Kind::Register;
            if (code == 8 && batchRun) {            // item not in the inventory: skip this slot, keep going
                ++batch_.missing;
                Finish(now_);
            } else {
                Fail(ErrorText(code), now_);
            }
        }
        return;
    }
    if (kind == "state" && inFlight_.req.kind == Kind::List && w.size() == 7) {
        unsigned category = 0, pageNo = 0, pages = 0, count = 0;
        if (!CollectionWire::Number(w[3], category) || !CollectionWire::Number(w[4], pageNo) ||
            !CollectionWire::Number(w[5], pages) || !CollectionWire::Number(w[6], count) || pages < 1) return;
        building_ = {};
        building_.category = category;
        building_.page = pageNo;
        building_.pages = pages;
        building_.count = count;
        building_.loaded = true;
        const unsigned first = pageNo * CollectionWire::PageSize;
        inFlight_.expectedSets = count > first ? std::min(CollectionWire::PageSize, count - first) : 0;
        inFlight_.gotSets = inFlight_.gotItems = 0;
        building_.sets.assign(inFlight_.expectedSets, SetView{});
        if (!inFlight_.expectedSets) { page_ = building_; Touch(); SetStatus(""); Finish(now_); }
        return;
    }
    if (kind == "set" && inFlight_.req.kind == Kind::List && w.size() == 8) {
        unsigned index = 0, category = 0, owned = 0;
        int id = 0;
        std::string name;
        if (!CollectionWire::Number(w[3], index) || index >= building_.sets.size() || !CollectionWire::Number(w[4], id) ||
            !CollectionWire::Number(w[5], category) || !CollectionWire::Number(w[6], owned) ||
            !CollectionWire::Unhex(w[7], name, CollectionWire::MaxName)) return;
        auto& set = building_.sets[index];
        set.id = id;
        set.category = category;
        set.owned = owned;
        set.name = name;
        ++inFlight_.gotSets;
        return;
    }
    if (kind == "items" && inFlight_.req.kind == Kind::List && w.size() == 4 + CollectionWire::SlotsPerSet) {
        unsigned index = 0;
        if (!CollectionWire::Number(w[3], index) || index >= building_.sets.size()) return;
        for (unsigned i = 0; i < CollectionWire::SlotsPerSet; ++i)
            if (!CollectionWire::Number(w[4 + i], building_.sets[index].items[i])) return;
        if (++inFlight_.gotItems == inFlight_.expectedSets && inFlight_.gotSets == inFlight_.expectedSets) {
            // Keep the effect text of sets that are still on the page; everything else is rebuilt.
            for (auto& s : building_.sets) if (const SetView* old = FindSet(s.id)) { s.effect = old->effect; s.effectLoaded = old->effectLoaded; s.upgrade = old->upgrade; s.upgradeLoaded = old->upgradeLoaded; s.rewards = old->rewards; s.rewardsLoaded = old->rewardsLoaded; }
            page_ = building_;
            Touch();
            SetStatus("");
            Finish(now_);
        }
        return;
    }
    if (kind == "fx" && inFlight_.req.kind == Kind::Info && w.size() == 7) {
        unsigned index = 0, lineNo = 0, lines = 0;
        std::string text;
        if (!CollectionWire::Number(w[3], index) || !CollectionWire::Number(w[4], lineNo) || !CollectionWire::Number(w[5], lines) ||
            index > 2 || lines > CollectionWire::MaxEffectLines || lineNo >= std::max(lines, 1u)) return;
        if (w[6] != "-" && !CollectionWire::Unhex(w[6], text, CollectionWire::MaxLine)) return;
        SetView* set = FindSet(inFlight_.req.set);
        if (!set) { Finish(now_); return; }
        auto& target = index == 0 ? set->effect : index == 1 ? set->upgrade : set->rewards;
        if (lineNo == 0) target.clear();
        if (lines) target.push_back(text);
        if (lineNo + 1 >= lines) {
            if (index == 0) { set->effectLoaded = true; Touch(); }
            else if (index == 1) { set->upgradeLoaded = true; Touch(); }
            else { set->rewardsLoaded = true; Touch(); Finish(now_); }
        }
        return;
    }
    if (kind == "dstate" && inFlight_.req.kind == Kind::DList && w.size() == 6) {
        unsigned pageNo = 0, pages = 0, count = 0;
        if (!CollectionWire::Number(w[3], pageNo) || !CollectionWire::Number(w[4], pages) || !CollectionWire::Number(w[5], count) || pages < 1) return;
        decompBuilding_ = {};
        decompBuilding_.page = pageNo;
        decompBuilding_.pages = pages;
        decompBuilding_.count = count;
        decompBuilding_.loaded = true;
        const unsigned first = pageNo * CollectionWire::PageSize;
        decompExpected_ = count > first ? std::min(CollectionWire::PageSize, count - first) : 0;
        if (!decompExpected_) { decomp_ = decompBuilding_; Touch(); Finish(now_); }
        return;
    }
    if (kind == "ditem" && inFlight_.req.kind == Kind::DList && w.size() == 7) {
        unsigned index = 0, item = 0, count = 0;
        std::string name;
        if (!CollectionWire::Number(w[3], index) || !CollectionWire::Number(w[4], item) || !CollectionWire::Number(w[5], count) ||
            !CollectionWire::Unhex(w[6], name, CollectionWire::MaxName) || !decompExpected_) return;
        decompBuilding_.items.push_back({item, count, name});
        if (decompBuilding_.items.size() >= decompExpected_) { decomp_ = decompBuilding_; decompExpected_ = 0; Touch(); Finish(now_); }
        return;
    }
    if (kind == "dis" && inFlight_.req.kind == Kind::Disassemble && w.size() == 7) {
        int result = 0;
        unsigned item = 0, mode = 0, n = 0;
        if (!CollectionWire::Number(w[3], result) || !CollectionWire::Number(w[4], item) || !CollectionWire::Number(w[5], mode) || !CollectionWire::Number(w[6], n)) return;
        if (result == 0 && n > 0) { disExpected_ = n; disGot_.clear(); disRewards_.clear(); return; }  // the rewards follow
        switch (result) {
        case 0: SetStatus("Nothing came out of it."); break;
        case 1: SetStatus("That item cannot be disassembled."); break;
        case 2: SetStatus(mode == 2 ? "That item cannot be disassembled 30 at a time." : "That item cannot be disassembled one at a time."); break;
        case 6: SetStatus("Your Event Inventory is full: the rewards were lost, contact a GM."); break;
        case 7: SetStatus(mode == 2 ? "You need 30 of that item." : "You do not have that item."); break;
        default: SetStatus("The item could not be disassembled."); break;
        }
        Finish(now_);
        RequestDList(decomp_.page);
        return;
    }
    if (kind == "dgot" && inFlight_.req.kind == Kind::Disassemble && w.size() == 7) {
        unsigned index = 0, item = 0;
        int amount = 0;
        std::string name;
        if (!CollectionWire::Number(w[3], index) || !CollectionWire::Number(w[4], amount) || !CollectionWire::Number(w[5], item) ||
            !CollectionWire::Unhex(w[6], name, CollectionWire::MaxName) || !disExpected_) return;
        disGot_.push_back(name + " x" + std::to_string(amount));
        disRewards_.push_back({item, amount, name});
        if (disGot_.size() >= disExpected_) {
            SetStatus("");  // the results popup shows what was received
            disExpected_ = 0;
            ready_ = disRewards_;
            rewardsReady_ = true;
            Finish(now_);
            RequestDList(decomp_.page);
        }
        return;
    }
    if (kind == "claim" && inFlight_.req.kind == Kind::Claim && w.size() == 7) {
        int result = 0, set = 0, tier = 0;
        unsigned bits = 0;
        if (!CollectionWire::Number(w[3], result) || !CollectionWire::Number(w[4], set) || !CollectionWire::Number(w[5], tier) ||
            !CollectionWire::Number(w[6], bits) || set != inFlight_.req.set || tier > 7 || bits > 255) return;
        if (result == 0) {
            if (SetView* s = FindSet(set)) s->rewardsLoaded = false;
            Touch();
        }
        switch (result) {
        case 0: SetStatus(tier == 1 ? "Activation reward claimed. Check your Event Inventory." :
                          tier == 7 ? "Completion reward claimed. Check your Event Inventory." :
                          std::to_string(tier) + " runes reward claimed. Check your Event Inventory."); break;
        case 1: SetStatus("That set has no rewards."); break;
        case 2: SetStatus("Activate this set first."); break;
        case 3: SetStatus("No reward is ready to claim."); break;
        case 6: SetStatus("Your Event Inventory is full."); break;
        default: SetStatus("The reward could not be claimed."); break;
        }
        Finish(now_);
        if (result == 0) RequestInfo(set);
        return;
    }
    if (kind == "up" && inFlight_.req.kind == Kind::Upgrade && w.size() == 6) {
        int result = 0, set = 0;
        unsigned mask = 0;
        if (!CollectionWire::Number(w[3], result) || !CollectionWire::Number(w[4], set) || !CollectionWire::Number(w[5], mask) ||
            set != inFlight_.req.set || mask >= CollectionWire::MaskLimit) return;
        if (SetView* s = FindSet(set)) {
            if (result <= 1) { s->owned = mask; s->upgradeLoaded = false; Touch(); }
        }
        const unsigned level = (mask >> CollectionWire::LevelShift) & CollectionWire::MaxLevel;
        switch (result) {
        case 0: SetStatus("Upgrade succeeded! Level +" + std::to_string(level) + "."); break;
        case 1: SetStatus("The upgrade failed. Your next chance is better."); break;
        case 2: SetStatus("This set has no upgrades."); break;
        case 3: SetStatus("This set is at its maximum level."); break;
        case 4: SetStatus("Activate this set first."); break;
        case 5: SetStatus("You do not have enough Crones."); break;
        case 7: SetStatus("You do not have the required materials (unmodified items only)."); break;
        default: SetStatus("The upgrade could not be done."); break;
        }
        Finish(now_);
        if (result <= 1) RequestInfo(set);
        return;
    }
    if (kind == "eq" && (inFlight_.req.kind == Kind::Equip || inFlight_.req.kind == Kind::Activate) && w.size() == 7) {
        int result = 0, set = 0;
        unsigned active = 0, mask = 0;
        if (!CollectionWire::Number(w[3], result) || !CollectionWire::Number(w[4], set) || !CollectionWire::Number(w[5], active) ||
            !CollectionWire::Number(w[6], mask) || set != inFlight_.req.set || mask >= CollectionWire::MaskLimit) return;
        const bool activating = inFlight_.req.kind == Kind::Activate;
        const bool wantEquip = inFlight_.req.slot == 1;
        if (result == 0 || result == 3) {
            if (SetView* s = FindSet(set)) {
                s->owned = mask;
                if (activating) s->owned |= CollectionWire::OwnBit;
                Touch();
            }
            if (activating) SetStatus(result == 0 ? "Set activated. Equip it to use its bonus." : "This set is already activated.");
            else SetStatus(result == 0 ? (wantEquip ? "Set equipped. The set bonus is active." : "Set unequipped. The set bonus is removed.")
                                       : (wantEquip ? "This set is already equipped." : "This set is not equipped."));
        } else {
            SetStatus(result == 2 ? (activating ? "Unlock at least one rune of this set first." : "Activate this set first.") :
                      result == 5 ? "You do not have enough Crones." : "That set cannot be changed.");
        }
        Finish(now_);
        return;
    }
    if (kind == "reg" && inFlight_.req.kind == Kind::Register && w.size() == 7) {
        int result = 0, set = 0;
        unsigned slot = 0, mask = 0;
        if (!CollectionWire::Number(w[3], result) || !CollectionWire::Number(w[4], set) || !CollectionWire::Number(w[5], slot) ||
            !CollectionWire::Number(w[6], mask) || set != inFlight_.req.set || slot != inFlight_.req.slot) return;
        if (SetView* s = FindSet(set)) { s->owned = (mask & (CollectionWire::ActiveBit - 1)) | (s->owned & ~(CollectionWire::ActiveBit - 1)); Touch(); }
        if (batch_.open) {
            if (result == 0) ++batch_.registered;
            else if (result == 3) ++batch_.registered;  // already there counts as done
            else ++batch_.other;
        }
        if (!batch_.open) {
            SetStatus(result == 0 ? "Registered." : result == 3 ? "Already registered." : "That item could not be registered.");
        }
        Finish(now_);
    }
}
}
