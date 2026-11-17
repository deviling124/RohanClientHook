#pragma once
#include "../../shared/DropBuffProtocol.h"
#include <chrono>
#include <mutex>
#include <string_view>

// On-screen countdown for the server's timed drop-rate buff. The server decides everything; this class
// only remembers the last reply, counts down between replies (not while paused) and draws a small
// non-interactive label. Receive may be called from the game thread, the rest from the render thread.
class DropBuffHud {
public:
    using Clock = std::chrono::steady_clock;
    // Applies a "DBF1 ..." reply. Returns false for anything malformed.
    bool Receive(std::string_view text, Clock::time_point now = Clock::now());
    // New session or disconnect: forget the buff and poll again right away.
    void Reset(Clock::time_point now = Clock::now());
    // The buff as it stands now. percent is 0 when there is none or it has run out.
    DropBuffWire::State Current(Clock::time_point now = Clock::now()) const;
    bool Active(Clock::time_point now = Clock::now()) const { return Current(now).percent > 0; }
    // True when the ".dbf1" poll should be sent. Moves the next poll forward.
    bool PollDue(Clock::time_point now = Clock::now());
    void Draw();

    static constexpr std::chrono::seconds ActivePoll{5}, IdlePoll{20};

private:
    mutable std::mutex mutex_;
    DropBuffWire::State state_{};
    Clock::time_point receivedAt_{}, nextPoll_{};
};
