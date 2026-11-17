#include "../src/EventShop/EventShopWindow.h"
#include <cstdio>
#include <cstdlib>
#include <climits>

namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks; if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
EventShopWire::Request Take(EventShopWindow& window) {
    const auto commands = window.TakeCommands();
    EventShopWire::Request request;
    Check(commands.size() == 1 && EventShopWire::ParseRequest(commands[0], request), "one valid request");
    return request;
}
EventShopWire::Offer Stone() { return {1, 2884093, 250, 10, 123, "Guild Stone"}; }
void ApproveOpen(EventShopWindow& window, const EventShopWire::Request& request) {
    Check(request.action == EventShopWire::Action::Open, "shop requests access first");
    window.Receive("EPS1 opened " + std::to_string(request.nonce));
}
void Reply(EventShopWindow& w, const EventShopWire::Request& r, std::int64_t balance = 30) {
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " " + std::to_string(r.page) + " 1 1 " + std::to_string(balance) + " " + std::to_string(r.category));
    Check(!w.Ready(), "partial catalog cannot buy");
    w.Receive(EventShopWire::Row(r.nonce, 0, Stone()));
    Check(!w.Ready() && !w.Buy(0), "row without details cannot buy");
    w.Receive(EventShopWire::Detail(r.nonce, 0, Stone()));
    Check(w.Ready() && w.Offers().size() == 1 && w.Balance() == balance, "complete catalog published");
}
}
int main() {
    using namespace EventShopWire;
    Request parsed;
    Check(!ParseRequest(".eps1 list 1 0 0", parsed), "All category removed");
    Check(ParseRequest(".eps1 list 1 0 4", parsed), "Other category accepted");
    Check(!ParseRequest(".eps1 list 1 0 5", parsed), "unknown category rejected");
    Check(!ParseRequest(".eps1 list 0 0 1", parsed), "zero nonce rejected");
    Check(!ParseRequest(".eps1 list 4294967296 0 1", parsed), "nonce overflow rejected");
    Check(!ParseRequest(".eps1 list 1 -1 1", parsed), "negative page rejected");
    Check(!ParseRequest(".eps1 list 1 10000 1", parsed), "page bound");
    Check(!ParseRequest(".eps1 list 1 0 1 extra", parsed), "extra field rejected");
    Check(!ParseRequest(".eps1  list 1 0 1", parsed), "empty field rejected");
    Check(!Guid("00000000-0000-0000-0000-000000000000"), "nil purchase key rejected");
    std::string decoded;
    Check(Unhex(Hex("Guild Stone"), decoded) && decoded == "Guild Stone", "name codec");
    Check(!Unhex("00", decoded) && !Unhex("0a", decoded) && !Unhex("xyz", decoded), "name controls rejected");
    auto largest = Stone(); largest.id = INT_MAX; largest.item = INT_MAX; largest.revision = INT64_MAX;
    largest.quantity = 255; largest.price = 1000000000; largest.name.assign(48, 'W');
    largest.description.assign(80, 'X');
    Check(Valid(largest) && Row(UINT32_MAX, 5, largest).size() <= MaxReply, "largest offer fits native transport");
    Check(Detail(UINT32_MAX, 5, largest).size() <= MaxReply && Unhex(Hex(largest.description), decoded, 80) && decoded == largest.description,
        "largest description fits native transport and decodes");
    Check(!Unhex(Hex(largest.description), decoded), "name decoder retains 48 byte bound");
    largest.description += 'X'; Check(!Valid(largest), "81 byte description rejected");
    largest.description = "Melee\nAttack"; Check(!Valid(largest), "description controls rejected");
    Check(Detail(1, 0, Stone()) == "EPS1 detail 1 0 -", "empty description uses explicit marker");

    EventShopWindow w; w.Open(); const auto opening = Take(w);
    Check(!w.Visible(), "window stays hidden pending safe-zone check");
    ApproveOpen(w, opening);
    auto r = Take(w);
    Check(!w.Visible() && r.action == Action::List && r.category == 1 && r.page == 0,
        "approved open loads Equipment before showing window");
    w.Receive("EPS1 state " + std::to_string(r.nonce + 1) + " 0 1 0 99 1");
    Check(!w.Ready() && !w.Buy(0), "stale response ignored");
    Reply(w, r); Check(w.Visible(), "safe catalog opens the shop");
    Check(!w.SelectCategory(0) && !w.SelectCategory(5), "category boundaries");
    Check(w.SelectCategory(3), "select Materials"); r = Take(w);
    Check(r.category == 3 && r.page == 0, "category change resets page"); Reply(w, r);
    Check(w.Buy(0), "buy valid offer"); auto purchase = Take(w);
    Check(purchase.action == Action::Buy && Guid(purchase.key) && purchase.offer == 1 && purchase.revision == 123, "purchase only sends identity and quote");
    w.Receive("EPS1 error " + std::to_string(purchase.nonce) + " 3");
    Check(w.PendingPurchase() && !w.Retry() && w.TakeCommands().empty(), "busy purchase delays automatic retry");
    w.Tick(std::chrono::steady_clock::now() + std::chrono::milliseconds(650));
    auto busyPurchase = Take(w);
    Check(busyPurchase.key == purchase.key && busyPurchase.nonce != purchase.nonce && busyPurchase.offer == purchase.offer &&
        busyPurchase.revision == purchase.revision, "busy purchase keeps GUID and quote with fresh nonce");
    purchase = busyPurchase;
    Check(!w.Buy(0) && !w.Load(0) && !w.SelectCategory(2), "pending purchase blocks duplicates and navigation");
    w.Close(); w.Open(); const auto pendingOpen = Take(w);
    Check(w.PendingPurchase() && !w.Visible(), "pending purchase does not bypass the open gate");
    ApproveOpen(w, pendingOpen);
    Check(w.PendingPurchase() && w.Visible() && w.TakeCommands().empty(), "approved reopen preserves pending purchase");
    w.Receive("EPS1 buy " + std::to_string(purchase.nonce) + " 11111111-1111-1111-1111-111111111111 0 20 1234");
    Check(w.PendingPurchase(), "mismatched purchase key ignored");
    w.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(16));
    Check(w.PendingPurchase() && w.Retry(), "timeout permits same purchase check"); auto retry = Take(w);
    Check(retry.key == purchase.key && retry.nonce != purchase.nonce && retry.revision == purchase.revision, "retry same transaction with fresh transport nonce");
    w.Receive("EPS1 error " + std::to_string(retry.nonce) + " 2");
    Check(w.PendingPurchase() && w.Retry(), "unknown database outcome never discards purchase key"); retry = Take(w);
    w.Receive("EPS1 buy " + std::to_string(retry.nonce) + " " + purchase.key + " 0 20 1234");
    Check(!w.PendingPurchase() && !w.Ready(), "receipt triggers refresh"); r = Take(w);
    Check(r.category == 3, "purchase refresh keeps category");
    w.Receive("EPS1 error " + std::to_string(r.nonce) + " 3");
    Check(w.TakeCommands().empty(), "busy list waits before retry");
    w.Tick(std::chrono::steady_clock::now() + std::chrono::milliseconds(650));
    auto busyRetry = Take(w); Check(busyRetry.nonce != r.nonce, "busy list uses fresh request"); r = busyRetry; Reply(w, r, 20);
    w.Receive("EPS1 buy " + std::to_string(purchase.nonce) + " " + purchase.key + " 0 20 1234");
    Check(w.Ready() && w.TakeCommands().empty(), "duplicate receipt has no effect");
    Check(w.Buy(0), "intentional second purchase allowed"); auto second = Take(w);
    Check(second.key != purchase.key, "new intentional purchase has new GUID");
    w.Receive("EPS1 buy " + std::to_string(second.nonce) + " " + second.key + " 4 0 0");
    r = Take(w); Reply(w, r, 0); Check(!w.Buy(0), "insufficient displayed balance disables buy");
    w.Load(0); r = Take(w);
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 1 7 100 3");
    Check(!w.Ready() && !w.Buy(0), "oversized catalog refused");
    w.Load(0); r = Take(w);
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 1 1 100 2");
    Check(!w.Ready(), "wrong category response refused");
    w.Load(0); r = Take(w);
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 0 0 100 3");
    Check(w.Ready() && w.Offers().empty(), "empty category works");
    w.Load(0); r = Take(w);
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 1 1 100 3");
    auto sword = Stone(); sword.item=2621591; sword.name="Split Sword"; sword.description="Melee Attack +10%";
    w.Receive(Detail(r.nonce,0,sword));
    Check(!w.Ready(), "description without row cannot publish");
    w.Receive(Row(r.nonce,0,sword));
    Check(w.Ready() && w.Offers()[0].description == sword.description, "description-before-row preserved");
    w.Load(0); r = Take(w);
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 1 1 100 3");
    w.Receive(Row(r.nonce,0,sword));
    w.Receive("EPS1 detail " + std::to_string(r.nonce) + " 0 " + Hex(std::string(81,'X')));
    Check(!w.Ready() && !w.Buy(0), "oversized description fails closed");
    w.Load(0); r = Take(w);
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 1 1 100 3");
    w.Receive(Row(r.nonce,0,sword));
    w.Receive("EPS1 detail " + std::to_string(r.nonce) + " 1 -");
    Check(!w.Ready(), "out-of-range description rejected");
    w.Load(0); r = Take(w); Reply(w,r);
    Check(w.Buy(0), "new purchase for bounded busy retry test"); auto bounded = Take(w);
    w.Receive("EPS1 error " + std::to_string(bounded.nonce) + " 3");
    const auto busyStarted = std::chrono::steady_clock::now();
    w.Tick(busyStarted + std::chrono::seconds(14)); bounded = Take(w);
    w.Receive("EPS1 error " + std::to_string(bounded.nonce) + " 3");
    w.Tick(busyStarted + std::chrono::seconds(16));
    Check(w.PendingPurchase() && w.Retry(), "busy retries do not extend original deadline");
    const auto recovery = Take(w);
    const auto outside = [](const EventShopWire::Request& request) {
        return "EPS1 error " + std::to_string(request.nonce) + " " + std::to_string(SafeZoneRequired);
    };
    w.Receive(outside(recovery));
    Check(w.PendingPurchase() && w.Offers().empty() && w.Status().find("safe zone") != std::string::npos,
        "safe-zone denial during recovery retains uncertain purchase key");
    Check(w.Retry() && Take(w).key == recovery.key, "recovery uses the same key back in a safe zone");
    EventShopWindow entry;
    entry.Open();
    entry.Receive(outside(Take(entry)));
    Check(!entry.Visible() && !entry.Ready() && entry.TakeCommands().empty() &&
        entry.TakeChatNotice().find("safe zone") != std::string::npos && entry.TakeChatNotice().empty(),
        "safe-zone denial prevents the popup and produces one chat notice");
    entry.Open(); ApproveOpen(entry, Take(entry));
    entry.Receive(outside(Take(entry)));
    Check(!entry.Visible() && !entry.Ready() && entry.TakeChatNotice().find("safe zone") != std::string::npos,
        "leaving a safe zone during catalog loading also prevents the popup");
    entry.Open(); ApproveOpen(entry, Take(entry)); const auto safeList = Take(entry); Reply(entry, safeList);
    Check(entry.Buy(0), "can attempt to buy from a previously valid catalog");
    entry.Receive(outside(Take(entry)));
    Check(!entry.Visible() && !entry.PendingPurchase() && !entry.Ready() && entry.Offers().empty() && !entry.Buy(0),
        "safe-zone denial of first buy closes shop and clears the rejected attempt");
    entry.Open(); const auto cancelled = Take(entry); entry.Close();
    ApproveOpen(entry, cancelled);
    Check(!entry.Visible() && entry.TakeCommands().empty(), "late approval cannot reopen a closed shop");
    w.ResetSession(); Check(!w.Visible() && !w.Ready() && w.Offers().empty() && !w.PendingPurchase(), "session state cleared");
    w.Receive("EPS1 state " + std::to_string(r.nonce) + " 0 1 1 100 3");
    Check(!w.Ready(), "previous account reply ignored");
    std::printf("Event shop: %u checks passed.\n", checks);
}
