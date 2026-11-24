#pragma once
#include "../../shared/BossCheckProtocol.h"
#include <deque>
#include <string>
#include <vector>

// Client side of the Boss Check protocol (shared/BossCheckProtocol.h): one list request at a time, the move request, paging and the
// timeout. No game or Windows dependencies, so it is unit tested on its own (tests/BossCheckClientTests.cpp). The window reads it.
namespace BossCheckClient {

constexpr int RowsPerPage = 6;
constexpr unsigned long long TimeoutMs = 6000, MinGapMs = 1000;
// Reasons that never come from the server: 100 = no answer in time, 101 = asked again too soon.
constexpr int ErrorTimeout = 100, ErrorTooSoon = 101;

class Client {
public:
    // Asks the server for the list. False (see rejected()) when the last request was less than a second ago.
    bool Refresh(unsigned long long nowMs) {
        if (Busy(nowMs)) { rejected_ = ErrorTooSoon; return false; }
        rejected_ = 0;
        Begin(nowMs);
        list_ = BossWire::List{};
        list_.nonce = nonce_;
        waitingList_ = true;
        commands_.push_back(BossWire::BuildList(nonce_));
        return true;
    }
    // Teleports to the boss in `row` (a position in the shown list). The list is kept; the answer arrives as moved().
    bool Move(int row, unsigned long long nowMs) {
        if (row < 0 || row >= static_cast<int>(list_.rows.size()) || Busy(nowMs)) { rejected_ = ErrorTooSoon; return false; }
        rejected_ = 0;
        Begin(nowMs);
        list_.nonce = nonce_;
        list_.moved = -1;
        list_.movedIndex = -1;
        waitingMove_ = true;
        commands_.push_back(BossWire::BuildMove(nonce_, list_.rows[static_cast<std::size_t>(row)].index));
        return true;
    }
    void Receive(std::string_view line) {
        if (!waitingList_ && !waitingMove_) return;
        if (!BossWire::Apply(list_, line)) return;
        if (waitingList_ && (list_.done || list_.error)) { waitingList_ = false; update_ = true; if (page_ >= Pages()) page_ = Pages() - 1; }
        if (waitingMove_ && (list_.moved >= 0 || list_.error)) { waitingMove_ = false; moveUpdate_ = true; }
    }
    void Tick(unsigned long long nowMs) {
        if ((waitingList_ || waitingMove_) && nowMs >= startedAt_ && nowMs - startedAt_ > TimeoutMs) {
            list_.error = ErrorTimeout;
            if (waitingList_) update_ = true; else moveUpdate_ = true;
            waitingList_ = waitingMove_ = false;
        }
    }
    std::vector<std::string> TakeCommands() {
        std::vector<std::string> out(commands_.begin(), commands_.end());
        commands_.clear();
        return out;
    }
    // True once after a list answer (or a failure) arrived / after a move answer (or a failure) arrived.
    bool TakeUpdate() { const bool v = update_; update_ = false; return v; }
    bool TakeMoveUpdate() { const bool v = moveUpdate_; moveUpdate_ = false; return v; }
    void Reset() { commands_.clear(); waitingList_ = waitingMove_ = update_ = moveUpdate_ = started_ = false; list_ = BossWire::List{}; page_ = 0; }

    // Paging: 6 rows per page. Pages() is at least 1.
    int Pages() const { const int n = static_cast<int>(list_.rows.size()); return n <= 0 ? 1 : (n + RowsPerPage - 1) / RowsPerPage; }
    int page() const { return page_; }
    void First() { page_ = 0; }
    void Last() { page_ = Pages() - 1; }
    void Prev() { if (page_ > 0) --page_; }
    void Next() { if (page_ + 1 < Pages()) ++page_; }
    // The list row shown in screen row `slot` (0..5) of the current page, or null.
    const BossWire::Row* RowAt(int slot) const {
        const int i = page_ * RowsPerPage + slot;
        return slot >= 0 && slot < RowsPerPage && i < static_cast<int>(list_.rows.size()) ? &list_.rows[static_cast<std::size_t>(i)] : nullptr;
    }
    int RowIndexAt(int slot) const { return page_ * RowsPerPage + slot; }
    bool waiting() const { return waitingList_ || waitingMove_; }
    int error() const { return list_.error; }
    int moved() const { return list_.moved; }
    int rejected() const { return rejected_; }
    const BossWire::List& list() const { return list_; }

private:
    bool Busy(unsigned long long nowMs) const {
        return waiting() || (started_ && nowMs >= startedAt_ && nowMs - startedAt_ < MinGapMs);
    }
    void Begin(unsigned long long nowMs) {
        nonce_ = nonce_ >= 0x7fffffffu ? 1u : nonce_ + 1;
        started_ = true;
        startedAt_ = nowMs;
        list_.error = 0;
    }
    BossWire::List list_;
    std::deque<std::string> commands_;
    std::uint32_t nonce_ = 0;
    unsigned long long startedAt_ = 0;
    bool started_ = false, waitingList_ = false, waitingMove_ = false, update_ = false, moveUpdate_ = false;
    int rejected_ = 0, page_ = 0;
};

inline Client& Shared() {
    static Client client;
    return client;
}

inline const char* MoveText(int code) {
    switch (code) {
    case 0: return "You were moved next to the boss.";
    case 1: return "That boss is not alive.";
    case 2: return "Boss Check is turned off on this server.";
    case 3: case ErrorTooSoon: return "Wait a few seconds before moving again.";
    case 4: return "There is no such boss.";
    case 5: return "You cannot move from here to that boss.";
    case ErrorTimeout: return "The server did not answer. Is the server updated?";
    default: return "You cannot move right now.";
    }
}
inline const char* ListErrorText(int code) {
    switch (code) {
    case 1: return "Boss Check is turned off on this server.";
    case 3: case ErrorTooSoon: return "Please wait a moment before refreshing.";
    case ErrorTimeout: return "The server did not answer. Is the server updated?";
    default: return "Boss Check failed.";
    }
}
}  // namespace BossCheckClient
