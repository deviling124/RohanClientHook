#include "../src/CustomEvents/EventNotice.cpp"
#include "../src/CustomEvents/MonsterName.h"
#include "../../RohanMapServ_Hook/src/CustomEvents/MapTHAdapter.h"
#include "../../RohanMapServ_Hook/src/CustomEvents/BossTracker.h"
#include "../../RohanMapServ_Hook/src/CustomEvents/Belzac.h"
#include "../../RohanMapServ_Hook/src/CustomEvents/WorldBoss.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace CustomEvents { void ReadTestClock(SYSTEMTIME& time) { time = {}; } }
namespace {
int checks = 0, displayed = 0, forwarded = 0, broadcast = 0;
unsigned color = 0, category = 0;
std::string rendered;
std::vector<unsigned char> wire;
void* forwardedReader = nullptr;
void Check(bool ok, const char* description) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
struct Reader {
    unsigned unused = 0;
    const unsigned char* data = nullptr;
    unsigned unused2 = 0, size = 0, cursor = 0;
};
static_assert(offsetof(Reader, size) == 0x0C && offsetof(Reader, cursor) == 0x10);
int __cdecl StockDecoder(void* reader) { ++forwarded; forwardedReader = reader; return 73; }
void __fastcall Display(void* self, void*, unsigned mode, const void* text, unsigned palette,
                        unsigned a, void* links, unsigned count) {
    ++displayed; category = mode; color = palette;
    Check(self == reinterpret_cast<void*>(0x12345678) && !a && !links && !count,
          "native receiver and all six dispatch argument slots preserved");
    const auto* native = static_cast<const EventNotice::NativeText*>(text);
    Check(native->proxy == 0 && native->capacity >= native->length && native->capacity >= 16 && native->length <= 160,
          "borrowed string follows reference ABI and length bounds");
    rendered.assign(native->data, native->length);
    Check(native->data[native->length] == 0, "native text is terminated for chat list copies");
}
int Receive(std::vector<unsigned char> body, unsigned cursor = 0) {
    Reader reader{}; reader.data = body.data(); reader.size = static_cast<unsigned>(body.size()); reader.cursor = cursor;
    const auto saved = body;
    const int status = EventNotice::DecodeNotice(&reader);
    Check(body == saved, "client never mutates incoming wire bytes");
    return status;
}
std::vector<unsigned char> OverridePacket(std::uint32_t id, const char* name) {
    std::vector<unsigned char> out(EventNoticeWire::HeaderSize + EventNoticeWire::MaxText);
    std::size_t written = 0;
    Check(EventNoticeWire::EncodeNameOverride(id, name, out.data(), out.size(), written), "override packet encodes");
    out.resize(written);
    return out;
}
void SetString(std::vector<unsigned char>& script, unsigned offset, const char* text, std::vector<char>* heap) {
    const unsigned length = static_cast<unsigned>(std::strlen(text));
    *reinterpret_cast<unsigned*>(&script[offset + 0x14]) = length;
    if (heap) {
        heap->assign(64, 0); std::memcpy(heap->data(), text, length);
        *reinterpret_cast<char**>(&script[offset + 4]) = heap->data();
        *reinterpret_cast<unsigned*>(&script[offset + 0x18]) = 63;
    } else {
        std::memcpy(&script[offset + 4], text, length + 1);
        *reinterpret_cast<unsigned*>(&script[offset + 0x18]) = 15;
    }
}
std::string ReadString(const std::vector<unsigned char>& script, unsigned offset) {
    const unsigned length = *reinterpret_cast<const unsigned*>(&script[offset + 0x14]);
    const unsigned capacity = *reinterpret_cast<const unsigned*>(&script[offset + 0x18]);
    const char* chars = capacity < 16 ? reinterpret_cast<const char*>(&script[offset + 4])
                                      : *reinterpret_cast<char* const*>(&script[offset + 4]);
    return std::string(chars, length);
}
void TestMonsterNames() {
    std::vector<unsigned char> inlineScript(0x320), heapScript(0x320);
    std::vector<char> heapName;
    SetString(inlineScript, 0x08, "Apir1", nullptr);
    SetString(inlineScript, 0x24, "Apir Reaver", nullptr);
    SetString(heapScript, 0x08, "Giant spearspider1", nullptr);
    SetString(heapScript, 0x24, "Giant Spearspider", &heapName);
    MonsterName::TestRegister(262212, inlineScript.data());
    MonsterName::TestRegister(262214, heapScript.data());
    const int shown = displayed, passed = forwarded;
    Check(Receive(OverridePacket(262212, "PAIR RARVEE")) == 0, "override packet is consumed, not forwarded");
    Check(ReadString(inlineScript, 0x24) == "PAIR RARVEE", "display name replaced in place (inline string)");
    Check(ReadString(inlineScript, 0x08) == "Apir1", "string of a different length is left untouched");
    Receive(OverridePacket(262214, "GNAIT PSEARDIRESP"));
    Check(ReadString(heapScript, 0x24) == "GNAIT PSEARDIRESP", "display name replaced in place (heap string)");
    Receive(OverridePacket(262212, "TOOLONGFORTHISNAME"));
    Check(ReadString(inlineScript, 0x24) == "PAIR RARVEE", "different length is ignored");
    Receive(OverridePacket(999999, "ABC"));
    Check(displayed == shown && forwarded == passed, "unknown monster ids change nothing and show nothing");
    Receive(OverridePacket(262212, "="));
    Check(ReadString(inlineScript, 0x24) == "Apir Reaver" && ReadString(inlineScript, 0x08) == "Apir1", "equals sign restores the real name");
    Receive(OverridePacket(262214, "="));
    Check(ReadString(heapScript, 0x24) == "Giant Spearspider", "heap name restored");
    Receive(OverridePacket(262212, "="));
    Check(ReadString(inlineScript, 0x24) == "Apir Reaver", "restoring twice is harmless");
    auto bad = OverridePacket(262212, "PAIR RARVEE");
    bad.pop_back();
    Receive(bad);
    Check(ReadString(inlineScript, 0x24) == "Apir Reaver" && displayed == shown, "truncated override is rejected without effect");
    Check(Receive(OverridePacket(262212, "PAIR RARVEE")) == 0, "override accepted again after restore");
    MonsterName::TestExpire();
    Receive(OverridePacket(262212, "="));
}
void TestNativeImage(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE mapping = file == INVALID_HANDLE_VALUE ? nullptr : CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    auto* raw = mapping ? static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    Check(raw != nullptr, "map reference client as inert file data");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw + dos->e_lfanew);
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(image != nullptr, "materialize inert client image without invoking its loader or code");
    std::memcpy(image, raw, nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        std::memcpy(image + sections[i].VirtualAddress, raw + sections[i].PointerToRawData, sections[i].SizeOfRawData);
    Check(EventNotice::MatchesReference(image), "decoder, dispatcher and both native green/Notice blue entries match reference");
    for (const unsigned address : {EventNotice::DecoderAddress, EventNotice::DispatchAddress}) {
        auto* byte = image + address - 0x400000;
        *byte ^= 1;
        Check(!EventNotice::MatchesReference(image), "wrong native function fingerprint blocks installation");
        *byte ^= 1;
    }
    for (const unsigned index : {EventNotice::EventColorIndex, 0x22U}) {
        auto* byte = image + EventNotice::PaletteAddress - 0x400000 + index * 4;
        *byte ^= 1;
        Check(!EventNotice::MatchesReference(image), "mismatched native palette entry blocks startup");
        *byte ^= 1;
    }
    Check(EventNotice::EventColorIndex == 0x19 && EventNotice::EventColor == 0xFF42C762,
          "events select the existing native green rather than an appended index");
    const auto* palette = reinterpret_cast<const unsigned*>(image + EventNotice::PaletteAddress - 0x400000);
    Check(palette[EventNotice::EventColorIndex] == 0xFF42C762 && palette[0x22] == 0xFF4080FF,
          "native event green and Notice blue resolve to expected opaque ARGB values");
    Check(EventNotice::MatchesReference(image), "validation leaves native palette and instructions intact");
    Check(MonsterName::MatchesReference(image), "monster script table insert fingerprint matches the reference client");
    image[0xAD4B90 - 0x400000 + 3] ^= 1;
    Check(!MonsterName::MatchesReference(image), "changed monster table insert disables the name hook");
    image[0xAD4B90 - 0x400000 + 3] ^= 1;
    VirtualFree(image, 0, MEM_RELEASE); UnmapViewOfFile(raw); CloseHandle(mapping); CloseHandle(file);
}
}
namespace GmNotice {
void SendSystemBroadcast(void*, unsigned short opcode, const void* body, int bytes) {
    ++broadcast;
    Check(opcode == 0x1F0A, "server event uses existing registered notice envelope");
    wire.assign(static_cast<const unsigned char*>(body), static_cast<const unsigned char*>(body) + bytes);
    Reader reader{}; reader.data = wire.data(); reader.size = static_cast<unsigned>(wire.size());
    Check(EventNotice::DecodeNotice(&reader) == 0 && reader.cursor == reader.size,
          "real server event codec is consumed completely by real client decoder");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    using namespace EventNoticeWire;
    EventNotice::originalDecoder = StockDecoder;
    EventNotice::dispatch = reinterpret_cast<EventNotice::DispatchFn>(Display);
    EventNotice::testManager = reinterpret_cast<void*>(0x12345678);
    CustomEvents::MapTH::Announce(CustomEvents::Belzac.announcement);
    Check(displayed == 1 && !forwarded && rendered == "[Event]: Belzac was summoned in front of Ehres Harbor" &&
          category == 0x0D && color == EventNotice::EventColorIndex, "end-to-end Belzac summon has event prefix and custom color only");
    CustomEvents::MapTH::Announce(CustomEvents::WorldBoss.announcement);
    Check(rendered == "[Event]: World Boss was summoned", "World Boss uses same dedicated presentation");
    char kill[161]{};
    Check(CustomEvents::FormatKillAnnouncement("Seijuro", "Belzac", kill, sizeof kill), "killer text formats successfully");
    CustomEvents::MapTH::Announce(kill);
    Check(rendered == "[Event]: [Seijuro] killed Belzac. Congratulations.", "kill notice uses same event renderer without Notice prefix");
    const auto valid = wire;
    const int count = displayed;
    for (std::size_t size = 0; size < valid.size(); ++size) {
        Receive(std::vector<unsigned char>(valid.begin(), valid.begin() + size));
        Check(displayed == count, "all truncated prefixes and payloads avoid event display");
    }
    for (const unsigned value : {0U, 161U, 32767U, 65535U}) {
        auto body = valid; const auto length = static_cast<unsigned short>(value);
        std::memcpy(body.data() + 1, &length, 2); Receive(body);
        Check(displayed == count, "empty oversized signed-negative and inconsistent lengths are rejected");
    }
    auto body = valid; body[9] = 2; Receive(body);
    body = valid; body[0] = 1; Receive(body);
    body = valid; body.push_back(0); Receive(body);
    body = valid; body.back() = '\n'; Receive(body);
    body = valid; body.back() = 0; Receive(body);
    body = valid; body[HeaderSize] = 'X'; Receive(body);
    Check(displayed == count, "unknown versions modes trailing data controls NULs and missing event prefix are rejected");
    const int prior = forwarded;
    body = valid; std::memset(body.data() + 3, 0, sizeof Marker);
    Reader normal{}; normal.data = body.data(); normal.size = static_cast<unsigned>(body.size());
    Check(EventNotice::DecodeNotice(&normal) == 73 && forwarded == prior + 1 && forwardedReader == &normal && normal.cursor == 0,
          "unmarked normal GM notice forwards original reader and return value unchanged");
    body = valid; body.insert(body.begin(), 5, 0xAA);
    Check(Receive(body, 5) == 0 && displayed == count + 1, "notice at nonzero native reader cursor works");
    EventNotice::testReady = false; Receive(valid);
    EventNotice::testReady = true; EventNotice::testManager = nullptr; Receive(valid);
    Check(displayed == count + 1, "not-ready or missing UI does not call native dispatch");
    Check(EventNotice::DecodeNotice(reinterpret_cast<void*>(1)) == 73, "unreadable reader is guarded before forwarding unchanged");
    TestMonsterNames();
    const int sent = broadcast;
    CustomEvents::MapTH::Announce("ordinary GM message");
    CustomEvents::MapTH::Announce("[Event]: injected\nline");
    Check(broadcast == sent, "event encoder rejects unrelated and multiline text");
    TestNativeImage(argv[1]);
    std::printf("Event notice server/client: %d checks, 0 failures\n", checks);
}
