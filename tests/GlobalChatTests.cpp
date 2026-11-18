#include "../src/Chats/GlobalChat.cpp"
#include "../../RohanMapServ_Hook/src/Chats/WorldChat.cpp"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using GlobalChatWire::Channel;
constexpr char Name[] = "TwentyCharacterName!";
constexpr unsigned Sender = 0x10203040;
constexpr unsigned MeleeColor = 0xFFECB186, RangedColor = 0xFF72B189;
static_assert(sizeof(Name) == 21);
int checks = 0, broadcasts = 0, replies = 0, nativeCalls = 0, draws = 0, inserts = 0;
bool muted = false, statusAvailable = true, allAvailable = true;
unsigned lastColor = 0, lastStyle = 0, lastNativeColor = 0, lastOutline = 0;
std::string line, logChannel;
bool linkTypesAvailable = false;
unsigned char linkTypes[3]{};
int published = 0;
std::size_t publishedLink = 0;
std::string publishedText;
struct Reader { unsigned unused; const unsigned char* body; unsigned unused2, size, cursor; };
struct Object {
    std::array<unsigned, 0x1A10 / sizeof(unsigned)> fields{};
    void Put(unsigned offset, unsigned value) { fields[offset / 4] = value; }
    void Put(unsigned offset, Object& child) {
        Put(offset, reinterpret_cast<unsigned>(child.fields.data()));
    }
    void* Data() { return fields.data(); }
};
Object connection, player, ui, manager, all, wrapper, list, status, melee, ranged;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
void* __fastcall FakeFindWindow(void* self, void*, unsigned id) {
    Check(self == ui.Data(), "lookup uses current native UI manager");
    if (id == GlobalChat::ChatManagerId) return manager.Data();
    return id == GlobalChat::StatusWindowId && statusAvailable ? status.Data() : nullptr;
}
void* __fastcall FakeFindAll(void* self, void*) {
    Check(self == manager.Data(), "only the chat manager is searched");
    return allAvailable ? all.Data() : nullptr;
}
void* __fastcall FindControl(void* self, void*, unsigned id) {
    Check(self == status.Data(), "colors come from Status");
    return id == 0x3E ? melee.Data() : id == 0x41 ? ranged.Data() : nullptr;
}
void __fastcall Insert(void* self, void*, const void* object, unsigned color,
    unsigned zero, unsigned mode, unsigned a, unsigned b, unsigned outline) {
    Check(self == list.Data() && !zero && mode == 15 && !a && !b, "only native All listbox receives plain rows");
    const auto& text = *static_cast<const GlobalChat::NativeText*>(object);
    Check(!text.proxy && text.length <= text.capacity && text.capacity >= 16 && !text.data[text.length], "native text ABI");
    line.assign(text.data, text.length);
    ++inserts;
    lastNativeColor = color;
    lastOutline = outline;
}
int __cdecl OriginalShout(void*) { ++nativeCalls; return 17; }
void __fastcall Text(void*, void*, unsigned, const void*, unsigned, unsigned color, unsigned style, unsigned outline, const char*) {
    Check(style == 2, "native font style preserved"); ++draws; lastColor = color; lastStyle = outline;
}
void __fastcall Rect(void*, void*, const char*, const void*, unsigned color, unsigned, unsigned outline, unsigned style, unsigned, unsigned) {
    Check(style == 2, "native rectangle font style preserved"); ++draws; lastColor = color; lastStyle = outline;
}
int __fastcall ChatBlocked(void* self, void*, void* target) {
    Check(self == connection.Data() && target == player.Data(), "native mute check receives server player");
    return muted ? 1 : 0;
}
int Receive(const unsigned char* body, unsigned size) {
    Reader reader{0, body, 0, size, 0};
    const int result = GlobalChat::DecodeShout(&reader);
    Check(reader.cursor == (result ? 0U : size), "marked packets consumed and unmarked cursor preserved");
    return result;
}
void __fastcall Reply(void* self, void*, int opcode, const void* body, int size) {
    Check(self == connection.Data() && opcode == 0x1F04, "usage response is private");
    Check(Receive(static_cast<const unsigned char*>(body), size) == 0, "private usage renders in All");
    ++replies;
}
bool Submit(std::string_view text, unsigned identity = Sender, bool item = false, void* source = connection.Data()) {
    unsigned char body[400]{};
    const auto n = static_cast<short>(text.size());
    std::memcpy(body + 4, &identity, 4);
    std::memcpy(body + 8, &n, 2);
    body[10] = item ? 1 : 0;
    std::memcpy(body + 15, text.data(), text.size());
    return WorldChat::TryHandleCommand(source, body, 15 + n + body[10]);
}
// A message carrying `blobSize` link bytes; the three type bytes the translator would have saved.
bool SubmitLinked(std::string_view text, std::size_t blobSize, unsigned char t0, unsigned char t1 = 0,
                  unsigned char t2 = 0, bool typesSaved = true, unsigned identity = Sender) {
    unsigned char body[400]{};
    const auto n = static_cast<short>(text.size());
    const auto links = static_cast<short>(blobSize);
    std::memcpy(body + 4, &identity, 4);
    std::memcpy(body + 8, &n, 2);
    std::memcpy(body + 10, &links, 2);
    std::memcpy(body + 15, text.data(), text.size());
    for (std::size_t i = 0; i < blobSize; ++i) body[15 + n + i] = static_cast<unsigned char>(0x40 + i % 7);
    linkTypesAvailable = typesSaved;
    linkTypes[0] = t0; linkTypes[1] = t1; linkTypes[2] = t2;
    const bool handled = WorldChat::TryHandleCommand(connection.Data(), body, 15 + n + blobSize);
    linkTypesAvailable = false;
    return handled;
}
}

namespace ChatCommon {
LPCSTR GetPlayerName(void* ptr) {
    Check(ptr == connection.Data(), "gets sender name from connection");
    return Name;
}
void LogChatConsole(const char* channel, WORD, void*, const unsigned char*, short) { logChannel = channel; }
void LogChatConsoleNamed(const char* channel, WORD, const char*, const unsigned char*, short) { logChannel = channel; }
bool TakeLinkTypes(int, unsigned char (&types)[3]) {
    if (!linkTypesAvailable) return false;
    std::memcpy(types, linkTypes, 3);
    return true;
}
}
namespace DiscordBridge {
void Publish(GlobalChatWire::Channel, std::string_view, std::string_view text, std::string_view link) {
    ++published;
    publishedLink = link.size();
    publishedText.assign(text);
}
}
namespace GmNotice {
void SendSystemBroadcast(void* self, unsigned short opcode, const void* body, int size) {
    Check(self == reinterpret_cast<void*>(Targets::Packet::SystemBroadcastManager) && opcode == 0x1F04,
          "uses native global player broadcaster with Shout envelope");
    const auto* wire = static_cast<const unsigned char*>(body);
    unsigned id = 0;
    std::memcpy(&id, wire, sizeof id);
    Check(id == Sender, "outbound character ID is server owned");
    Check(Receive(wire, size) == 0, "actual client consumes actual server body");
    ++broadcasts;
}
}

int main() {
    using namespace GlobalChat;
    using namespace GlobalChatWire;
    player.Put(0x30, Sender);
    connection.Put(0x534, player);
    manager.Put(0, ChatManagerType);
    manager.Put(0xF0, all);
    all.Put(0, ChatWindowType);
    all.Put(0x104, wrapper);
    wrapper.Put(4, list);
    list.Put(0, ListBoxType);
    status.Put(0, StatusWindowType);
    melee.Put(0, TextBoxType);
    ranged.Put(0, TextBoxType);
    melee.Put(0x78, MeleeColor);
    ranged.Put(0x78, RangedColor);
    testUiManager = ui.Data();
    findWindow = reinterpret_cast<LookupFn>(FakeFindWindow);
    findControl = reinterpret_cast<LookupFn>(FindControl);
    findAll = reinterpret_cast<AllFn>(FakeFindAll);
    insert = reinterpret_cast<InsertFn>(Insert);
    originalShout = OriginalShout;
    originalText = reinterpret_cast<FontTextFn>(Text);
    originalRect = reinterpret_cast<FontRectFn>(Rect);
    WorldChat::nativeChatBlocked = reinterpret_cast<WorldChat::ChatBlockedFn>(ChatBlocked);
    WorldChat::send = reinterpret_cast<WorldChat::SendFn>(Reply);

    Check(Submit(".WoRlD\t  hello  ") && broadcasts == 1 && logChannel == "World", "World command broadcasts once");
    Check(line == "[World] TwentyCharacterName!: hello", "keeps complete maximum-length name");
    Check(lastNativeColor == WorldFallback && lastOutline == WorldOutline, "World row carries its own style marker");
    testCaller = 0x4F3655;
    DrawText(nullptr, nullptr, 0, nullptr, 1, WorldFallbackColor, 2, lastOutline, "wrapped remainder");
    Check(lastColor == MeleeColor && lastStyle == 2, "real Melee label ARGB reaches normal font path");

    const std::string longText(153, 'x');
    Check(Submit(".trade " + longText) && broadcasts == 2 && logChannel == "Trade", "Trade broadcasts largest command");
    Check(line == "[Trade] TwentyCharacterName!: " + longText, "text is preserved through longest accepted command");
    Check(lastNativeColor == TradeFallback && lastOutline == TradeOutline, "Trade row carries its own style marker");
    testCaller = 0x4F37B7;
    DrawRect(nullptr, nullptr, "wrapped remainder", nullptr, TradeFallbackColor, 1, lastOutline, 2, 0, 0);
    Check(lastColor == RangedColor && lastStyle == 2, "real Ranged label ARGB reaches rectangle font path");
    statusAvailable = false;
    DrawRect(nullptr, nullptr, "trade", nullptr, TradeFallbackColor, 1, lastOutline, 2, 0, 0);
    Check(lastColor == TradeFallbackColor && lastStyle == 2, "missing Status resource keeps verified visible color");
    statusAvailable = true;
    testCaller = 0x4F87FD;
    DrawRect(nullptr, nullptr, "other UI", nullptr, TradeFallbackColor, 1, lastOutline, 2, 0, 0);
    Check(lastStyle == TradeOutline && lastColor == TradeFallbackColor, "other font call sites are untouched");
    testCaller = 0x4F37B7;
    DrawRect(nullptr, nullptr, "ordinary notice", nullptr, 0xFF4080FF, 1, 2, 2, 0, 0);
    Check(lastColor == 0xFF4080FF && lastStyle == 2, "ordinary Notice color preserved");

    Check(!Submit(".worldwide hello") && !Submit("hello"), "unrelated Say proceeds normally");
    Check(Submit(".world") && replies == 1 && line.find("Usage: .world") != std::string::npos, "bare command gets private help");
    Check(Submit(".trade hi", Sender, true) && replies == 2 && broadcasts == 2, "item links are not broadcast");
    Check(Submit(std::string_view(".trade hi\0there", 15)) && replies == 3 && broadcasts == 2, "embedded NUL rejected");
    Check(Submit(".trade forged", 0x1234) && broadcasts == 2, "forged sender is consumed");
    Check(Submit(".world unavailable", Sender, false, nullptr) && broadcasts == 2, "missing player is consumed");
    muted = true;
    Check(Submit(".world muted") && broadcasts == 2, "native mute blocks World");
    muted = false;
    player.Put(0x1980, 0x40);
    Check(Submit(".trade blocked") && broadcasts == 2, "native Say state blocks Trade");
    player.Put(0x1980, 0);

    all.Put(0x144, 1);
    const int before = inserts;
    Check(Submit(".trade party") && broadcasts == 3 && inserts == before, "never falls back to another tab");
    all.Put(0x144, 0);
    allAvailable = false;
    Check(Submit(".world loading") && broadcasts == 4 && inserts == before, "safe during chat window initialization");

    unsigned char wire[HeaderSize + MaxText]{};
    const auto length = Encode(Channel::Trade, Sender, Name, "hello", wire, sizeof wire);
    Check(length && Receive(wire, static_cast<unsigned>(length)) == 0, "valid marked message is consumed");
    wire[30] = 3;
    Check(Receive(wire, static_cast<unsigned>(length)) == 0, "unknown marked version is consumed");
    Check(Receive(wire, 30) == 0, "truncated marked header is consumed");
    wire[26] = 0;
    Check(Receive(wire, static_cast<unsigned>(length)) == 17 && nativeCalls == 1, "ordinary Shout stays on the native decoder");
    Check(draws == 5, "five font calls forwarded once each");

    // ---- Item links in World/Trade (stage 1: carried and shown as text) ----
    allAvailable = true;
    const int sentBefore = broadcasts, insertsBefore = inserts, publishedBefore = published;
    Check(SubmitLinked(".trade WTS [Sprint Scroll(Superior)]", 66, 1) && broadcasts == sentBefore + 1 &&
          inserts == insertsBefore + 1, "a valid item link is broadcast and shown");
    Check(line == "[Trade] TwentyCharacterName!: WTS [Sprint Scroll(Superior)]", "link text is displayed unchanged");
    Check(published == publishedBefore + 1 && publishedLink == 66 && publishedText == "WTS [Sprint Scroll(Superior)]",
          "Discord receives the text and the 66-byte blob");
    Check(SubmitLinked(".world [A][B][C]", 188, 1, 1, 1) && broadcasts == sentBefore + 2, "three item links fit");
    const int repliesBefore = replies;
    Check(SubmitLinked(".world [x]", 66, 1, 0, 0, false) && replies == repliesBefore + 1 &&
          broadcasts == sentBefore + 2, "link with no saved type bytes is refused");
    Check(SubmitLinked(".world [x]", 66, 2) && replies == repliesBefore + 2 && broadcasts == sentBefore + 2,
          "non-item link type is refused");
    Check(SubmitLinked(".world [x]", 66, 0) && replies == repliesBefore + 3 && broadcasts == sentBefore + 2,
          "link with no item entry is refused");
    Check(SubmitLinked(".world [x]", 40, 1) && replies == repliesBefore + 4 && broadcasts == sentBefore + 2,
          "too-short link blob is refused");
    Check(SubmitLinked(".world [x]", 201, 1) && replies == repliesBefore + 5 && broadcasts == sentBefore + 2,
          "oversized link blob is refused");
    Check(SubmitLinked(".world [x]", 66, 1, 0, 0, true, 0x1234) && broadcasts == sentBefore + 2 &&
          replies == repliesBefore + 5, "forged sender with a link is consumed");
    Check(published == publishedBefore + 2, "refused link messages are not sent to Discord");
    Check(Submit(".trade plain") && published == publishedBefore + 3 && publishedLink == 0, "plain messages publish no blob");

    std::ifstream reference("00_reference/rohanclient_unpacked.exe", std::ios::binary);
    std::vector<unsigned char> image(std::istreambuf_iterator<char>(reference), {});
    Check(image.size() > 0x830900 && MatchesReference(image.data()), "native fingerprints match the supplied client");
    image[FindAllAddress - 0x400000] ^= 1;
    Check(!MatchesReference(image.data()), "unexpected native All lookup stops activation");
    std::printf("World/Trade server-client checks passed (%d)\n", checks);
}
