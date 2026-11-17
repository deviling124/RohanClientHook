#pragma once
#include "../../shared/EventShopProtocol.h"
#include <chrono>
struct IDirect3DDevice9;

class EventShopWindow {
public:
    static void InitializeFonts();
    void Open();
    void Close() { visible_ = false; confirm_ = -1; openNonce_ = 0; openAfterLoad_ = false; }
    void ResetSession();
    bool Visible() const { return visible_; }
    bool Ready() const { return phase_ == Phase::Ready; }
    bool PendingPurchase() const { return !purchaseKey_.empty(); }
    bool Load(unsigned page);
    bool SelectCategory(unsigned category);
    bool Buy(unsigned index);
    bool Retry();
    void Receive(std::string_view text);
    std::vector<std::string> TakeCommands();
    std::string TakeChatNotice();
    void Draw(IDirect3DDevice9* device);
    void Tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    const std::vector<EventShopWire::Offer>& Offers() const { return offers_; }
    const std::string& Status() const { return status_; }
    std::int64_t Balance() const { return balance_; }
private:
    enum class Phase { Idle, Loading, Buying, Ready, Failed, Uncertain };
    using Clock = std::chrono::steady_clock;
    void Start(Phase phase, const std::string& command);
    void Fail(const char* message);
    bool RenewPurchaseNonce();
    static std::uint32_t NextNonce();
    bool visible_ = false, state_ = false;
    bool listRetry_ = false, purchaseRetry_ = false;
    bool recoveringPurchase_ = false;
    bool scrollToTop_ = false, balanceKnown_ = false;
    bool sendOpen_ = false, openAfterLoad_ = false;
    Phase phase_ = Phase::Idle;
    std::uint32_t nonce_ = 0, openNonce_ = 0;
    unsigned page_ = 0, pages_ = 0, expected_ = 0, received_ = 0, details_ = 0;
    unsigned category_ = 1;
    std::int64_t balance_ = 0;
    std::array<EventShopWire::Offer, EventShopWire::PageSize> incoming_{};
    std::vector<EventShopWire::Offer> offers_;
    std::string purchaseKey_, purchaseCommand_;
    std::string status_ = "Type .epshop in Say chat to open the shop.", notice_, chatNotice_;
    std::vector<std::string> commands_;
    Clock::time_point deadline_{}, pollAt_{}, openDeadline_{};
    int confirm_ = -1;
};
