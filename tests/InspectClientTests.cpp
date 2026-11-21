#include "../src/Inspect/InspectClient.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static InspectWire::Profile Sample(std::uint32_t nonce) {
    InspectWire::Profile p;
    p.nonce = nonce; p.level = 99; p.name = "Anden";
    for (int i = 0; i < 6; ++i) p.attr[i] = 100 + i;
    for (int i = 0; i < 7; ++i) p.stat[i] = 200 + i;
    for (int i = 0; i < 8; ++i) p.vit[i] = 300 + i;
    for (int i = 0; i < 4; ++i) p.res[i] = 10 + i;
    for (unsigned i = 0; i < InspectWire::SlotCount; ++i) p.eq[i] = 5000 + i;
    return p;
}

// The nonce is read back from the queued command, the way the server sees it.
static std::uint32_t NonceOf(const std::string& command) {
    InspectWire::Request r;
    return InspectWire::ParseRequest(command, r) ? r.nonce : 0;
}

int main() {
    using namespace InspectClient;
    Client c;
    CHECK(c.state() == State::Idle);
    CHECK(!c.Request("bad name", 1000) && c.state() == State::Idle && c.rejected() == ErrorBadName && !c.TakeUpdate());
    CHECK(c.TakeCommands().empty());
    CHECK(!c.Request("", 1000) && c.rejected() == ErrorBadName);
    CHECK(!c.Request("ABCDEFGHIJKLMNOPQ", 1000));  // 17 characters
    // a full exchange
    CHECK(c.Request("Anden", 2000) && c.state() == State::Waiting);
    auto commands = c.TakeCommands();
    CHECK(commands.size() == 1 && c.TakeCommands().empty());
    const auto nonce = NonceOf(commands[0]);
    CHECK(nonce != 0);
    CHECK(!c.Request("Other", 2100) && c.rejected() == ErrorTooSoon && c.state() == State::Waiting);   // still waiting, untouched
    c.Reset();
    CHECK(c.Request("Anden", 5000));
    commands = c.TakeCommands();
    const auto second = NonceOf(commands[0]);
    CHECK(second != 0 && second != nonce);
    auto lines = InspectWire::BuildReplies(Sample(second));
    c.Receive("INS1 attr 999 1 2 3 4 5 6");        // another nonce: ignored
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) { c.Receive(lines[i]); CHECK(c.state() == State::Waiting); }
    c.Receive(lines.back());
    CHECK(c.state() == State::Ready && c.TakeUpdate() && !c.TakeUpdate());
    CHECK(c.profile().level == 99 && c.profile().name == "Anden" && c.profile().eq[34] == 5034 && c.profile().attr[5] == 105);
    // too soon after a finished request
    CHECK(!c.Request("Anden", 5500) && c.rejected() == ErrorTooSoon && c.state() == State::Ready);
    // an error answer
    CHECK(c.Request("Anden", 7000));
    commands = c.TakeCommands();
    c.Receive(InspectWire::BuildError(NonceOf(commands[0]), 2));
    CHECK(c.state() == State::Failed && c.error() == 2 && c.TakeUpdate());
    // an answer that ends before every part arrived is a failure, not a half filled window
    CHECK(c.Request("Anden", 9000));
    commands = c.TakeCommands();
    const auto partialNonce = NonceOf(commands[0]);
    auto partial = InspectWire::BuildReplies(Sample(partialNonce));
    c.Receive(partial[0]);
    c.Receive(partial.back());
    CHECK(c.state() == State::Failed && c.error() == 2);
    // timeout
    CHECK(c.Request("Anden", 20000));
    c.Tick(20000 + TimeoutMs);
    CHECK(c.state() == State::Waiting);
    c.Tick(20000 + TimeoutMs + 1);
    CHECK(c.state() == State::Failed && c.error() == ErrorTimeout);
    c.Receive("INS1 done 1");                         // late answers after the failure change nothing
    CHECK(c.error() == ErrorTimeout);
    // names
    NameTable names;
    names.Parse("# comment\r\ntype 0x30104 Human|Guardian\r\ndbtype 17 Elf|Hunter\ngrade 0 Guild Master\ntitle 12 The Brave\nbogus 1 x\ntype zz A|B\ntype 5\n");
    CHECK(names.Race(0x30104, 99) == "Human" && names.Class(0x30104, 99) == "Guardian");
    CHECK(names.Race(1, 17) == "Elf" && names.Class(1, 17) == "Hunter");   // falls back to the database type
    CHECK(names.Race(1, 18).empty() && names.Class(1, 18).empty());
    CHECK(names.Rank(0) == "Guild Master" && names.Rank(-1).empty() && names.Title(12) == "The Brave" && names.Title(13).empty());
    names.Parse("");
    CHECK(names.Race(0x30104, 17).empty() && names.Rank(0).empty());
    CHECK(std::string(ErrorText(2)).find("not found") != std::string::npos);
    CHECK(std::string(ErrorText(12345)) == "Inspect failed.");
    std::printf(failures ? "%d FAILED\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
