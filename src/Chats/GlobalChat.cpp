#include "GlobalChat.h"
#include "../../shared/GlobalChatWire.h"
#include "detours.h"
#include <intrin.h>
#include <cstdio>

namespace GlobalChat {
namespace {
using namespace GlobalChatWire;

constexpr unsigned ShoutDecoder = 0x00402B80;
constexpr unsigned FindWindowAddress = 0x00585580;
constexpr unsigned FindControlAddress = 0x0056AE30;
constexpr unsigned FindAllAddress = 0x005F4670;
constexpr unsigned InsertAddress = 0x004F6620;
constexpr unsigned FontRectAddress = 0x00516E50, FontTextAddress = 0x00516D30;
constexpr unsigned ChatManagerId = 2, StatusWindowId = 7;
constexpr unsigned ChatManagerType = 0x00C3FD34, ChatWindowType = 0x00C3F8DC;
constexpr unsigned StatusWindowType = 0x00C3059C, TextBoxType = 0x00C2EB54;
constexpr unsigned ListBoxType = 0x00C2EA7C;
constexpr unsigned Palette = 0x00C2E930;
constexpr unsigned WorldFallback = 0x2C, TradeFallback = 0x2B;
constexpr unsigned WorldFallbackColor = 0xFFFFFF99, TradeFallbackColor = 0xFF82C0FB;
constexpr unsigned MeleeLabel = 0x3E, RangedLabel = 0x41;
constexpr unsigned WorldOutline = 0x5B524757, TradeOutline = 0x5B524754;
constexpr unsigned NativeCategory = 15;
constexpr std::size_t MaxLine = 8 + MaxName + 2 + MaxText;

using ReaderFn = int(__cdecl*)(void*);
using LookupFn = void*(__thiscall*)(void*, unsigned);
using AllFn = void*(__thiscall*)(void*);
using InsertFn = void(__thiscall*)(void*, const void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
using FontRectFn = void(__thiscall*)(void*, const char*, const void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
using FontTextFn = void(__thiscall*)(void*, unsigned, const void*, unsigned, unsigned, unsigned, unsigned, const char*);
ReaderFn originalShout = reinterpret_cast<ReaderFn>(ShoutDecoder);
LookupFn findWindow = reinterpret_cast<LookupFn>(FindWindowAddress);
LookupFn findControl = reinterpret_cast<LookupFn>(FindControlAddress);
AllFn findAll = reinterpret_cast<AllFn>(FindAllAddress);
InsertFn insert = reinterpret_cast<InsertFn>(InsertAddress);
FontRectFn originalRect = reinterpret_cast<FontRectFn>(FontRectAddress);
FontTextFn originalText = reinterpret_cast<FontTextFn>(FontTextAddress);

struct NativeText {
    unsigned proxy = 0;
    const char* data;
    unsigned unused[3]{};
    unsigned length;
    unsigned capacity = static_cast<unsigned>(MaxLine);
};
static_assert(sizeof(NativeText) == 28 && offsetof(NativeText, length) == 20);

struct Destination {
    void* list = nullptr;
    unsigned index = 0;
    unsigned outline = 0;
};

#ifdef GLOBAL_CHAT_TESTING
void* testUiManager = nullptr;
bool testInWorld = true;
thread_local unsigned testCaller = 0;
#endif

template<class T>
T Read(const void* object, unsigned offset = 0) {
    return *reinterpret_cast<const T*>(static_cast<const unsigned char*>(object) + offset);
}

void* Window(unsigned id, unsigned type) {
#ifdef GLOBAL_CHAT_TESTING
    void* ui = testUiManager;
#else
    void* ui = Read<void*>(reinterpret_cast<void*>(0x00D4C5E0));
#endif
    if (!ui) return nullptr;
    void* window = findWindow(ui, id);
    return window && Read<unsigned>(window) == type ? window : nullptr;
}

unsigned StatusColor(Channel channel, unsigned fallback) {
    __try {
        auto* status = Window(StatusWindowId, StatusWindowType);
        if (!status) return fallback;
        auto* label = findControl(status, channel == Channel::World ? MeleeLabel : RangedLabel);
        if (!label || Read<unsigned>(label) != TextBoxType) return fallback;
        // TextBox::Draw forwards +0x78 as the foreground ARGB. Use the label
        // color supplied by CharacterStatus.ui, independent of the stat value.
        const unsigned color = Read<unsigned>(label, 0x78);
        return (color >> 24) && (color & 0x00FFFFFF) ? color : fallback;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

Destination Resolve(Channel channel) {
    __try {
#ifdef GLOBAL_CHAT_TESTING
        if (!testInWorld) return {};
#else
        if (!Read<unsigned char>(reinterpret_cast<void*>(0x00D3D2CD))) return {};
#endif
        auto* manager = Window(ChatManagerId, ChatManagerType);
        if (!manager || !Read<void*>(manager, 0xF0)) return {};
        auto* all = findAll(manager);
        if (!all || Read<unsigned>(all) != ChatWindowType || Read<unsigned>(all, 0x144)) return {};
        auto* wrapper = Read<void*>(all, 0x104);
        auto* list = wrapper ? Read<void*>(wrapper, 4) : nullptr;
        if (!list || Read<unsigned>(list) != ListBoxType) return {};
        return {list, channel == Channel::World ? WorldFallback : TradeFallback,
            channel == Channel::World ? WorldOutline : TradeOutline};
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
}

DecodeResult ReadChannel(void* reader, unsigned char (&buffer)[HeaderSize + MaxText + MaxLink], Message& message) {
    bool marked = false;
    __try {
        if (!reader) return DecodeResult::Unmarked;
        const auto* data = Read<const unsigned char*>(reader, 4);
        const unsigned size = Read<unsigned>(reader, 0x0C);
        const unsigned cursor = Read<unsigned>(reader, 0x10);
        if (!data || size > 65535 || cursor > size || size - cursor < 30) return DecodeResult::Unmarked;
        const auto* body = data + cursor;
        if (Read<unsigned>(body, 26) != Magic) return DecodeResult::Unmarked;
        marked = true;
        *reinterpret_cast<unsigned*>(static_cast<unsigned char*>(reader) + 0x10) = size;
        if (size - cursor > sizeof buffer) return DecodeResult::Invalid;
        std::memcpy(buffer, body, size - cursor);
        return Decode(buffer, size - cursor, message);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return marked ? DecodeResult::Invalid : DecodeResult::Unmarked;
    }
}

int __cdecl DecodeShout(void* reader) {
    unsigned char bytes[HeaderSize + MaxText + MaxLink];
    Message message;
    const auto result = ReadChannel(reader, bytes, message);
    if (result == DecodeResult::Unmarked) return originalShout(reader);
    if (result != DecodeResult::Message) return 0;
    const auto target = Resolve(message.channel);
    if (!target.list) return 0;

    char line[MaxLine + 1]{};
    const int count = std::snprintf(line, sizeof line, "[%s] %.*s: %.*s",
        Label(message.channel), static_cast<int>(message.name.size()), message.name.data(),
        static_cast<int>(message.text.size()), message.text.data());
    if (count < 1 || static_cast<std::size_t>(count) >= sizeof line) return 0;
    const NativeText native{0, line, {}, static_cast<unsigned>(count)};
    // Add directly to the All listbox. Argument 7 is stored at +0xAC on
    // each wrapped row and forwarded to the font renderer. Use a private
    // marker there and restore the native value at the font boundary while
    // substituting the Status foreground.
    insert(target.list, &native, target.index, 0, NativeCategory, 0, 0, target.outline);
    return 0;
}

Channel DrawChannel(unsigned caller, unsigned color, unsigned outline, bool rect) {
    const unsigned expectedColor = outline == WorldOutline ? WorldFallbackColor : TradeFallbackColor;
    if ((outline != WorldOutline && outline != TradeOutline) || color != expectedColor) return Channel::None;
    if (rect) {
        switch (caller) {
        case 0x004F360B: case 0x004F37B7: case 0x004F3987: case 0x004F3B7D:
        case 0x004F3D6E: case 0x004F4003: case 0x004F416E: case 0x004F42E0:
        case 0x004F4463: case 0x004F45F2: break;
        default: return Channel::None;
        }
    } else {
        switch (caller) {
        case 0x004F3655: case 0x004F384D: case 0x004F3A3B: case 0x004F3C31:
        case 0x004F3DB8: case 0x004F404D: case 0x004F41B8: case 0x004F432A:
        case 0x004F44AD: case 0x004F4634: break;
        default: return Channel::None;
        }
    }
    return outline == WorldOutline ? Channel::World : Channel::Trade;
}

void __fastcall DrawRect(void* self, void*, const char* text, const void* rect,
    unsigned color, unsigned font, unsigned outline, unsigned style, unsigned a, unsigned b) {
#ifdef GLOBAL_CHAT_TESTING
    const auto caller = testCaller;
#else
    const auto caller = reinterpret_cast<unsigned>(_ReturnAddress());
#endif
    const auto channel = DrawChannel(caller, color, outline, true);
    if (Known(channel)) {
        color = StatusColor(channel, color);
        outline = 2;
    }
    originalRect(self, text, rect, color, font, outline, style, a, b);
}
void __fastcall DrawText(void* self, void*, unsigned font, const void* rect, unsigned align,
    unsigned color, unsigned style, unsigned outline, const char* text) {
#ifdef GLOBAL_CHAT_TESTING
    const auto caller = testCaller;
#else
    const auto caller = reinterpret_cast<unsigned>(_ReturnAddress());
#endif
    const auto channel = DrawChannel(caller, color, outline, false);
    if (Known(channel)) {
        color = StatusColor(channel, color);
        outline = 2;
    }
    originalText(self, font, rect, align, color, style, outline, text);
}
}

bool MatchesReference(const unsigned char* base) {
    __try {
        if (!base) return false;
        constexpr unsigned char decoder[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x6A,0xFF,0x68,0x84,0xAC,0xBD,0x00,0x64,0xA1,0x00};
        constexpr unsigned char window[] = {0x8B,0x51,0x04,0x8B,0x02,0x3B,0xC2,0x56,0x74,0x18,0x8B,0x74,0x24,0x08,0x8B,0xFF};
        constexpr unsigned char all[] = {0x8B,0x89,0xF0,0,0,0,0x8B,0x01,0x3B,0xC1,0x56,0x74,0x16,0x8D,0x49,0};
        constexpr unsigned char control[] = {0x8B,0x91,0xD4,0,0,0,0x85,0xD2,0x74,0x13,0x8B,0x81,0xD8,0,0,0};
        constexpr unsigned char add[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x6A,0xFF,0x68,0x0B,0x18,0xBE,0,0x64,0xA1,0};
        constexpr unsigned char font[] = {0x6A,0xFF,0x68,0x78,0x29,0xBE,0,0x64,0xA1,0,0,0,0,0x50,0x64,0x89};
        return !std::memcmp(base + ShoutDecoder - 0x400000, decoder, sizeof decoder) &&
            !std::memcmp(base + FindWindowAddress - 0x400000, window, sizeof window) &&
            !std::memcmp(base + FindAllAddress - 0x400000, all, sizeof all) &&
            !std::memcmp(base + FindControlAddress - 0x400000, control, sizeof control) &&
            !std::memcmp(base + InsertAddress - 0x400000, add, sizeof add) &&
            !std::memcmp(base + FontRectAddress - 0x400000, font, sizeof font) &&
            !std::memcmp(base + FontTextAddress - 0x400000, font, sizeof font) &&
            Read<unsigned>(base, Palette - 0x400000 + WorldFallback * 4) == WorldFallbackColor &&
            Read<unsigned>(base, Palette - 0x400000 + TradeFallback * 4) == TradeFallbackColor;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
LONG Attach() {
    LONG result = DetourAttach(&reinterpret_cast<PVOID&>(originalShout), reinterpret_cast<PVOID>(DecodeShout));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalRect), reinterpret_cast<PVOID>(DrawRect));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalText), reinterpret_cast<PVOID>(DrawText));
    return result;
}
}
