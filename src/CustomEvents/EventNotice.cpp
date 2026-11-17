#include "EventNotice.h"
#include "../../shared/EventNoticeProtocol.h"
#include "detours.h"
#include <cstring>

namespace EventNotice {
namespace {
constexpr unsigned DecoderAddress = 0x00403EB0, DispatchAddress = 0x005E9DE0;
constexpr unsigned PaletteAddress = 0x00C2E930, EventColorIndex = 0x19;
constexpr unsigned EventColor = 0xFF42C762; // Existing native green; Notice blue is FF4080FF.
using DecoderFn = int(__cdecl*)(void*);
using DispatchFn = void(__thiscall*)(void*, unsigned, const void*, unsigned, unsigned, void*, unsigned);
DecoderFn originalDecoder = reinterpret_cast<DecoderFn>(DecoderAddress);
DispatchFn dispatch = reinterpret_cast<DispatchFn>(DispatchAddress);
#ifdef EVENT_NOTICE_TESTING
void* testManager = nullptr;
bool testReady = true;
#endif

// A read-only view in the reference client's old MSVC string layout. Native
// chat widgets copy its contents synchronously into their own game-allocated
// strings. This object owns no game heap allocation and has no CRT destructor.
// Control characters are rejected, including the native multiline mutation path.
struct NativeText {
    unsigned proxy = 0;
    const char* data;
    unsigned unused[3]{};
    unsigned length;
    unsigned capacity = static_cast<unsigned>(EventNoticeWire::MaxText);
};
static_assert(sizeof(void*) == 4 && sizeof(NativeText) == 28);
static_assert(offsetof(NativeText, data) == 4 && offsetof(NativeText, length) == 20 && offsetof(NativeText, capacity) == 24);

void* ChatManager() {
#ifdef EVENT_NOTICE_TESTING
    return testReady ? testManager : nullptr;
#else
    __try {
        if (!*reinterpret_cast<const unsigned char*>(0x00D3D2CD)) return nullptr;
        return *reinterpret_cast<void**>(0x00D4DD4C);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#endif
}
EventNoticeWire::Result ReadNotice(void* reader, char (&text)[EventNoticeWire::MaxText + 1]) {
    using namespace EventNoticeWire;
    __try {
        if (!reader) return Result::Unrelated;
        auto* object = static_cast<unsigned char*>(reader);
        const auto* data = *reinterpret_cast<const unsigned char**>(object + 4);
        const unsigned size = *reinterpret_cast<const unsigned*>(object + 0x0C);
        const unsigned cursor = *reinterpret_cast<const unsigned*>(object + 0x10);
        if (!data || size > 65535 || cursor > size) return Result::Unrelated;
        std::string_view payload;
        const Result result = Decode(data + cursor, size - cursor, payload);
        if (result == Result::Unrelated) return result; // Stock decoder receives its exact original reader.
        if (result == Result::Event) {
            std::memcpy(text, payload.data(), payload.size());
            text[payload.size()] = '\0';
        }
        // A malformed marked event is consumed, never passed to the stock
        // decoder's unbounded text-copy path. No other packet family is changed.
        *reinterpret_cast<unsigned*>(object + 0x10) = size;
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return Result::Unrelated; }
}
int __cdecl DecodeNotice(void* reader) {
    char text[EventNoticeWire::MaxText + 1]{};
    const auto result = ReadNotice(reader, text);
    if (result == EventNoticeWire::Result::Unrelated) return originalDecoder(reader);
    if (result == EventNoticeWire::Result::Event) ShowLocal(text);
    return 0;
}
bool MatchesPalette(const unsigned char* base) {
    return *reinterpret_cast<const unsigned*>(base + PaletteAddress - 0x400000 + EventColorIndex * 4) == EventColor &&
        *reinterpret_cast<const unsigned*>(base + PaletteAddress - 0x400000 + 0x22 * 4) == 0xFF4080FF;
}
}

void ShowLocal(const char* message) {
    const auto length = strnlen_s(message, EventNoticeWire::MaxText + 1);
    if (!length || length > EventNoticeWire::MaxText) return;
    void* manager = ChatManager();
    if (!manager) return;
    NativeText native{};
    native.data = message;
    native.length = static_cast<unsigned>(length);
    // Mode 0x0D uses native notice filtering and the existing green palette entry.
    dispatch(manager, 0x0D, &native, EventColorIndex, 0, nullptr, 0);
}

bool MatchesReference(const unsigned char* base) {
    __try {
        if (!base) return false;
        constexpr unsigned char decoder[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x6A,0xFF,0x68,0x76,0xAD,0xBD,0x00,0x64,0xA1,0x00};
        constexpr unsigned char display[] = {0x51,0x8B,0x41,0x0C,0x57,0x8B,0x38,0x3B,0xF8,0x89,0x4C,0x24,0x04,0x74,0x42,0x53};
        return !std::memcmp(base + DecoderAddress - 0x400000, decoder, sizeof decoder) &&
            !std::memcmp(base + DispatchAddress - 0x400000, display, sizeof display) &&
            MatchesPalette(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
LONG Attach() {
    return DetourAttach(&reinterpret_cast<PVOID&>(originalDecoder), reinterpret_cast<PVOID>(DecodeNotice));
}
}
