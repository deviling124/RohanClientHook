#define KIOSK_SEARCH_TESTING
#include "../src/KioskSearch.cpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace KioskSearch;
using namespace KioskSearchProtocol;
int failures = 0, nativeDecodes = 0, nativeTicks = 0, nativeListings = 0, nativeSelections = 0;
int sendResult = 0;
unsigned lastTick = 0;
void* lastTickShop = nullptr;
const void* lastListingsBody = nullptr;
std::array<unsigned char, 0x500> search{};
std::array<unsigned char, 0x100> scroll{};
std::array<unsigned char, 0x300> shop{}, otherShop{};
std::array<unsigned char, 32> manager{}, chat{}, player{}, connection{}, ownerLabel{}, titleLabel{};
std::array<std::array<char, NameBytes>, 20> heapNames{};
bool haveScroll = true, haveShop = true;
std::string notice, ownerText, titleText;
struct Sent { int opcode; void* connection; unsigned a, b; std::vector<unsigned char> body; };
std::vector<Sent> sent;

void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void __fastcall FakeSelect(void*, void*, int) { ++nativeSelections; }
int __cdecl FakeDecode(void* reader) {
    ++nativeDecodes;
    Check(Read<unsigned>(reader, 0x10) == 0, "unmarked reply preserves the native reader cursor");
    return 71;
}
void __fastcall FakeTick(void* self, void*, unsigned now) {
    ++nativeTicks; lastTickShop = self; lastTick = now;
}
void __fastcall FakeListings(void* self, void*, const void* body) {
    ++nativeListings; lastListingsBody = body;
    Check(self == shop.data() || self == otherShop.data(), "listing builder retains its native owner");
}
void* __fastcall FakeWindow(void* self, void*, unsigned id) {
    Check(self == manager.data() && id == 0x22, "lookup resolves the existing shop window");
    return haveShop ? shop.data() : nullptr;
}
void* __fastcall FakeControl(void* self, void*, unsigned id) {
    if (self == search.data() && id == 0x24) return haveScroll ? scroll.data() : nullptr;
    if (self == shop.data() && id == 0x16) return ownerLabel.data();
    if (self == shop.data() && id == 0x19) return titleLabel.data();
    return nullptr;
}
void __fastcall FakeText(void* self, void*, const char* text) {
    Check(nativeListings > 0, "remote labels update after the original listing builder");
    if (self == ownerLabel.data()) ownerText = text;
    else if (self == titleLabel.data()) titleText = text;
    else Check(false, "only native shop text controls are updated");
}
int __fastcall FakeSend(void* self, void*, int opcode, void* body, unsigned a, unsigned b) {
    const size_t bytes = opcode == LookupOpcode ? sizeof(LookupRequest) : sizeof(int);
    const auto* data = static_cast<const unsigned char*>(body);
    sent.push_back({opcode, self, a, b, {data, data + bytes}});
    return sendResult;
}
void __fastcall FakeDispatch(void* self, void*, unsigned mode, const void* text, unsigned color,
    unsigned zero, void* links, unsigned count) {
    Check(self == chat.data() && mode == 0x0d && color == 0x19 && !zero && !links && !count,
        "status uses native chat dispatch without item links");
    const auto& native = *static_cast<const NativeText*>(text);
    notice.assign(native.data, native.length);
}
void Reset() {
    search.fill(0); scroll.fill(0); shop.fill(0); otherShop.fill(0);
    Write(search.data(), 0, SearchType); Write(shop.data(), 0, ShopType); Write(otherShop.data(), 0, ShopType);
    Write(ownerLabel.data(), 0, 0x00c2eb54u); Write(titleLabel.data(), 0, 0x00c2eb54u);
    Write(search.data(), 0x130, 1); Write(scroll.data(), 0x78, 0);
    sent.clear(); notice.clear(); ownerText.clear(); titleText.clear();
    nativeDecodes = nativeTicks = nativeListings = nativeSelections = 0;
    lastTick = 0; lastTickShop = nullptr; lastListingsBody = nullptr;
    haveScroll = haveShop = true; sendResult = 0;
    pending = {}; opened = {};
    testIdentity = {connection.data(), player.data(), 100};
    testManager = manager.data(); testChat = chat.data();
    testMetadataA = 0x12345678; testMetadataB = 0x87654321; testNow = 1000;
}
void Name(int row, const char* value, size_t length = SIZE_MAX) {
    if (length == SIZE_MAX) length = std::strlen(value);
    auto* object = search.data() + 0x188 + row * 28;
    std::memset(object, 0, 28);
    const unsigned capacity = length >= 16 ? 31 : 15;
    Write(object, 0x14, static_cast<unsigned>(length)); Write(object, 0x18, capacity);
    if (length < 16) std::memcpy(object + 4, value, length);
    else {
        std::memcpy(heapNames[row].data(), value, (std::min)(length, size_t(NameBytes)));
        const char* ptr = heapNames[row].data(); Write(object, 4, ptr);
    }
}
LookupReply Reply(const char* name = "Seller", int seller = 200, Result result = Result::Found) {
    LookupReply reply{}; reply.result = result; reply.magic = LookupMagic; reply.seller = seller;
    strcpy_s(reply.name, name); return reply;
}
struct Reader {
    unsigned proxy = 0;
    const unsigned char* data = nullptr;
    unsigned unused = 0;
    unsigned size = 0, cursor = 0;
};
static_assert(offsetof(Reader, data) == 4 && offsetof(Reader, size) == 0x0c && offsetof(Reader, cursor) == 0x10);
unsigned DecodeBytes(const void* body, unsigned size, unsigned cursor = 0) {
    Reader reader{0, static_cast<const unsigned char*>(body), 0, size, cursor};
    Decode(&reader); return reader.cursor;
}
void Request(const char* name = "Seller") { Name(0, name); Select(search.data(), nullptr, 0); }
void Open() { Request(); const auto reply = Reply(); DecodeBytes(&reply, sizeof reply); }

void TestSelection() {
    Reset(); Write(search.data(), 0x130, 8); Write(scroll.data(), 0x78, 3);
    Name(0, "Wrong"); Name(5, "ScrolledSeller"); Select(search.data(), nullptr, 2);
    Check(sent.size() == 1 && nativeSelections == 0, "search result replaces the locator with one remote lookup");
    if (sent.size() == 1) {
        const auto& packet = sent[0];
        const auto& request = *reinterpret_cast<const LookupRequest*>(packet.body.data());
        Check(packet.opcode == LookupOpcode && packet.body.size() == 25 && request.magic == LookupMagic &&
            !std::strcmp(request.name, "ScrolledSeller"), "scrolled visible row submits the selected seller name and marker");
        Check(packet.connection == connection.data() && packet.a == testMetadataA && packet.b == testMetadataB,
            "lookup preserves current connection metadata");
    }
    Reset(); Name(0, "ABCDEFGHIJKLMNOPQRST"); Select(search.data(), nullptr, 0);
    Check(sent.size() == 1 && !std::strcmp(pending.name, "ABCDEFGHIJKLMNOPQRST"), "20-byte heap-backed seller names are accepted");
    for (const int invalidRow : {-1, 5, 100}) {
        Reset(); Name(0, "Seller"); Select(search.data(), nullptr, invalidRow);
        Check(sent.empty(), "invalid visible rows send no lookup");
    }
    Reset(); Name(0, "Seller"); Write(search.data(), 0x130, 0); Select(search.data(), nullptr, 0);
    Check(sent.empty(), "empty result list sends no lookup");
    Reset(); Name(0, "Seller"); Write(scroll.data(), 0x78, 20); Select(search.data(), nullptr, 0);
    Check(sent.empty(), "out-of-range scroll offset sends no lookup");
    Reset(); Name(0, "Seller"); haveScroll = false; Select(search.data(), nullptr, 0);
    Check(sent.empty(), "missing native scroll control sends no lookup");
    Reset(); Name(0, "Bad\nName"); Select(search.data(), nullptr, 0);
    Check(sent.empty(), "control characters in seller names are rejected");
    Reset(); Name(0, "Bad\0Name", 8); Select(search.data(), nullptr, 0);
    Check(sent.empty(), "embedded NUL does not change selected seller identity");
    Reset(); Name(0, "Seller"); Write(search.data() + 0x188, 0x18, 3u); Select(search.data(), nullptr, 0);
    Check(sent.empty(), "invalid native string length/capacity is rejected");
    Reset(); Name(0, "Seller"); testIdentity = {}; Select(search.data(), nullptr, 0);
    Check(sent.empty(), "disconnected client cannot request remote shopping");
    Reset(); Name(0, "Seller"); sendResult = 1; Select(search.data(), nullptr, 0);
    Check(!pending.identity && notice.find("Could not open") != std::string::npos,
        "lookup send failure clears pending request and reports the failure");
}
void TestReplies() {
    Reset(); Open();
    Check(sent.size() == 2 && sent[1].opcode == 0x1d0c && Read<int>(sent[1].body.data()) == 200,
        "resolved seller triggers the native browse request");
    Check(Read<int>(shop.data(), 0x1e4) == 200 && opened.window == shop.data() && opened.seller == 200,
        "native shop owner and scoped remote context are set before browsing");
    Check(!pending.identity && nativeDecodes == 0, "remote lookup is consumed without native path rendering");
    auto reply = Reply(); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 2, "duplicate reply cannot open another shop");

    Reset(); reply = Reply(); reply.magic = 0; Reader reader{0, reinterpret_cast<unsigned char*>(&reply), 0, sizeof reply, 0};
    Check(Decode(&reader) == 71 && nativeDecodes == 1 && reader.cursor == 0,
        "ordinary location replies keep the original decoder and reader intact");
    Reset(); Request(); reply = Reply();
    Check(DecodeBytes(&reply, 38) == 38 && sent.size() == 1 && nativeDecodes == 0,
        "marked reply without seller extension is consumed without opening or following a path");
    Reset(); Request(); reply = Reply(); std::memset(reply.name, 'x', NameBytes);
    Check(DecodeBytes(&reply, sizeof reply) == sizeof reply && sent.size() == 1,
        "unterminated marked reply name is consumed and rejected");
    Reset(); Request(); reply = Reply("Different"); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 1 && pending.identity, "reply for another seller leaves the current request pending");
    Reset(); Request(); testNow += LookupTimeout; reply = Reply(); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 1 && !opened.identity, "expired reply cannot grant remote shop state");
    Reset(); Request(); ++testIdentity.character; reply = Reply(); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 1 && !opened.identity, "reply from the previous character is rejected");
    Reset(); Request(); testIdentity.connection = reinterpret_cast<void*>(0x9876); reply = Reply(); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 1 && !opened.identity, "reply from a prior connection is rejected");
    for (const Result result : {Result::Disabled, Result::Unavailable}) {
        Reset(); Request(); reply = Reply("Seller", 0, result); DecodeBytes(&reply, sizeof reply);
        Check(sent.size() == 1 && !pending.identity && !notice.empty(), "disabled or closed shops report failure without a browse");
    }
    for (int seller : {0, -1, 100}) {
        Reset(); Request(); reply = Reply("Seller", seller); DecodeBytes(&reply, sizeof reply);
        Check(sent.size() == 1 && !opened.identity, "invalid or own seller ID never starts browsing");
    }
    Reset(); Request(); haveShop = false; reply = Reply(); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 1 && !opened.identity && !notice.empty(), "missing native shop window reports unavailable");
    Reset(); Request(); sendResult = 1; reply = Reply(); DecodeBytes(&reply, sizeof reply);
    Check(sent.size() == 2 && !opened.identity && notice.find("Could not load") != std::string::npos,
        "browse send failure revokes the client remote context");
    Reset(); Request(); reply = Reply(); std::array<unsigned char, sizeof reply + 3> prefixed{};
    std::memcpy(prefixed.data() + 3, &reply, sizeof reply);
    Check(DecodeBytes(prefixed.data(), static_cast<unsigned>(prefixed.size()), 3) == prefixed.size() && sent.size() == 2,
        "tagged decoder reads from the reader cursor rather than buffer start");
}
void TestScopeAndTimeout() {
    Reset(); Open(); Tick(shop.data(), nullptr, 1234);
    Check(nativeTicks == 0, "remote shop skips only its proximity auto-close");
    Tick(otherShop.data(), nullptr, 4321);
    Check(nativeTicks == 1 && lastTickShop == otherShop.data() && lastTick == 4321,
        "ordinary shop ticks preserve the owner and original argument");
    Write(shop.data(), 0x1e4, 201); Tick(shop.data(), nullptr, 77);
    Check(nativeTicks == 2, "a different seller retains native proximity handling");
    Reset(); Open(); testNow += ShopTimeout; Tick(shop.data(), nullptr, 9);
    Check(nativeTicks == 1 && !opened.identity, "remote shop permission expires after ten minutes");
    Reset(); Open(); ++testIdentity.character; Tick(shop.data(), nullptr, 10);
    Check(nativeTicks == 1 && !opened.identity, "character change restores normal shop proximity checks");
    Reset(); Open(); unsigned body = 0;
    Listings(shop.data(), nullptr, &body);
    Check(nativeListings == 1 && lastListingsBody == &body && ownerText == "Seller" && titleText == "Remote Kiosk",
        "remote list preserves native item construction and sets seller/title labels");
    ownerText.clear(); titleText.clear(); Listings(otherShop.data(), nullptr, &body);
    Check(nativeListings == 2 && ownerText.empty() && titleText.empty(), "ordinary listings retain native labels");
    Reset(); Request(); testNow += LookupTimeout; Frame();
    Check(!pending.identity && notice.find("timed out") != std::string::npos, "pending lookup timeout is reported once");
    notice.clear(); Frame(); Check(notice.empty(), "expired request does not repeat its timeout notice");
    Reset(); Open(); Request(); testIdentity = {}; Frame();
    Check(!pending.identity && !opened.identity && notice.empty(), "disconnect drops pending and open shop state silently");
}
void TestFingerprint() {
    std::ifstream file("00_reference/rohanclient_unpacked.exe", std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(bytes.size() > 0x1000, "reference client is available for fingerprint validation");
    if (bytes.size() <= 0x1000) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (section[i].PointerToRawData + section[i].SizeOfRawData <= bytes.size() &&
            section[i].VirtualAddress + section[i].SizeOfRawData <= image.size())
            std::memcpy(image.data() + section[i].VirtualAddress, bytes.data() + section[i].PointerToRawData, section[i].SizeOfRawData);
    }
    Check(MatchesReference(image.data()), "all kiosk hook sites match the supplied reference client");
    for (unsigned address : {0x5cc390u, 0x4260a0u, 0x5b3390u, 0x5b3460u, 0x585580u, 0x56ae30u, 0x4f9610u}) {
        image[address - 0x400000] ^= 1;
        Check(!MatchesReference(image.data()), "each changed native hook/helper fingerprint is rejected");
        image[address - 0x400000] ^= 1;
    }
}
}
int main() {
    originalSelect = reinterpret_cast<SelectFn>(FakeSelect);
    originalDecode = FakeDecode;
    originalTick = reinterpret_cast<TickFn>(FakeTick);
    originalListings = reinterpret_cast<ListingsFn>(FakeListings);
    findWindow = reinterpret_cast<FindFn>(FakeWindow);
    findControl = reinterpret_cast<FindFn>(FakeControl);
    setText = reinterpret_cast<TextFn>(FakeText);
    KioskSearch::send = reinterpret_cast<SendFn>(FakeSend);
    dispatch = reinterpret_cast<DispatchFn>(FakeDispatch);
    TestSelection(); TestReplies(); TestScopeAndTimeout(); TestFingerprint();
    std::printf("Kiosk search client tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
