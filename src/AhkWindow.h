#pragma once
#include "../shared/AhkProtocol.h"
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// Owned by the render thread. The hook queues network text onto that thread.
class AhkWindow {
public:
    void Open();
    void Close();
    // Account/connection changes invalidate even an in-flight save.
    void ResetSession();
    bool Visible() const { return visible_; }
    bool Busy() const { return phase_==Phase::Loading || phase_==Phase::Saving; }
    void Draw();
    void Receive(std::string_view text);
    std::vector<std::string> TakeCommands();

    // Also used by the standalone preview and model regression tests.
    bool Apply();
    void RestoreDefaults();
    void CancelChanges();
    bool Ready() const { return phase_==Phase::Ready && synchronized_; }
    bool Eligible() const { return eligible_; }
    bool Dirty() const;
    const Ahk::Settings& Draft() const { return draft_; }
    Ahk::Settings* EditDraft() { return Ready() && eligible_ ? &draft_ : nullptr; }
    const std::string& Status() const { return status_; }
    void Tick();

private:
    enum class Phase { Idle,Loading,Ready,Saving,Failed };
    using Clock=std::chrono::steady_clock;
    void StartLoad();
    void Fail(const std::string& message);
    static std::uint32_t NextNonce();
    bool visible_=false,eligible_=false,synchronized_=false,stateReceived_=false;
    Phase phase_=Phase::Idle;
    std::uint32_t nonce_=0;
    std::uint16_t rowsReceived_=0;
    int category_=0;
    Ahk::Settings accepted_=Ahk::Defaults(),draft_=accepted_,incoming_{},pending_{};
    std::string status_="Type .ahk in Say chat to open settings.";
    Clock::time_point deadline_{};
    std::vector<std::string> commands_;
};
