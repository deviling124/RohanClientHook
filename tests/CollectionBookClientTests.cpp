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

    // --- local mode: the catalog comes from the cb_*.bin files, the server only tells what the character owns --------------
    {
        CollectionBin::Catalog bin;
        CollectionBin::Rune r1; r1.id = 1; r1.icon = 5001; r1.name = "Rune of Fire"; r1.itemName = "Fire Tablet";
        r1.needs[0] = {2983726, 1, "Fire Tablet"}; r1.needs[1] = {777, 30, "Rune Dust"};
        CollectionBin::Rune r2; r2.id = 2; r2.iconFile = "ice"; r2.name = "Rune of Ice"; r2.itemName = "Ice Tablet"; r2.needs[0] = {2983727, 1, "Ice Tablet"};
        bin.runes = {r1, r2};
        CollectionBin::Set set; set.id = 101; set.name = "Rune Set"; set.runes[0] = 1; set.runes[1] = 2; set.crones = 1500000;
        bin.sets = {set};
        CollectionBin::Bonus b1; b1.setId = 101; b1.count = 1; b1.kind = 1; b1.value = 5; b1.divisor = 2; b1.perLevel = 1;
        CollectionBin::Bonus b2; b2.setId = 101; b2.count = 2; b2.kind = 4; b2.value = 3; b2.divisor = 1; b2.perLevel = 0;
        bin.bonuses = {b1, b2};
        CollectionBin::Level l1; l1.setId = 101; l1.level = 1; l1.crones = 100000; l1.rate = 50000; l1.pity = 5000;
        l1.items[0].item = 777; l1.items[0].amount = 3; l1.items[0].name = "Rune Dust";
        bin.levels = {l1};
        CollectionBin::Reward w1; w1.setId = 101; w1.tier = 1; w1.item = 555; w1.amount = 10; w1.name = "Potion";
        bin.rewards = {w1};
        const std::string dir = ".";
        Check(CollectionBin::Write(dir, bin), "the test bin files are written");
        Client c;
        std::string error;
        Check(c.LoadLocalCatalog(dir, error) && c.Local(), "the local catalog loads");
        c.RequestList(1, 0);
        c.Tick(1000);
        auto cmds = c.TakeCommands();
        Check(cmds.size() == 1 && cmds[0].rfind(".cbk1 mine ", 0) == 0, "the first list asks the server what the character owns");
        Check(c.CurrentPage().loaded && c.CurrentPage().sets.size() == 1 && c.CurrentPage().sets[0].owned == 0 && c.Status().empty(), "the book shows the catalog at once, before the owned data arrives");
        const auto n = NonceOf(cmds[0]);
        c.Receive(Wait(n));
        c.Receive(MineState(n, 2, 1));
        OwnEntry own[2] = {{101, 3u | OwnBit, 2u, 3u}, {RuneBase + 1, 1u, 0u, 0u}};   // set 101 has failed three refines in a row
        c.Receive(OwnRow(n, 0, own, 2));
        Check(!c.Busy() && c.CurrentPage().loaded && c.CurrentPage().sets.size() == 1, "the page is composed locally");
        const auto& v = c.CurrentPage().sets[0];
        Check(v.name == "Rune Set" && v.owned == (3u | OwnBit) && v.items[0] == 5001 && v.items[1] == (CollectionBin::IconImageFlag | (CollectionBin::IconImageBase + 1)), "names, item icons and .gtx picture ids");
        Check(v.effect.size() > 5 && v.effect[0] == "[Activation Cost]" && v.effect[1] == "1,500,000 Crones", "the effect text shows the activation cost");
        Check(v.effect[4] == "Rune of Fire (Rune ID: 1)", "the effect text lists the requirements");
        Check(v.effect[v.effect.size() - 2] == "1 Rune: VIT +5 (+1 per 2 Lv)" && v.effect.back() == "2 Runes: AGI +3", "the effect text lists the bonus rows");
        Check(v.upgrade.size() > 5 && v.upgrade[1] == "Current level: +0" && v.upgrade[3] == "Success rate: 65.0%" && v.upgrade[4] == "Base 50.0%, +5.0% per failure" && v.upgrade[5] == "Failures in a row: 3", "the refine text shows the chance including the failure bonus");
        Check(v.upgrade.back() == "Rune Dust x3", "the refine text lists the materials");
        Check(v.rewards.size() == 2 && v.rewards[0] == "[Claimed] Activation" && v.rewards[1] == "Potion x10", "the rewards text uses the claimed bits");
        c.RequestList(2, 0);
        Check(c.CurrentPage().category == 2 && c.CurrentPage().sets.size() == 2 && !c.Busy(), "another category needs no round trip");
        Check(c.CurrentPage().sets[0].id == RuneBase + 1 && c.CurrentPage().sets[0].name == "Rune of Fire" && c.CurrentPage().sets[0].owned == 1u, "runes are listed by their own name");
        Check(c.CurrentPage().sets[0].effect[0] == "[Requirement]" && c.CurrentPage().sets[0].effect.size() == 3 && c.CurrentPage().sets[0].effect[1] == "Fire Tablet x 1" && c.CurrentPage().sets[0].effect[2] == "Rune Dust x 30", "rune requirement text lists every item");
        Check(c.CurrentPage().sets[0].needItem[0] == 2983726 && c.CurrentPage().sets[0].needAmount[0] == 1 && c.CurrentPage().sets[0].needItem[1] == 777 &&
              c.CurrentPage().sets[0].needAmount[1] == 30 && c.CurrentPage().sets[0].needItem[2] == 0, "a rune carries its requirement items for the boxes");
        c.RequestList(1, 0);
        c.RequestInfo(101);
        Check(!c.Busy(), "info is answered locally");
        // an upgrade answer changes the level and the refine text at once
        c.RequestUpgrade(101);
        c.Tick(2000);
        cmds = c.TakeCommands();
        Check(cmds.size() == 1 && cmds[0].rfind(".cbk1 up ", 0) == 0, "upgrade still goes to the server");
        c.Receive(UpResult(NonceOf(cmds[0]), 0, 101, 3u | OwnBit | (1u << LevelShift)));
        Check(c.TakeRefineEvent() == 1 && c.TakeRefineEvent() == 0, "a successful refine asks for the success sound once");
        Check(c.CurrentPage().sets[0].upgrade[1] == "Current level: +1" && c.CurrentPage().sets[0].upgrade[2] == "Maximum level reached.", "an upgrade answer updates the page");
        // equipping another set unequips the first one
        c.RequestEquip(101, true);
        c.Tick(3000);
        cmds = c.TakeCommands();
        c.Receive(EquipResult(NonceOf(cmds[0]), 0, 101, 1, 3u | OwnBit | ActiveBit));
        Check(Active(c.CurrentPage().sets[0]), "an equip answer updates the page");
        // an empty book
        Client e;
        Check(e.LoadLocalCatalog(dir, error), "loads again");
        e.RequestList(1, 0);
        e.Tick(1000);
        cmds = e.TakeCommands();
        e.Receive(MineState(NonceOf(cmds[0]), 0, 0));
        Check(!e.Busy() && e.CurrentPage().sets.size() == 1 && e.CurrentPage().sets[0].owned == 0, "a character without anything still sees the catalog");
        // a broken file set is refused and the client stays server driven
        Client bad;
        Check(!bad.LoadLocalCatalog("nonexistent-folder", error) && !bad.Local() && !error.empty(), "missing files are reported");
        for (const char* f : {"cb_rune.bin", "cb_set.bin", "cb_bonus.bin", "cb_level.bin", "cb_reward.bin"}) std::remove(f);
    }

    std::printf("Collection Book client tests passed (%d checks).\n", checks);
    return 0;
}
