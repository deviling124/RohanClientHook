#include "../src/EventManager/EventInviteStore.h"
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
}
int main() {
    using namespace EventManagerWire;
    EventManager::InviteStore store;
    Check(!store.Current(1000).id && !store.Join(1, 1000), "empty store shows nothing and joins nothing");
    store.Apply({5, true, 60, State::Asking, "Disguise Event"}, 1000);
    auto view = store.Current(1000);
    Check(view.id == 5 && view.name == "Disguise Event" && view.seconds == 60, "an invite is shown");
    Check(store.Current(4000).seconds == 57, "the countdown runs between packets");
    Check(store.Join(5, 4000) && !store.Current(4000).id, "Join queues and closes the popup");
    Check(!store.Join(5, 4500), "a double click does not queue twice");
    const auto commands = store.TakeCommands();
    Check(commands.size() == 1 && commands[0] == ".evm1 join 5" && store.TakeCommands().empty(), "the command text");
    store.Apply({5, true, 56, State::Asking, "Disguise Event"}, 4300);
    Check(!store.Current(4300).id, "a repeat right after the click keeps it closed");
    store.Apply({5, true, 55, State::Joined, "Disguise Event"}, 4600);
    Check(!store.Current(4600).id && !store.Join(5, 9000), "joined stays closed and cannot join again");
    store.Apply({5, true, 53, State::Joined, "Disguise Event"}, 6600);
    Check(!store.Current(6600).id, "a repeat does not reopen it");
    store.Apply({9, true, 40, State::Asking, "Refused"}, 6700);
    Check(store.Join(9, 6700), "join the second event");
    store.Apply({9, true, 40, State::WrongPlace, "Refused"}, 6900);
    view = store.Current(6900);
    Check(view.id == 9 && view.state == State::WrongPlace, "a refused join brings the popup back with the reason");
    store.Apply({9, false}, 6950);
    store.Apply({9, true, 40, State::Asking, "Lost"}, 7000);
    Check(store.Join(9, 7000), "click");
    store.Apply({9, true, 38, State::Asking, "Lost"}, 9000);
    Check(store.Current(9000).id == 9, "a click the server never got brings the popup back");
    store.Apply({9, false}, 9100);
    store.Apply({6, true, 20, State::Asking, "Other Event"}, 5000);
    Check(store.Current(5000).id == 6, "the event that starts first is shown");
    store.Apply({6, false}, 5100);
    Check(!store.Current(5100).id, "a close removes the invite");
    store.Apply({4, true, 60, State::Asking, "Quiet"}, 5200);
    Check(store.Current(5200).id == 4 && !store.Current(5200 + ExpiryMs).id, "an invite the server stops repeating expires");
    store.Apply({7, true, 3, State::Asking, "Short"}, 20000);
    Check(store.Current(22999).id == 7 && !store.Current(23000).id, "the popup ends at the start time");
    store.Apply({8, false}, 30000);
    Check(!store.Current(30000).id, "a close for an unknown invite is ignored");
    for (std::uint32_t id = 10; id < 20; ++id) store.Apply({id, true, 60, State::Asking, "Many"}, 40000);
    for (std::uint32_t id = 10; id < 10 + MaxInvites; ++id) store.Apply({id, false}, 40000);
    Check(!store.Current(40000).id, "the number of invites is bounded");
    store.Apply({30, true, 60, State::WrongPlace, "Disguise Event"}, 50000);
    Check(store.Join(30, 50000), "a refused player can click again");
    store.Apply({31, true, 60, State::Dead, "Disguise Event"}, 50000);
    unsigned char body[HeaderSize + MaxText]{};
    std::size_t written = 0;
    Message decoded;
    Check(Encode({31, true, 60, State::Dead, "Disguise Event"}, body, sizeof body, written) &&
          Decode(body, written, decoded) && decoded.state == State::Dead, "the client reads the dead refusal");
    store.Reset();
    Check(!store.Current(50000).id && store.TakeCommands().empty(), "reset forgets everything");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
