#include "../src/CollectionBook/CollectionBookClient.h"
#include <cstdio>
#include <cstdlib>

using namespace CollectionBookClient;
using namespace CollectionWire;
namespace {
int checks = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", label); std::exit(1); }
}
// The nonce of the single command a client just sent (".cbk1 <verb> <nonce> ...").
std::uint32_t NonceOf(const std::string& command) {
    const auto w = Words(command);
    std::uint32_t n = 0;
    Check(w.size() >= 3 && Number(w[2], n), "command has a nonce");
    return n;
}
Set MakeSet(int id, const char* name, unsigned owned, unsigned base) {
    Set s;
    s.id = id; s.category = 1; s.owned = owned; s.name = name;
    for (unsigned i = 0; i < SlotsPerSet; ++i) s.items[i] = base + i;
    return s;
}
// Feeds the server's list reply for `count` sets (first page) into the client.
void ServeList(Client& c, std::uint32_t nonce, unsigned category, unsigned page, unsigned pages, unsigned count,
               const std::vector<Set>& sets) {
    c.Receive(Wait(nonce));
    c.Receive(State(nonce, category, page, pages, count));
    for (unsigned i = 0; i < sets.size(); ++i) {
        c.Receive(SetRow(nonce, i, sets[i]));
        c.Receive(ItemsRow(nonce, i, sets[i]));
    }
}
}

int main() {
    // --- list --------------------------------------------------------------------------------------------------
    Client c;
    c.RequestList(1, 0);
    Check(c.Busy() && c.Status() == "Loading...", "a queued list is busy and says loading");
    c.Tick(1000);
    auto cmds = c.TakeCommands();
    Check(cmds.size() == 1 && cmds[0].rfind(".cbk1 list ", 0) == 0 && cmds[0].find(" 1 0") != std::string::npos, "list command sent");
    const auto n1 = NonceOf(cmds[0]);
    c.Tick(1200);
    Check(c.TakeCommands().empty(), "no poll before 500 ms");
    c.Tick(1600);
    cmds = c.TakeCommands();
    Check(cmds.size() == 1 && cmds[0] == ".cbk1 poll " + std::to_string(n1), "poll after 500 ms");
    std::vector<Set> sets = {MakeSet(11, "Wing Set", 5, 100), MakeSet(12, "Toxic Set", 0, 200)};
    ServeList(c, n1, 1, 0, 1, 2, sets);
    Check(!c.Busy() && c.CurrentPage().loaded && c.CurrentPage().sets.size() == 2, "page loaded with two sets");
    Check(c.CurrentPage().pages == 1 && c.CurrentPage().count == 2 && c.CurrentPage().category == 1, "page metadata");
    Check(c.CurrentPage().sets[0].name == "Wing Set" && c.CurrentPage().sets[0].owned == 5 && c.CurrentPage().sets[0].items[5] == 105,
          "set contents");
    Check(c.Status().empty(), "status cleared after a list");

    // --- stale and malformed replies are ignored ---------------------------------------------------------------------
    const unsigned version = c.Version();
    c.Receive(State(n1 + 1, 1, 0, 1, 0));
    c.Receive("garbage");
    c.Receive("CBK1 set");
    Check(c.Version() == version, "unrelated lines change nothing");

    // --- an empty category ------------------------------------------------------------------------------------------
    c.RequestList(3, 0);
    c.Tick(1700);
    Check(c.TakeCommands().empty(), "requests are 600 ms apart");
    c.Tick(2300);
    cmds = c.TakeCommands();
    const auto n2 = NonceOf(cmds.at(0));
    c.Receive(State(n2, 3, 0, 1, 0));
    Check(!c.Busy() && c.CurrentPage().sets.empty() && c.CurrentPage().category == 3, "an empty category is an empty page");

    // --- effect text ---------------------------------------------------------------------------------------------------
    c.RequestList(1, 0);
    c.Tick(3000);
    cmds = c.TakeCommands();
    ServeList(c, NonceOf(cmds.at(0)), 1, 0, 1, 2, sets);
    c.RequestInfo(11);
    c.RequestInfo(11);
    c.Tick(3700);
    cmds = c.TakeCommands();
    Check(cmds.size() == 1 && cmds[0].rfind(".cbk1 info ", 0) == 0 && cmds[0].find(" 11") != std::string::npos, "info sent once");
    const auto n3 = NonceOf(cmds[0]);
    c.Receive(FxRow(n3, 0, 0, 2, "2 Set Effect"));
    Check(c.Busy(), "info is not complete before the last line");
    c.Receive(FxRow(n3, 0, 1, 2, "ATK +1%"));
    c.Receive(FxRow(n3, 1, 0, 1, "No upgrades for this set."));
    c.Receive(FxRow(n3, 2, 0, 1, "No rewards for this set."));
    Check(!c.Busy() && c.FindSet(11)->effectLoaded && c.FindSet(11)->effect.size() == 2 && c.FindSet(11)->effect[1] == "ATK +1%",
          "effect lines stored");

    // --- register all: skips owned slots, tolerates missing items ---------------------------------------------------------
    c.RequestRegisterAll(11);           // owned = 5 (slots 0 and 2): slots 1, 3, 4, 5 remain
    Check(c.Status() == "Registering...", "registration run announced");
    std::vector<unsigned> slotsAsked;
    std::uint64_t t = 4400;
    for (int step = 0; step < 4; ++step) {
        c.Tick(t);
        cmds = c.TakeCommands();
        Check(cmds.size() == 1 && cmds[0].rfind(".cbk1 reg ", 0) == 0, "reg command sent");
        const auto w = Words(cmds[0]);
        unsigned slot = 0;
        Check(Number(w[4], slot) && w[3] == "11", "reg names set 11 and a slot");
        slotsAsked.push_back(slot);
        const auto n = NonceOf(cmds[0]);
        if (step == 1) c.Receive(Error(n, 8));                    // item not in the inventory
        else c.Receive(RegResult(n, 0, 11, slot, c.FindSet(11)->owned | (1u << slot)));
        t += 700;
    }
    Check(slotsAsked == std::vector<unsigned>({1, 3, 4, 5}), "slots asked in order, owned ones skipped");
    Check(!c.Busy() && c.Status() == "Registered 3 of 4 items (1 not in your inventory).", "summary after the run");
    Check(c.FindSet(11)->owned == (5u | (1u << 1) | (1u << 4) | (1u << 5)), "owned mask follows the replies");
    c.RequestRegisterAll(11);
    Check(c.Status() == "Registering..." || c.Busy(), "a second run is allowed once the first finished");
    // let the leftover slot 3 attempt time out cleanly
    c.Tick(t);
    cmds = c.TakeCommands();
    Check(cmds.size() == 1 && Words(cmds[0])[4] == "3", "only the still-missing slot is asked again");

    // --- fatal errors abort the run ---------------------------------------------------------------------------------------
    Client d;
    d.RequestList(1, 0);
    d.Tick(10);
    ServeList(d, NonceOf(d.TakeCommands().at(0)), 1, 0, 1, 1, {MakeSet(21, "Solo", 0, 300)});
    d.RequestRegisterAll(21);
    d.Tick(1000);
    cmds = d.TakeCommands();
    d.Receive(Error(NonceOf(cmds.at(0)), 7));
    Check(!d.Busy() && d.Status() == "Please close and reopen the book.", "error 7 aborts the whole run");
    d.RequestRegisterAll(21);
    d.Tick(2000);
    cmds = d.TakeCommands();
    d.Receive(Error(NonceOf(cmds.at(0)), 9));
    Check(d.Status() == "Close your kiosk first.", "error 9 text");

    // --- timeout -----------------------------------------------------------------------------------------------------------
    Client e;
    e.RequestList(1, 0);
    e.Tick(100);
    e.TakeCommands();
    e.Tick(100 + 10001);
    Check(!e.Busy() && e.Status() == "No answer from the server.", "silence times out after 10 s");

    // --- reset -----------------------------------------------------------------------------------------------------------
    c.Reset();
    Check(!c.Busy() && !c.CurrentPage().loaded && c.TakeCommands().empty() && c.Status().empty(), "reset clears everything");
    c.RequestRegisterAll(11);
    Check(!c.Busy(), "registering an unknown set does nothing");

    // --- decompose list and disassemble -----------------------------------------------------------------------------------------------
    {
        Client d;
        d.RequestDList(0);
        d.Tick(5000);
        auto dc = d.TakeCommands();
        Check(dc.size() == 1 && dc[0].rfind(".cbk1 dlist ", 0) == 0 && dc[0].size() > 2 && dc[0].substr(dc[0].size() - 2) == " 0", "dlist sent");
        const auto dn = NonceOf(dc[0]);
        d.Receive(DState(dn, 0, 2, 7));
        d.Receive(DItem(dn, 0, 4001, 12, "Old Card"));
        Check(d.Busy(), "the list waits for all its rows");
        for (unsigned i = 1; i < 5; ++i) d.Receive(DItem(dn, i, 4001 + i, i, "Item " + std::to_string(i)));
        Check(!d.Busy() && d.Decomp().loaded && d.Decomp().pages == 2 && d.Decomp().count == 7 && d.Decomp().items.size() == 5 &&
              d.Decomp().items[0].name == "Old Card" && d.Decomp().items[0].count == 12, "the first page of the decompose list");
        d.RequestDisassemble(4001, 2);
        Check(d.Status() == "Disassembling...", "disassemble says so");
        d.Tick(7000);
        dc = d.TakeCommands();
        Check(dc.size() == 1 && dc[0].rfind(".cbk1 dis ", 0) == 0 && dc[0].find(" 4001 2") != std::string::npos, "dis sent with item and mode");
        const auto dn2 = NonceOf(dc[0]);
        d.Receive(DisResult(dn2, 0, 4001, 2, 2));
        d.Receive(DGot(dn2, 0, 30, 100, "Dust"));
        Check(d.Busy(), "the answer waits for all rewards");
        d.Receive(DGot(dn2, 1, 1, 101, "Gem"));
        std::vector<Reward> got;
        Check(d.TakeRewards(got) && got.size() == 2 && got[0].item == 100 && got[0].amount == 30 && got[1].name == "Gem" && !d.TakeRewards(got), "the rewards are handed out once for the popup");
        Check(d.Status().empty(), "the popup shows the rewards, the status stays empty");
        d.Tick(9000);
        dc = d.TakeCommands();
        Check(dc.size() == 1 && dc[0].rfind(".cbk1 dlist ", 0) == 0, "the list is refreshed after a disassembly");
        d.Receive(DState(NonceOf(dc[0]), 0, 1, 0));
        d.RequestDisassemble(4001, 1);
        d.Tick(11000);
        dc = d.TakeCommands();
        d.Receive(DisResult(NonceOf(dc[0]), 7, 4001, 1, 0));
        Check(d.Status() == "You do not have that item.", "not enough items is explained");
    }

    std::printf("Collection Book client tests passed (%d checks).\n", checks);
    return 0;
}
