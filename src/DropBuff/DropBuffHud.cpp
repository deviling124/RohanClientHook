#include "DropBuffHud.h"
#include <cstdio>
#ifndef AHK_WINDOW_MODEL_ONLY
#include "imgui.h"
#endif

bool DropBuffHud::Receive(std::string_view text, Clock::time_point now) {
    DropBuffWire::State parsed;
    if (!DropBuffWire::Parse(text, parsed)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = parsed;
    receivedAt_ = now;
    nextPoll_ = now + (parsed.percent > 0 ? ActivePoll : IdlePoll);
    return true;
}

void DropBuffHud::Reset(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = {};
    receivedAt_ = now;
    nextPoll_ = now;
}

DropBuffWire::State DropBuffHud::Current(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    DropBuffWire::State state = state_;
    if (state.percent <= 0) return {};
    if (!state.paused && now > receivedAt_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - receivedAt_).count();
        state.remainingSeconds = elapsed >= static_cast<long long>(state.remainingSeconds) ? 0u :
            state.remainingSeconds - static_cast<unsigned>(elapsed);
    }
    if (state.remainingSeconds == 0) return {};
    return state;
}

bool DropBuffHud::PollDue(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (now < nextPoll_) return false;
    nextPoll_ = now + (state_.percent > 0 ? ActivePoll : IdlePoll);
    return true;
}

#ifndef AHK_WINDOW_MODEL_ONLY
void DropBuffHud::Draw() {
    const auto state = Current();
    if (state.percent <= 0) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 600.f || display.y < 300.f) return;
    // Left of the native buff icons, which sit in the top right corner.
    ImGui::SetNextWindowPos(ImVec2(display.x - 255.f, 8.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(.6f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##DropBuffHud", nullptr, flags)) {
        char text[64];
        std::snprintf(text, sizeof text, "Drop +%d%%  %u:%02u", state.percent,
            state.remainingSeconds / 60, state.remainingSeconds % 60);
        ImGui::TextColored(state.paused ? ImVec4(.63f, .70f, .78f, 1.f) : ImVec4(.96f, .78f, .43f, 1.f), "%s", text);
        if (state.paused) ImGui::TextDisabled("paused in safe zone");
    }
    ImGui::End();
}
#else
void DropBuffHud::Draw() {}
#endif
