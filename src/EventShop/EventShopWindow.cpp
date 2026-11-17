#include "EventShopWindow.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <utility>
#ifndef AHK_WINDOW_MODEL_ONLY
#include "imgui.h"
#include "ItemIcons.h"
#endif

namespace {
constexpr char SafeZoneNotice[] = "The Event Point Shop is only available in safe zones.";

std::string NewKey() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) return {};
    char text[37]{};
    std::snprintf(text, sizeof text, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return text;
}
const char* BuyResult(unsigned result) {
    switch (result) {
    case 0: return "Purchase complete. Collect your item from Event Inventory.";
    case 1: return "Your character could not be verified. Reopen the shop.";
    case 2: return "This offer is no longer available.";
    case 3: return "This offer changed. Review its updated price before buying.";
    case 4: return "You do not have enough Event Points.";
    case 5: return "Purchase reference mismatch. Check Event Inventory before trying again.";
    case 6: return "Your Event Inventory is full. Collect items before buying.";
    default: return "The purchase could not be confirmed.";
    }
}
}
std::uint32_t EventShopWindow::NextNonce() {
    static std::uint32_t next = static_cast<std::uint32_t>(Clock::now().time_since_epoch().count());
    if (!++next) ++next;
    return next;
}
void EventShopWindow::ResetSession() {
    *this = EventShopWindow{};
}
void EventShopWindow::Open() {
    if (openNonce_ || openAfterLoad_) return;
    openNonce_ = NextNonce();
    sendOpen_ = true;
    openDeadline_ = Clock::now() + std::chrono::seconds(5);
}
void EventShopWindow::Start(Phase phase, const std::string& command) {
    phase_ = phase;
    listRetry_ = purchaseRetry_ = false;
    commands_.clear(); commands_.push_back(command);
    deadline_ = Clock::now() + std::chrono::seconds(15);
    pollAt_ = Clock::now() + std::chrono::milliseconds(500);
}
bool EventShopWindow::Load(unsigned page) {
    if (PendingPurchase() || page > 9999) return false;
    nonce_ = NextNonce(); page_ = page; expected_ = received_ = details_ = 0; state_ = false;
    confirm_ = -1; scrollToTop_ = true; offers_.clear(); incoming_ = {};
    status_ = "Loading Event Points and offers...";
    Start(Phase::Loading, ".eps1 list " + std::to_string(nonce_) + " " + std::to_string(page) + " " + std::to_string(category_));
    return true;
}
bool EventShopWindow::SelectCategory(unsigned category) {
    if (category < 1 || category > EventShopWire::CategoryCount || PendingPurchase() || phase_ == Phase::Loading) return false;
    category_ = category; return Load(0);
}
bool EventShopWindow::Buy(unsigned index) {
    if (!Ready() || PendingPurchase() || index >= offers_.size() || balance_ < offers_[index].price) return false;
    const auto& offer = offers_[index];
    purchaseKey_ = NewKey();
    if (purchaseKey_.empty()) { Fail("Could not create a purchase reference. Please try again."); return false; }
    nonce_ = NextNonce();
    purchaseCommand_ = ".eps1 buy " + std::to_string(nonce_) + " " + purchaseKey_ + " " +
        std::to_string(offer.id) + " " + std::to_string(offer.revision);
    status_ = "Confirming purchase..."; notice_.clear(); confirm_ = -1;
    recoveringPurchase_ = false;
    Start(Phase::Buying, purchaseCommand_); return true;
}
bool EventShopWindow::Retry() {
    if (!PendingPurchase() || phase_ != Phase::Uncertain) return false;
    if (!RenewPurchaseNonce()) return false;
    recoveringPurchase_ = true;
    status_ = "Checking the same purchase...";
    Start(Phase::Buying, purchaseCommand_); return true;
}
bool EventShopWindow::RenewPurchaseNonce() {
    // Keep the exact GUID and quote. Use a fresh transport nonce to bypass
    // a cached transient database error; SQL replays the original receipt.
    const auto old = EventShopWire::Words(purchaseCommand_);
    if (old.size() != 6) return false;
    const std::string offer(old[4]), revision(old[5]);
    nonce_ = NextNonce();
    purchaseCommand_ = ".eps1 buy " + std::to_string(nonce_) + " " + purchaseKey_ + " " + offer + " " + revision;
    return true;
}
void EventShopWindow::Fail(const char* message) {
    phase_ = PendingPurchase() ? Phase::Uncertain : Phase::Failed;
    status_ = message; commands_.clear();
    if (std::exchange(openAfterLoad_, false)) chatNotice_ = message;
}
void EventShopWindow::Tick(Clock::time_point now) {
    if (openNonce_ && now >= openDeadline_) {
        openNonce_ = 0;
        chatNotice_ = "The Event Point Shop did not respond. Try .epshop again.";
    }
    if (phase_ != Phase::Loading && phase_ != Phase::Buying) return;
    if (now >= deadline_) {
        Fail(PendingPurchase() ? "Confirmation timed out. Check the same purchase before buying again." :
             "The shop did not respond. Refresh to try again."); return;
    }
    if (now >= pollAt_) {
        if (listRetry_) {
            listRetry_ = false; nonce_ = NextNonce(); state_ = false; received_ = details_ = 0; incoming_ = {};
            commands_.clear(); commands_.push_back(".eps1 list " + std::to_string(nonce_) + " " + std::to_string(page_) + " " + std::to_string(category_));
        } else if (purchaseRetry_) {
            purchaseRetry_ = false;
            if (!RenewPurchaseNonce()) { Fail("Purchase could not be retried. Check Event Inventory."); return; }
            commands_.clear(); commands_.push_back(purchaseCommand_);
        } else if (commands_.size() < 4) commands_.push_back(".eps1 poll " + std::to_string(nonce_));
        pollAt_ = now + std::chrono::milliseconds(500);
    }
}
std::vector<std::string> EventShopWindow::TakeCommands() {
    Tick();
    auto result = std::exchange(commands_, {});
    if (std::exchange(sendOpen_, false) && openNonce_)
        result.push_back(".eps1 open " + std::to_string(openNonce_));
    return result;
}

std::string EventShopWindow::TakeChatNotice() { return std::exchange(chatNotice_, {}); }

void EventShopWindow::Receive(std::string_view text) {
    using namespace EventShopWire;
    if (text.size() > MaxReply) return;
    const auto w = Words(text);
    std::uint32_t nonce = 0;
    if (w.size() < 3 || w[0] != "EPS1" || !Number(w[2], nonce) || !nonce) return;
    if (nonce == openNonce_) {
        if (Clock::now() >= openDeadline_) { Tick(); return; }
        if (w[1] == "opened" && w.size() == 3) {
            openNonce_ = 0;
            if (PendingPurchase()) visible_ = true;
            else {
                openAfterLoad_ = !visible_;
                if (phase_ != Phase::Loading) Load(page_);
            }
        } else if (w[1] == "error" && w.size() == 4) {
            unsigned code = 0;
            if (!Number(w[3], code)) return;
            if (code == SafeZoneRequired) Close();
            else openNonce_ = 0;
            chatNotice_ = code == SafeZoneRequired ? SafeZoneNotice :
                code == 1 ? "The Event Point Shop is disabled on this server." :
                code == 3 ? "The shop could not check your position. Try .epshop again." :
                "The Event Point Shop is unavailable. Try .epshop again.";
        }
        return;
    }
    if (nonce != nonce_) return;
    if (phase_ != Phase::Loading && phase_ != Phase::Buying && phase_ != Phase::Uncertain) return;
    if (w[1] == "wait" && w.size() == 3) return;
    if (w[1] == "error" && w.size() == 4) {
        unsigned code = 0;
        if (!Number(w[3], code)) return;
        if (code == SafeZoneRequired) {
            // The denied attempt was never queued. A prior uncertain attempt
            // may have committed, so retain its purchase key for recovery.
            if (!recoveringPurchase_ && phase_ != Phase::Uncertain) {
                purchaseKey_.clear();
                purchaseCommand_.clear();
            }
            Close();
            offers_.clear();
            notice_.clear();
            chatNotice_ = PendingPurchase() ? "Return to a safe zone and reopen the shop to check your purchase." : SafeZoneNotice;
            Fail(chatNotice_.c_str());
            return;
        }
        if (code == 3 && phase_ == Phase::Loading) {
            listRetry_ = true; commands_.clear(); pollAt_ = Clock::now() + std::chrono::milliseconds(600);
            status_ = "The shop is busy. Retrying..."; return;
        }
        if (code == 3 && phase_ == Phase::Buying) {
            // Busy means this attempt was not queued. Keep the purchase identity
            // and original deadline; only renew its transport nonce after delay.
            purchaseRetry_ = true; commands_.clear(); pollAt_ = Clock::now() + std::chrono::milliseconds(600);
            status_ = "The shop is busy. Retrying the same purchase..."; return;
        }
        Fail(PendingPurchase() ? "Purchase not confirmed. Use Check purchase to retry safely." :
            code == 1 ? "The Event Point Shop is disabled on this server." :
            code == 3 ? "The shop is busy. Please refresh shortly." : "The shop is unavailable. Please refresh shortly.");
        return;
    }
    if (PendingPurchase()) {
        unsigned result = 0; std::int64_t balance = 0; int item = 0;
        if (w[1] != "buy" || w.size() != 7 || w[3] != purchaseKey_ ||
            !Number(w[4], result) || result > 6 || !Number(w[5], balance) || balance > MaxBalance ||
            !Number(w[6], item) || (result == 0 && item <= 0)) return;
        balance_ = balance; balanceKnown_ = true; notice_ = BuyResult(result);
        purchaseKey_.clear(); purchaseCommand_.clear();
        Load(0); return;
    }
    if (phase_ != Phase::Loading) return;
    if (w[1] == "state" && w.size() == 8) {
        unsigned page = 0, pages = 0, count = 0, category = 0; std::int64_t balance = 0;
        if (!Number(w[3], page) || page != page_ || !Number(w[4], pages) || pages > 10000 ||
            !Number(w[5], count) || count > PageSize || !Number(w[6], balance) || balance > MaxBalance ||
            !Number(w[7], category) || category != category_ ||
            (pages && page >= pages) || (!pages && (page || count))) { Fail("Invalid shop response. Refresh to try again."); return; }
        // A repeated poll may replay the same complete response. Reset assembly
        // only on state, then publish after every row and its detail arrive.
        state_ = true; expected_ = count; received_ = details_ = 0; incoming_ = {};
        pages_ = pages; balance_ = balance; balanceKnown_ = true;
    } else if (w[1] == "row" && w.size() == 10 && state_) {
        unsigned index = 0; Offer o;
        if (!Number(w[3], index) || index >= expected_ || !Number(w[4], o.id) ||
            !Number(w[5], o.revision) || !Number(w[6], o.item) || !Number(w[7], o.quantity) ||
            !Number(w[8], o.price) || !Unhex(w[9], o.name) || !Valid(o)) {
            Fail("Invalid offer received. Refresh to try again."); return;
        }
        // Detail may arrive before the corresponding row.
        o.description = std::move(incoming_[index].description);
        incoming_[index] = std::move(o); received_ |= 1u << index;
    } else if (w[1] == "detail" && w.size() == 5 && state_) {
        unsigned index = 0; std::string description;
        if (!Number(w[3], index) || index >= expected_ ||
            (w[4] != "-" && !Unhex(w[4], description, 80))) {
            Fail("Invalid item details received. Refresh to try again."); return;
        }
        incoming_[index].description = std::move(description); details_ |= 1u << index;
    } else return;
    if (state_ && received_ == ((1u << expected_) - 1) && details_ == received_) {
        offers_.assign(incoming_.begin(), incoming_.begin() + expected_);
        phase_ = Phase::Ready; commands_.clear();
        if (std::exchange(openAfterLoad_, false)) visible_ = true;
        status_ = offers_.empty() ? "No offers are available yet." : "Purchases are delivered to Event Inventory.";
    }
}
#ifndef AHK_WINDOW_MODEL_ONLY
namespace {
constexpr ImU32 Background = IM_COL32(17, 23, 32, 255);
constexpr ImU32 Surface = IM_COL32(24, 33, 45, 255);
constexpr ImU32 Border = IM_COL32(49, 63, 80, 255);
constexpr ImU32 Text = IM_COL32(237, 241, 247, 255);
constexpr ImU32 Muted = IM_COL32(161, 178, 198, 255);
constexpr ImU32 Gold = IM_COL32(246, 198, 109, 255);
ImFont* shopFont = nullptr;
ImFont* shopBold = nullptr;

ImVec4 Tint(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

enum class ButtonTone { Neutral, Selected, Primary };
bool ShopButton(const char* label, ImVec2 size, ButtonTone tone = ButtonTone::Neutral) {
    ImU32 base = IM_COL32(32, 45, 61, 255);
    ImU32 hover = IM_COL32(44, 60, 79, 255);
    ImU32 ink = Text;
    if (tone == ButtonTone::Selected) {
        base = IM_COL32(64, 50, 34, 255);
        hover = IM_COL32(80, 61, 40, 255);
        ink = Gold;
    } else if (tone == ButtonTone::Primary) {
        base = Gold;
        hover = IM_COL32(255, 218, 149, 255);
        ink = IM_COL32(30, 24, 16, 255);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, Tint(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tint(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tint(hover));
    ImGui::PushStyleColor(ImGuiCol_Text, Tint(ink));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool DrawShopHeader(const std::string& balance) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool compact = width < 400.f;
    const float balanceWidth = compact ? 141.f : (std::min)(172.f, width * .40f);
    const float walletHeight = compact ? 52.f : 66.f;
    const ImVec2 wallet(start.x + width - balanceWidth - (compact ? 0.f : 46.f),
                        start.y + (compact ? 40.f : 0.f));
    const ImVec2 walletEnd(wallet.x + balanceWidth, wallet.y + walletHeight);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(wallet, walletEnd, Surface, 10.f);
    draw->AddRect(wallet, walletEnd, Border, 10.f);
    draw->AddRectFilled(ImVec2(wallet.x + 8.f, wallet.y + 10.f),
                        ImVec2(wallet.x + 11.f, walletEnd.y - 10.f), Gold, 2.f);

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 7.f));
    ImGui::PushFont(shopBold, compact ? 23.f : 28.f);
    ImGui::TextUnformatted("Event Point Shop");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (compact ? 54.f : 40.f)));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wallet.x - start.x - 12.f);
    ImGui::PushStyleColor(ImGuiCol_Text, Tint(Muted));
    ImGui::TextUnformatted("Claim items in Event Inventory");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    ImGui::SetCursorScreenPos(ImVec2(wallet.x + 21.f, wallet.y + 8.f));
    ImGui::PushFont(shopBold, 11.f);
    ImGui::TextColored(Tint(Muted), "AVAILABLE EP");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(wallet.x + 21.f, wallet.y + 25.f));
    const float numberSize = balance.size() > 10 ? 12.f : (compact ? 20.f : 25.f);
    ImGui::PushFont(shopBold, numberSize);
    ImGui::TextColored(Tint(Gold), "%s", balance.c_str());
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(start.x + width - 34.f, start.y));
    ImGui::PushFont(shopFont, 20.f);
    const bool close = ShopButton("\xC3\x97##CloseShop", ImVec2(34, 34));
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close shop");
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, compact ? 92.f : 66.f));
    return close;
}

void DrawOfferDetails(IDirect3DDevice9* device, const EventShopWire::Offer& offer) {
    EventShopIcons::Draw(device, offer.item, 52.f);
    if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(offer.name.c_str());
        ImGui::TextColored(Tint(Muted), "Item ID %d  |  Quantity %d", offer.item, offer.quantity);
        if (!offer.description.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280.f);
            ImGui::TextUnformatted(offer.description.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTooltip();
    }
}
}

void EventShopWindow::InitializeFonts() {
    auto& atlas = *ImGui::GetIO().Fonts;
    if (atlas.Fonts.empty()) atlas.AddFontDefault();
    shopFont = shopBold = nullptr;
    char windows[MAX_PATH]{};
    const auto length = GetWindowsDirectoryA(windows, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    char path[MAX_PATH + 32]{};
    ImFontConfig config;
    config.Flags |= ImFontFlags_NoLoadError;
    std::snprintf(path, sizeof path, "%s\\Fonts\\segoeui.ttf", windows);
    shopFont = atlas.AddFontFromFileTTF(path, 15.f, &config);
    std::snprintf(path, sizeof path, "%s\\Fonts\\segoeuib.ttf", windows);
    shopBold = atlas.AddFontFromFileTTF(path, 16.f, &config);
    if (!shopBold) shopBold = shopFont;
}

void EventShopWindow::Draw(IDirect3DDevice9* device) {
    if (!visible_) return;
    Tick();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 365.f || display.y < 285.f) { Close(); return; }
    const ImVec2 available(display.x - 24.f, display.y - 24.f);
    const ImVec2 minimum((std::min)(492.f, available.x), (std::min)(375.f, available.y));
    const ImVec2 maximum((std::min)(900.f, available.x), (std::min)(820.f, available.y));
    ImGui::SetNextWindowSize(ImVec2((std::min)(720.f, maximum.x), (std::min)(510.f, maximum.y)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
    ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_FirstUseEver, ImVec2(.5f, .5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 13.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9, 9));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 9.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Tint(Background));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Tint(Surface));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Tint(Surface));
    ImGui::PushStyleColor(ImGuiCol_Border, Tint(Border));
    ImGui::PushStyleColor(ImGuiCol_Separator, Tint(Border));
    ImGui::PushStyleColor(ImGuiCol_Text, Tint(Text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Tint(Muted));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, Tint(Gold));
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("Event Point Shop###EventPointShop", nullptr, flags)) {
        ImGui::PushFont(shopFont, 16.f);
        if (DrawShopHeader(balanceKnown_ ? std::to_string(balance_) : "--")) Close();
        const bool canBrowse = !PendingPurchase() && phase_ != Phase::Loading;
        const float tabsWidth = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float tabWidth = (tabsWidth - gap * (EventShopWire::CategoryCount - 1)) / EventShopWire::CategoryCount;
        ImGui::PushFont(shopBold, tabsWidth < 420.f ? 12.f : 15.f);
        ImGui::BeginDisabled(!canBrowse);
        for (unsigned i = 0; i < EventShopWire::CategoryCount; ++i) {
            if (i > 0) ImGui::SameLine();
            const auto tone = category_ == i + 1 ? ButtonTone::Selected : ButtonTone::Neutral;
            if (ShopButton(EventShopWire::Categories[i], ImVec2(tabWidth, 36.f), tone) && category_ != i + 1)
                SelectCategory(i + 1);
        }
        ImGui::EndDisabled();
        ImGui::PopFont();

        if (!notice_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Tint(Gold));
            ImGui::TextWrapped("%s", notice_.c_str());
            ImGui::PopStyleColor();
        }
        if (!Ready() && !offers_.empty()) ImGui::TextWrapped("%s", status_.c_str());
        if (scrollToTop_) {
            ImGui::SetNextWindowScroll(ImVec2(0, 0));
            scrollToTop_ = false;
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Tint(Background));
        const bool showOffers = ImGui::BeginChild("##Offers", ImVec2(0, -56.f));
        ImGui::PopStyleColor();
        if (showOffers && offers_.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, Tint(Muted));
            ImGui::TextWrapped("%s", Ready() ? "No offers in this category." : status_.c_str());
            ImGui::PopStyleColor();
        }
        if (showOffers) {
            for (unsigned i = 0; i < offers_.size(); ++i) {
                const auto& offer = offers_[i];
                const bool confirming = Ready() && confirm_ == static_cast<int>(i);
                ImGui::PushID(offer.id);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(13, 12));
                if (confirming) ImGui::PushStyleColor(ImGuiCol_Border, Tint(Gold));
                const auto cardFlags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
                if (ImGui::BeginChild("##Offer", ImVec2(0, 0), cardFlags, ImGuiWindowFlags_NoScrollbar)) {
                    const float purchaseWidth = ImGui::GetContentRegionAvail().x < 450.f ? 94.f : 116.f;
                    constexpr auto columns = ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_SizingFixedFit;
                    if (ImGui::BeginTable("##OfferColumns", 3, columns)) {
                        ImGui::TableSetupColumn("Icon", ImGuiTableColumnFlags_WidthFixed, 57.f);
                        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Purchase", ImGuiTableColumnFlags_WidthFixed, purchaseWidth);
                        ImGui::TableNextColumn();
                        DrawOfferDetails(device, offer);
                        ImGui::TableNextColumn();
                        ImGui::PushFont(shopBold, 18.f);
                        ImGui::TextWrapped("%s", offer.name.c_str());
                        ImGui::PopFont();
                        ImGui::TextColored(Tint(Muted), "Quantity %d", offer.quantity);
                        if (!offer.description.empty()) ImGui::TextWrapped("%s", offer.description.c_str());
                        ImGui::TableNextColumn();
                        ImGui::PushFont(shopBold, offer.price >= 10000000 ? 13.f : 18.f);
                        ImGui::TextColored(Tint(Gold), "%d EP", offer.price);
                        ImGui::PopFont();
                        ImGui::BeginDisabled(!Ready() || PendingPurchase() || balance_ < offer.price || confirming);
                        if (ShopButton("Buy", ImVec2(purchaseWidth, 32.f), ButtonTone::Primary))
                            confirm_ = static_cast<int>(i);
                        ImGui::EndDisabled();
                        if (Ready() && balance_ < offer.price && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Not enough Event Points");
                        ImGui::EndTable();
                    }
                    if (confirming) {
                        ImGui::Separator();
                        ImGui::TextWrapped("Buy %s x%d for %d EP?", offer.name.c_str(), offer.quantity, offer.price);
                        if (ShopButton("Confirm purchase", ImVec2(144, 32), ButtonTone::Primary)) Buy(i);
                        ImGui::SameLine();
                        if (ShopButton("Cancel", ImVec2(82, 32))) confirm_ = -1;
                    }
                }
                ImGui::EndChild();
                if (confirming) ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (phase_ == Phase::Uncertain) {
            if (ShopButton("Check purchase", ImVec2(126, 31), ButtonTone::Primary)) Retry();
        } else {
            ImGui::BeginDisabled(!canBrowse);
            if (ShopButton("Refresh", ImVec2(82, 31))) Load(page_);
            const bool canPage = pages_ > 1;
            if (canPage) {
                ImGui::SameLine(tabsWidth - 187.f);
                ImGui::BeginDisabled(page_ == 0);
                if (ShopButton("Previous", ImVec2(76, 31))) Load(page_ - 1);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Tint(Muted), "%u / %u", page_ + 1, pages_);
                ImGui::SameLine();
                ImGui::BeginDisabled(page_ + 1 >= pages_);
                if (ShopButton("Next", ImVec2(56, 31))) Load(page_ + 1);
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(6);
}
#else
void EventShopWindow::InitializeFonts() {}
void EventShopWindow::Draw(IDirect3DDevice9*) {}
#endif
