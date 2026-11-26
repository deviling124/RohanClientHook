// What the Costume Effect window may send, and what the preview does to the player's aura, against fake client functions.
#include "../src/CostumeEffect/CostumeClient.h"
#include "../src/CostumeEffect/CostumePreview.h"
#include "../src/CostumeEffect/CostumeGuard.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
std::uint32_t NonceOf(const std::string& command) {
    CostumeWire::Request request;
    CHECK(CostumeWire::ParseRequest(command, request));
    return request.nonce;
}
void Answer(CostumeClient& client, ULONGLONG now, unsigned tickets, std::vector<CostumeWire::Copy> copies) {
    const auto commands = client.TakeCommands();
    CHECK(commands.size() == 1 && commands[0].rfind(".cos1 list ", 0) == 0);
    if (commands.empty()) return;
    CostumeWire::State state;
    state.nonce = NonceOf(commands[0]);
    state.ticket = 2985985;
    state.tickets = tickets;
    // As the server does: the asked-for copy, else the first usable one, and that costume's effects.
    CostumeWire::Request asked;
    CHECK(CostumeWire::ParseRequest(commands[0], asked));
    const CostumeWire::Copy* selected = nullptr;
    for (const auto& c : copies)
        if (asked.itemId && c.itemId == asked.itemId) selected = &c;
    for (const auto& c : copies)
        if (!selected && c.status == CostumeWire::Usable) selected = &c;
    if (selected) {
        state.selected = selected->itemId;
        const bool second = selected->type == 4456400 || selected->type == 4456399;
        state.base = second ? 4456400 : 4456429;
        if (second) state.recipes = {{4456399, "Winner"}};
        else state.recipes = {{4456428, "Dark Flame"}, {4456427, "Burning Soul"}};
    }
    state.copies = std::move(copies);
    for (const auto& line : CostumeWire::BuildState(state)) client.Receive(line, now);
}

// Drops a bag copy into the slot and answers the list that follows.
void Put(CostumeClient& client, ULONGLONG now, int itemId, unsigned type, unsigned tickets, const std::vector<CostumeWire::Copy>& copies) {
    CHECK(client.Drop({itemId, type, 1, itemId % 30}, now));
    client.Tick(now + CostumeClient::RetryGapMs);
    Answer(client, now + CostumeClient::RetryGapMs + 50, tickets, copies);
}

void WindowChoices() {
    using namespace CostumeWire;
    CostumeClient client;
    client.Open(1000);
    CHECK(client.Visible() && !client.CanCombine());
    client.StepEffect(1);                                  // nothing to choose before the server answered
    CHECK(client.Snapshot().effect == 0);
    Answer(client, 1100, 1, {{213, 4456429, Patterns}, {215, 4456429, Usable}, {216, 4456429, Options}, {217, 4456429, Usable}});
    auto view = client.Snapshot();
    CHECK(view.loaded && view.copy == 0 && view.placed.itemId == 0 && !view.ready);   // the slot starts empty
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 0);                                             // nothing to preview yet
    const std::vector<Copy> bag{{213, 4456429, Patterns}, {215, 4456429, Usable}, {216, 4456429, Options}, {217, 4456429, Usable}};
    Put(client, 2000, 213, 4456429, 1, bag);                                          // refused: shown, not changeable
    view = client.Snapshot();
    CHECK(view.copy == 0 && view.placed.itemId == 213 && view.status == CostumeClient::Message(HasPatterns));
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 0 && !client.CanCombine());
    Put(client, 3000, 999, 4456000, 1, bag);                                          // not a listed costume: not kept
    view = client.Snapshot();
    CHECK(view.copy == 0 && view.placed.itemId == 0 && view.status.find("can't have") != std::string::npos);
    Put(client, 4000, 217, 4456429, 1, bag);
    view = client.Snapshot();
    CHECK(view.copy == 217 && view.placed.itemId == 217 && view.placed.bag == 1 && view.status.empty());
    client.Remove();
    CHECK(client.Snapshot().copy == 0 && client.Snapshot().placed.itemId == 0);
    CHECK(client.TakeCommands().empty());
    Put(client, 5000, 215, 4456429, 1, bag);
    CHECK(!client.Drop({0, 4456429, 1, 0}, 5800) && !client.Drop({5, 0, 1, 0}, 5800));

    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 4456428 && client.Snapshot().position == 1 && client.CanCombine());
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 4456427);
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 0 && !client.CanCombine());      // wraps back to no change
    client.StepEffect(-1);
    CHECK(client.Snapshot().effect == 4456427);
    CHECK(!client.Combine(2000));                          // first press only asks
    CHECK(client.TakeCommands().empty() && client.Snapshot().confirming);
    client.StepEffect(-1);
    CHECK(!client.Snapshot().confirming);                  // a new choice needs a new confirmation
    CHECK(!client.Combine(2000));
    CHECK(client.Combine(2100));
    auto commands = client.TakeCommands();
    CHECK(commands.size() == 1);
    Request request;
    CHECK(ParseRequest(commands[0], request) && request.make && request.itemId == 215 && request.effect == 4456428);
    CHECK(!client.Combine(2150) && !client.CanCombine());  // never sent twice
    client.Receive(BuildResult(request.nonce + 5, Done), 2200);
    CHECK(client.Snapshot().combining);
    client.Receive(BuildResult(request.nonce, Done, 4456428), 2300);
    CHECK(client.Snapshot().status == CostumeClient::Message(Done) && client.Snapshot().effect == 0 &&
          client.Snapshot().placed.itemId == 0);                                      // the slot empties: the preview ends
    client.Tick(2300 + CostumeClient::RetryGapMs);
    Answer(client, 3200, 0, {});
    view = client.Snapshot();
    CHECK(view.status == CostumeClient::Message(Done) && view.copy == 0 && !view.ready);

    client.Refresh(10000);
    Answer(client, 10100, 0, {{215, 4456429, Usable}});
    Put(client, 11000, 215, 4456429, 0, {{215, 4456429, Usable}});
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 4456428 && !client.CanCombine());   // preview works without a ticket, applying does not

    Put(client, 12000, 215, 4456429, 1, {{215, 4456429, Usable}});
    client.StepEffect(1);
    client.Combine(13000);
    CHECK(client.Combine(13100));
    client.TakeCommands();
    client.Tick(13100 + CostumeClient::TimeoutMs + 1);
    view = client.Snapshot();
    CHECK(!view.loaded && !view.ready && view.status.find("Event Inventory") != std::string::npos);
    CHECK(client.TakeCommands().empty());                  // an unanswered combine is not repeated by itself
    client.Close();
    CHECK(!client.Visible());
    client.Reset();
    CHECK(client.TakeCommands().empty() && !client.Snapshot().loaded && client.Snapshot().effect == 0);
}

// A costume that already has an effect can be changed to any other one.
void Rechange() {
    using namespace CostumeWire;
    CostumeClient client;
    client.Open(1000);
    Answer(client, 1100, 1, {{300, 4456428, Usable}});
    const std::vector<Copy> bag{{300, 4456428, Usable}};
    Put(client, 2000, 300, 4456428, 1, bag);
    auto view = client.Snapshot();
    CHECK(view.copy == 300 && view.copyType == 4456428 && view.effect == 0);
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 4456427 && client.CanCombine());   // its own effect is skipped
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 0);
    client.StepEffect(-1);
    CHECK(client.Snapshot().effect == 4456427);
    client.StepEffect(-1);
    CHECK(client.Snapshot().effect == 0);                               // backwards skips it too
    client.StepEffect(1);
    CHECK(!client.Combine(3000));
    CHECK(client.Combine(3100));
    const auto commands = client.TakeCommands();
    Request request;
    CHECK(commands.size() == 1 && ParseRequest(commands[0], request) && request.itemId == 300 && request.effect == 4456427);
    CHECK(!client.Drop({301, 4456429, 1, 1}, 3200));                    // the slot is locked while combining
    client.Receive(BuildResult(request.nonce, SameEffect), 3300);
    CHECK(client.Snapshot().status == CostumeClient::Message(SameEffect));
}

// Another costume brings its own effects, and a late answer for the old slot content is ignored.
void OtherCostume() {
    using namespace CostumeWire;
    CostumeClient client;
    client.Open(1000);
    const std::vector<Copy> bag{{400, 4456429, Usable}, {401, 4456400, Usable}};
    const auto first = client.TakeCommands();                           // the opening list, not answered yet
    CHECK(first.size() == 1);
    CHECK(client.Drop({401, 4456400, 2, 3}, 1100));
    State late;
    late.nonce = NonceOf(first[0]);
    late.ticket = 2985985;
    for (const auto& line : BuildState(late)) client.Receive(line, 1150);
    client.Receive(BuildResult(late.nonce, Busy), 1160);
    auto view = client.Snapshot();
    CHECK(!view.loaded && view.loading && view.placed.itemId == 401 && view.status.empty());
    client.Tick(1200);
    CHECK(client.TakeCommands().empty());                               // not sooner than the server allows
    client.Tick(1000 + CostumeClient::RetryGapMs);
    Answer(client, 1800, 1, bag);
    view = client.Snapshot();
    CHECK(view.loaded && view.copy == 401 && view.copyType == 4456400 && view.state.recipes.size() == 1);
    client.StepEffect(1);
    CHECK(client.Snapshot().effect == 4456399 && client.CanCombine());
    Put(client, 9000, 400, 4456429, 1, bag);
    view = client.Snapshot();
    CHECK(view.copy == 400 && view.effect == 0 && view.state.recipes.size() == 2);
    // A drop long after the last list must not look like an unanswered request.
    CHECK(client.Drop({401, 4456400, 2, 3}, 60000));
    client.Tick(60001);
    CHECK(client.Snapshot().loading && client.Snapshot().status.empty() && client.TakeCommands().size() == 1);
    client.Close();
    CHECK(client.Snapshot().placed.itemId == 0);
}

// ---- Preview ----
struct Character { unsigned applied = 0; };
std::vector<std::string> calls;
Character* local = nullptr;
// Like the client: a key is stored only when it has a row. 65517 is the costume's own aura, 65516/65515 two effects.
void __fastcall FakeAttach(void* c, void*, unsigned key, int) {
    calls.push_back("attach " + std::to_string(key));
    if (key == 65517 || key == 65516 || key == 65515) static_cast<Character*>(c)->applied = key;
}
void __fastcall FakeRemove(void* c, void*, unsigned key, int) {
    calls.push_back("remove " + std::to_string(key));
    if (static_cast<Character*>(c)->applied == key) static_cast<Character*>(c)->applied = 0;
}
void* FakeLocal() { return local; }
// Dressing as the client does it: the aura is taken off first and the worn costume's own one comes with it.
unsigned equippedAura = 0;
bool missing = false;
bool FakeWear(void* c, const CostumePreview::Worn& item) {
    calls.push_back("wear " + std::to_string(item.itemId));
    if (missing) return false;
    static_cast<Character*>(c)->applied = item.type == 4456428 ? 65516 : 0;
    return true;
}
void FakeMark(void*, const CostumePreview::Worn& item, bool inUse) {
    calls.push_back((inUse ? "lock " : "unlock ") + std::to_string(item.itemId));
}
int bagObject = 0;
void* FakeItemObject(void*, const CostumePreview::Worn& item) { return item.itemId == 215 ? &bagObject : nullptr; }
void FakeTakeOff(void* c, const CostumePreview::Worn& item) {
    calls.push_back("takeoff " + std::to_string(item.itemId));
    static_cast<Character*>(c)->applied = equippedAura;
}
unsigned FakeApplied(void* c) { return static_cast<Character*>(c)->applied; }
void Expect(const char* what, const std::vector<std::string>& expected) {
    if (calls != expected) {
        ++failures;
        std::printf("FAIL %s:", what);
        for (const auto& c : calls) std::printf(" [%s]", c.c_str());
        std::printf("\n");
    }
    calls.clear();
}

void Preview() {
    using namespace CostumePreview;
    const Game fake{FakeAttach, FakeRemove, FakeLocal, FakeApplied, FakeWear, FakeTakeOff, FakeMark, FakeItemObject};
    Character me{65517}, other{65517};
    local = &me;
    TestSetup(fake);
    Set(0);
    Expect("nothing chosen, nothing touched", {});
    Set(4456428);
    Expect("preview replaces the own aura", {"remove 65517", "attach 65516"});
    Set(4456428);
    Expect("every frame after that does nothing", {});
    Set(4456427);
    Expect("switching puts the own aura back first", {"remove 65516", "attach 65517", "remove 65517", "attach 65515"});
    Set(0);
    Expect("closing restores the own aura", {"remove 65515", "attach 65517"});
    CHECK(me.applied == 65517 && !Active());

    me.applied = 0;
    Set(4456428);
    Set(0);
    Expect("without an own aura nothing is left behind", {"attach 65516", "remove 65516"});

    me.applied = 65517;
    Set(1234);
    Expect("an effect without a row leaves the own aura", {"remove 65517", "attach 1234", "attach 65517"});
    Set(1234);
    Expect("and is not retried", {});
    Set(0);
    calls.clear();

    Set(4456428);
    calls.clear();
    TestGameRemove(&me, 65517, 0x6B);                      // the player takes the costume off while previewing
    Expect("the preview comes off before the game removes the costume's aura", {"remove 65516", "attach 65517", "remove 65517"});
    CHECK(me.applied == 0 && !Active());
    Set(4456428);
    Expect("the same choice is not put back by itself", {});
    Set(0);
    me.applied = 65517;
    Set(4456428);
    calls.clear();
    TestGameAttach(&other, 65517, 0x6B);
    Expect("other characters do not end the preview", {"attach 65517"});
    TestGameAttach(&me, 7, 0x5D);
    Expect("other effect types do not end it", {"attach 7"});
    CHECK(Active());
    local = &other;                                        // character change
    Set(0);
    Expect("the old character is not touched", {});
    CHECK(!Active());
}

void Dressing() {
    using namespace CostumePreview;
    const Game fake{FakeAttach, FakeRemove, FakeLocal, FakeApplied, FakeWear, FakeTakeOff, FakeMark, FakeItemObject};
    Character me{65517}, other{0};
    local = &me;
    equippedAura = 65517;
    missing = false;
    TestSetup(fake);
    calls.clear();
    Update({}, 0);
    Expect("an empty slot changes nothing", {});
    Update({215, 4456429, 1, 4}, 0);
    Expect("a dropped costume is worn", {"wear 215"});
    Update({215, 4456429, 1, 4}, 0);
    Expect("every frame after that does nothing", {});
    Update({215, 4456429, 1, 4}, 4456427);
    Expect("the chosen effect goes on top", {"attach 65515"});
    Update({300, 4456428, 1, 5}, 0);
    Expect("another costume: the effect comes off, then the equipped look, then the new one", {"remove 65515", "takeoff 215", "wear 300"});
    CHECK(me.applied == 65516);                          // it brings its own aura
    Update({300, 4456428, 1, 5}, 4456427);
    calls.clear();
    Update({}, 0);
    Expect("emptying the slot restores the equipped look", {"remove 65515", "attach 65516", "takeoff 300"});
    CHECK(me.applied == 65517 && !Active());
    missing = true;
    Update({216, 4456429, 1, 6}, 0);
    Update({216, 4456429, 1, 6}, 0);
    Expect("an item that is gone is tried once", {"wear 216"});
    Update({}, 0);
    Expect("and there is nothing to put back", {});
    missing = false;
    Update({215, 4456429, 1, 4}, 0);
    calls.clear();
    local = &other;                                      // character change
    Update({}, 0);
    Expect("the old character is not touched", {});
    local = &me;
    CHECK(ItemObject({215, 4456429, 1, 4}) == &bagObject);   // for the slot's tooltip
    CHECK(!ItemObject({}) && !ItemObject({999, 4456429, 1, 4}));
    Hold({});
    Expect("nothing held, nothing marked", {});
    Hold({215, 4456429, 1, 4});
    Expect("the item in the slot is marked in use", {"lock 215"});
    Hold({215, 4456429, 1, 4});
    Expect("and marked again while it stays", {"lock 215"});
    Hold({300, 4456428, 1, 5});
    Expect("another item: the first is given back", {"unlock 215", "lock 300"});
    Hold({});
    Expect("an emptied slot gives the item back", {"unlock 300"});
    Hold({215, 4456429, 1, 4});
    calls.clear();
    local = &other;
    Hold({});
    Expect("another character's bag is not touched", {});
}
}  // namespace

void Guard() {
    using namespace CostumeGuard;
    CHECK(Decide(false, 4456429, 1, false) == DoubleClick::Use);   // window closed: the game as it is
    CHECK(Decide(true, 4456429, 1, false) == DoubleClick::Fill);
    CHECK(Decide(true, 4440914, 4, false) == DoubleClick::Fill);
    CHECK(Decide(true, 4456429, 1, true) == DoubleClick::Ignore);  // already in a slot: not equipped either
    CHECK(Decide(true, 2985985, 1, false) == DoubleClick::Use);    // other items are used as usual
    CHECK(Decide(true, 4456429, 0, false) == DoubleClick::Use);    // what is worn can still be taken off
}

int main(int argc, char** argv) {
    WindowChoices();
    Rechange();
    OtherCostume();
    Preview();
    Dressing();
    Guard();
    if (argc < 2) { std::printf("FAIL no reference client given\n"); return 1; }
    const HMODULE image = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(image && CostumePreview::MatchesReference(reinterpret_cast<const unsigned char*>(image)));
    CHECK(image && CostumeGuard::MatchesReference(reinterpret_cast<const unsigned char*>(image)));
    if (!failures) std::printf("Costume Effect client tests passed\n");
    return failures != 0;
}
