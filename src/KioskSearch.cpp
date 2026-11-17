#include "KioskSearch.h"
#include "../shared/KioskSearchProtocol.h"
#include "detours.h"
#include <cstring>
#include <mutex>

namespace KioskSearch {
namespace {
using namespace KioskSearchProtocol;
using SelectFn = void(__thiscall*)(void*, int);
using DecodeFn = int(__cdecl*)(void*);
using TickFn = void(__thiscall*)(void*, unsigned);
using ListingsFn = void(__thiscall*)(void*, const void*);
using FindFn = void*(__thiscall*)(void*, unsigned);
using TextFn = void(__thiscall*)(void*, const char*);
using SendFn = int(__thiscall*)(void*, int, void*, unsigned, unsigned);
using DispatchFn = void(__thiscall*)(void*, unsigned, const void*, unsigned, unsigned, void*, unsigned);
auto originalSelect = reinterpret_cast<SelectFn>(0x005cc390);
auto originalDecode = reinterpret_cast<DecodeFn>(0x004260a0);
auto originalTick = reinterpret_cast<TickFn>(0x005b3390);
auto originalListings = reinterpret_cast<ListingsFn>(0x005b3460);
auto findWindow = reinterpret_cast<FindFn>(0x00585580);
auto findControl = reinterpret_cast<FindFn>(0x0056ae30);
auto setText = reinterpret_cast<TextFn>(0x004f9610);
auto send = reinterpret_cast<SendFn>(0x00407f50);
auto dispatch = reinterpret_cast<DispatchFn>(0x005e9de0);
constexpr unsigned SearchType = 0x00c3ae84, ShopType = 0x00c38f2c;
constexpr ULONGLONG LookupTimeout = 15000, ShopTimeout = 10 * 60 * 1000;

template<class T> T Read(const void* p, size_t offset = 0) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(p) + offset, sizeof value);
    return value;
}
template<class T> void Write(void* p, size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<unsigned char*>(p) + offset, &value, sizeof value);
}
struct Identity {
    void* connection = nullptr;
    void* player = nullptr;
    unsigned character = 0;
    explicit operator bool() const { return connection && player && character; }
};
bool Same(Identity a, Identity b) {
    return a && b && a.connection == b.connection && a.player == b.player && a.character == b.character;
}
#ifdef KIOSK_SEARCH_TESTING
Identity testIdentity{};
void* testManager = nullptr;
void* testChat = nullptr;
unsigned testMetadataA = 0, testMetadataB = 0;
ULONGLONG testNow = 0;
#endif
ULONGLONG Now() noexcept {
#ifdef KIOSK_SEARCH_TESTING
    return testNow;
#else
    return GetTickCount64();
#endif
}
Identity Current() noexcept {
#ifdef KIOSK_SEARCH_TESTING
    return testIdentity;
#else
    __try {
        if (!*reinterpret_cast<const unsigned char*>(0x00d3d2cd)) return {};
        auto* player = *reinterpret_cast<void**>(0x00d3ffc0);
        if (!player) return {};
        return {*reinterpret_cast<void**>(0x00d3ab54), player, Read<unsigned>(player, 8)};
    } __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
#endif
}
void* Manager() noexcept {
#ifdef KIOSK_SEARCH_TESTING
    return testManager;
#else
    return *reinterpret_cast<void**>(0x00d4c5e0);
#endif
}
void* Chat() noexcept {
#ifdef KIOSK_SEARCH_TESTING
    return testChat;
#else
    return *reinterpret_cast<void**>(0x00d4dd4c);
#endif
}
struct NativeText {
    unsigned proxy = 0;
    const char* data;
    unsigned unused[3]{};
    unsigned length;
    unsigned capacity = 160;
};
static_assert(sizeof(NativeText) == 28);
void Notice(const char* text) {
    if (!Current()) return;
    auto* chat = Chat();
    if (!chat) return;
    const NativeText native{0, text, {}, static_cast<unsigned>(std::strlen(text))};
    dispatch(chat, 0x0d, &native, 0x19, 0, nullptr, 0);
}
int Send(Identity identity, unsigned opcode, void* body) {
    if (!Same(Current(), identity)) return -1;
#ifdef KIOSK_SEARCH_TESTING
    const unsigned a = testMetadataA, b = testMetadataB;
#else
    const unsigned a = *reinterpret_cast<unsigned*>(0x00d7b8e8);
    const unsigned b = *reinterpret_cast<unsigned*>(0x00d7b8ec);
#endif
    return send(identity.connection, opcode, body, a, b);
}
struct Pending {
    Identity identity{};
    char name[NameBytes]{};
    ULONGLONG started = 0;
};
struct OpenShop {
    Identity identity{};
    void* window = nullptr;
    int seller = 0;
    char name[NameBytes]{};
    ULONGLONG started = 0;
};
std::mutex mutex;
Pending pending{};
OpenShop opened{};

bool SelectedName(void* search, int visibleRow, char (&name)[NameBytes]) noexcept {
    __try {
        if (!search || Read<unsigned>(search) != SearchType || visibleRow < 0 || visibleRow >= 5) return false;
        auto* scroll = findControl(search, 0x24);
        if (!scroll) return false;
        const int first = Read<int>(scroll, 0x78), count = Read<int>(search, 0x130);
        if (first < 0 || first >= 20 || count < 1 || count > 20 || visibleRow >= count - first) return false;
        const auto* text = static_cast<unsigned char*>(search) + 0x188 + (first + visibleRow) * 28;
        const auto length = Read<unsigned>(text, 0x14), capacity = Read<unsigned>(text, 0x18);
        if (!length || length >= NameBytes || length > capacity) return false;
        const char* chars = capacity >= 16 ? Read<const char*>(text, 4) : reinterpret_cast<const char*>(text + 4);
        if (!chars) return false;
        std::memcpy(name, chars, length);
        name[length] = 0;
        // Embedded NULs would change the selected identity at the server.
        return std::strlen(name) == length && ValidName(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __fastcall Select(void* search, void*, int row) {
    LookupRequest request{};
    const auto identity = Current();
    if (!identity || !SelectedName(search, row, request.name)) return;
    request.magic = LookupMagic;
    {
        std::lock_guard guard(mutex);
        pending = {identity, {}, Now()};
        std::memcpy(pending.name, request.name, NameBytes);
    }
    // Replaces only the row's locator action. The stock search and paging stay native.
    if (Send(identity, LookupOpcode, &request) != 0) {
        { std::lock_guard guard(mutex); pending = {}; }
        Notice("[Kiosk] Could not open the shop. Please search again.");
    }
}

enum class ReplyKind { Native, Invalid, Remote };
ReplyKind ReadReply(void* reader, LookupReply& reply) noexcept {
    bool marked = false;
    __try {
        if (!reader) return ReplyKind::Native;
        const auto* data = Read<const unsigned char*>(reader, 4);
        const unsigned size = Read<unsigned>(reader, 0x0c), cursor = Read<unsigned>(reader, 0x10);
        if (!data || size > 65535 || cursor > size || size - cursor < 26) return ReplyKind::Native;
        if (Read<unsigned>(data + cursor, 22) != LookupMagic) return ReplyKind::Native;
        marked = true;
        Write(reader, 0x10, size);
        if (size - cursor != sizeof reply) return ReplyKind::Invalid;
        std::memcpy(&reply, data + cursor, sizeof reply);
        return ValidName(reply.name) ? ReplyKind::Remote : ReplyKind::Invalid;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return marked ? ReplyKind::Invalid : ReplyKind::Native; }
}
void BeginBrowse(const Pending& lookup, int seller) {
    auto* manager = Manager();
    auto* shop = manager ? findWindow(manager, 0x22) : nullptr;
    if (!shop || Read<unsigned>(shop) != ShopType || seller <= 0 || seller == static_cast<int>(lookup.identity.character)) {
        Notice("[Kiosk] This shop is unavailable.");
        return;
    }
    Write(shop, 0x1e4, seller);
    {
        std::lock_guard guard(mutex);
        opened = {lookup.identity, shop, seller, {}, Now()};
        std::memcpy(opened.name, lookup.name, NameBytes);
    }
    if (Send(lookup.identity, 0x1d0c, &seller) != 0) {
        { std::lock_guard guard(mutex); opened = {}; }
        Notice("[Kiosk] Could not load the shop. Please search again.");
    }
}
int __cdecl Decode(void* reader) {
    LookupReply reply{};
    const auto kind = ReadReply(reader, reply);
    if (kind == ReplyKind::Native) return originalDecode(reader);
    if (kind != ReplyKind::Remote) return 0;
    Pending lookup{};
    {
        std::lock_guard guard(mutex);
        if (!Same(pending.identity, Current()) || Now() - pending.started >= LookupTimeout ||
            std::strcmp(reply.name, pending.name)) return 0;
        lookup = pending;
        pending = {};
    }
    if (reply.result == Result::Disabled) Notice("[Kiosk] Remote shopping is disabled on this server.");
    else if (reply.result != Result::Found || reply.seller <= 0) Notice("[Kiosk] This shop has closed or is unavailable.");
    else BeginBrowse(lookup, reply.seller);
    return 0;
}
bool Remote(void* shop, OpenShop& state) {
    std::lock_guard guard(mutex);
    if (!Same(opened.identity, Current()) || Now() - opened.started >= ShopTimeout) { opened = {}; return false; }
    if (opened.window != shop || Read<int>(shop, 0x1e4) != opened.seller) return false;
    state = opened;
    return true;
}
void __fastcall Tick(void* shop, void*, unsigned now) {
    OpenShop state{};
    // This function only contains the native seller-presence/range auto-close.
    if (!Remote(shop, state)) originalTick(shop, now);
}
void __fastcall Listings(void* shop, void*, const void* body) {
    originalListings(shop, body);
    OpenShop state{};
    if (!Remote(shop, state)) return;
    if (auto* label = findControl(shop, 0x16); label && Read<unsigned>(label) == 0x00c2eb54)
        setText(label, state.name);
    if (auto* label = findControl(shop, 0x19); label && Read<unsigned>(label) == 0x00c2eb54)
        setText(label, "Remote Kiosk");
}
} // namespace

void Frame() {
    bool timeout = false;
    {
        std::lock_guard guard(mutex);
        const auto identity = Current();
        if (!Same(pending.identity, identity)) pending = {};
        else if (Now() - pending.started >= LookupTimeout) { pending = {}; timeout = true; }
        if (!Same(opened.identity, identity)) opened = {};
    }
    if (timeout) Notice("[Kiosk] Shop lookup timed out. Please search again.");
}
bool MatchesReference(const unsigned char* base) noexcept {
    struct Site { unsigned address; unsigned char bytes[16]; };
    constexpr Site sites[] = {
        {0x005cc390,{0x6a,0xff,0x68,0x00,0xbd,0xbe,0x00,0x64,0xa1,0,0,0,0,0x50,0x64,0x89}},
        {0x004260a0,{0x83,0xec,0x2c,0xa1,0x90,0x5e,0xd3,0,0x8b,0x4c,0x24,0x30,0x89,0x44,0x24,0x28}},
        {0x005b3390,{0x8b,0x81,0xe4,1,0,0,0x8b,0x0d,0x34,0x4a,0xd4,0,0x50,0xe8,0x3e,0x80}},
        {0x005b3460,{0x64,0xa1,0,0,0,0,0x6a,0xff,0x68,0x2b,0xac,0xbe,0,0x50,0x64,0x89}},
        {0x00585580,{0x8b,0x51,4,0x8b,2,0x3b,0xc2,0x56,0x74,0x18,0x8b,0x74,0x24,8,0x8b,0xff}},
        {0x0056ae30,{0x8b,0x91,0xd4,0,0,0,0x85,0xd2,0x74,0x13,0x8b,0x81,0xd8,0,0,0}},
        {0x004f9610,{0x56,0x8b,0xf1,0x8b,0x86,0x9c,0,0,0,0x85,0xc0,0x57,0x8b,0x7c,0x24,0x0c}},
    };
    __try {
        if (!base) return false;
        for (const auto& site : sites)
            if (std::memcmp(base + site.address - 0x400000, site.bytes, sizeof site.bytes)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
LONG Attach() {
    LONG result = DetourAttach(&reinterpret_cast<PVOID&>(originalSelect), reinterpret_cast<PVOID>(Select));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalDecode), reinterpret_cast<PVOID>(Decode));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalTick), reinterpret_cast<PVOID>(Tick));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalListings), reinterpret_cast<PVOID>(Listings));
    return result;
}
} // namespace KioskSearch
