#pragma once
#include "../../shared/EventManagerProtocol.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>

// What the server last said about the events that are about to start. No drawing here, so EventNotice.cpp,
// Bridge.cpp and the tests can use it by itself.
namespace EventManager {
class InviteStore {
public:
    static constexpr ULONGLONG JoinRetryMs = 1000;

    struct View {
        std::uint32_t id = 0;                   // 0 = no popup
        std::string name;
        unsigned seconds = 0;
        EventManagerWire::State state = EventManagerWire::State::Asking;
    };

    void Apply(const EventManagerWire::Message& message, ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        for (auto entry = invites.begin(); entry != invites.end(); ++entry) {
            if (entry->id != message.id) continue;
            if (!message.open) { invites.erase(entry); return; }
            // A repeat with an unchanged state does not answer a click that is still on its way.
            if (message.state != entry->state || now - entry->clickedAt >= JoinRetryMs) entry->sending = false;
            Update(*entry, message, now);
            return;
        }
        if (!message.open || invites.size() >= EventManagerWire::MaxInvites) return;
        Invite invite;
        invite.id = message.id;
        Update(invite, message, now);
        invites.push_back(invite);
    }

    // The invite to show: the event that starts first. A click on Join hides it; it comes back only if the
    // server says the player could not be moved, or the click went unanswered. An invite ends at its start
    // time, or when the server stops repeating it.
    View Current(ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        View view;
        ULONGLONG first = 0;
        for (auto entry = invites.begin(); entry != invites.end();) {
            if (now >= entry->startsAt || now - entry->receivedAt >= EventManagerWire::ExpiryMs) {
                entry = invites.erase(entry);
                continue;
            }
            const bool answered = entry->state == EventManagerWire::State::Joined ||
                (entry->sending && now - entry->clickedAt < EventManagerWire::ExpiryMs);
            if (!answered && (!view.id || entry->startsAt < first)) {
                first = entry->startsAt;
                view = {entry->id, entry->name, static_cast<unsigned>((entry->startsAt - now + 999) / 1000),
                        entry->state};
            }
            ++entry;
        }
        return view;
    }

    // A click on Join. True when the command was queued.
    bool Join(std::uint32_t id, ULONGLONG now) {
        std::lock_guard<std::mutex> guard(lock);
        for (auto& invite : invites) {
            if (invite.id != id) continue;
            if (invite.state == EventManagerWire::State::Joined ||
                (invite.clickedAt && now - invite.clickedAt < JoinRetryMs)) return false;
            invite.clickedAt = now;
            invite.sending = true;
            commands.emplace_back(std::string(EventManagerWire::JoinPrefix) + std::to_string(id));
            return true;
        }
        return false;
    }

    std::vector<std::string> TakeCommands() {
        std::lock_guard<std::mutex> guard(lock);
        std::vector<std::string> taken;
        taken.swap(commands);
        return taken;
    }

    void Reset() {
        std::lock_guard<std::mutex> guard(lock);
        invites.clear();
        commands.clear();
    }

private:
    struct Invite {
        std::uint32_t id = 0;
        std::string name;
        EventManagerWire::State state = EventManagerWire::State::Asking;
        ULONGLONG startsAt = 0, receivedAt = 0, clickedAt = 0;
        bool sending = false;
    };
    static void Update(Invite& invite, const EventManagerWire::Message& message, ULONGLONG now) {
        invite.name = message.name;
        invite.state = message.state;
        invite.receivedAt = now;
        invite.startsAt = now + static_cast<ULONGLONG>(message.seconds) * 1000;
    }
    std::mutex lock;
    std::vector<Invite> invites;
    std::vector<std::string> commands;
};

inline InviteStore& Invites() {
    static InviteStore store;
    return store;
}
}
