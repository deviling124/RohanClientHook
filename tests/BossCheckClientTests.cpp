#include "../src/BossCheck/BossCheckClient.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static std::uint32_t NonceOf(const std::string& command) {
    BossWire::Request r;
    return BossWire::ParseRequest(command, r) ? r.nonce : 0;
}

int main() {
    using namespace BossCheckClient;
    Client c;
    CHECK(c.Refresh(1000));
    auto commands = c.TakeCommands();
    CHECK(commands.size() == 1);
    const auto nonce = NonceOf(commands[0]);
    CHECK(nonce != 0 && c.waiting());
    CHECK(!c.Refresh(1500) && c.rejected() == ErrorTooSoon);       // one request at a time

    for (int i = 0; i < 8; ++i) c.Receive(BossWire::BuildRow(nonce, i, i % 2 == 0, "Boss " + std::to_string(i)));
    c.Receive(BossWire::BuildRow(nonce + 1, 9, true, "Wrong nonce"));
    CHECK(!c.TakeUpdate());
    c.Receive(BossWire::BuildDone(nonce, 8));
    CHECK(c.TakeUpdate() && !c.waiting());
    CHECK(c.Pages() == 2 && c.page() == 0);
    CHECK(c.RowAt(0) && c.RowAt(0)->name == "Boss 0" && c.RowAt(0)->alive);
    CHECK(c.RowAt(5) && c.RowAt(5)->name == "Boss 5");
    c.Next();
    CHECK(c.page() == 1 && c.RowAt(0)->name == "Boss 6" && c.RowAt(1)->name == "Boss 7" && !c.RowAt(2));
    c.Next();
    CHECK(c.page() == 1);
    c.First();
    CHECK(c.page() == 0);
    c.Prev();
    CHECK(c.page() == 0);
    c.Last();
    CHECK(c.page() == 1 && c.RowIndexAt(1) == 7);

    // move: the second screen row of page 1 is boss 7
    CHECK(!c.Move(0, 1500));                                       // within a second of the list request
    CHECK(c.Move(7, 3000));
    commands = c.TakeCommands();
    CHECK(commands.size() == 1);
    BossWire::Request r;
    CHECK(BossWire::ParseRequest(commands[0], r) && r.move && r.index == 7);
    c.Receive(BossWire::BuildMoved(r.nonce, 7, 1));
    CHECK(c.TakeMoveUpdate() && c.moved() == 1 && !c.waiting());
    CHECK(c.Pages() == 2);                                         // the list survives a move
    CHECK(!c.Move(99, 9000));

    // timeout
    CHECK(c.Refresh(20000));
    c.TakeCommands();
    c.Tick(20000 + TimeoutMs - 1);
    CHECK(c.waiting());
    c.Tick(20000 + TimeoutMs + 1);
    CHECK(!c.waiting() && c.error() == ErrorTimeout && c.TakeUpdate());

    // server error
    CHECK(c.Refresh(40000));
    commands = c.TakeCommands();
    c.Receive(BossWire::BuildError(NonceOf(commands[0]), 1));
    CHECK(c.TakeUpdate() && c.error() == 1);
    c.Reset();
    CHECK(c.list().rows.empty() && !c.waiting());

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("BossCheckClientTests passed\n");
    return 0;
}
