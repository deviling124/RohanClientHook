#include "CombatStatLines.h"
#include "TooltipVariants.generated.h"
#include <cstdint>
#include <cstring>
#include "detours.h"

namespace CombatStatLines {
using ItemTooltip::Variant;
using ItemTooltip::kVariants;
namespace {
struct Line { int type; unsigned textId; };
// Attribute numbers are the item bin AttributeType values (the server's Hero6 numbers); text ids are text.bin records.
constexpr Line kLines[] = {
    {181, 6461},     // PVP Attack :
    {204, 6459},     // PVP Defense :
    {182, 6462},     // PVE Attack :
    {203, 6460},     // PVE Defense :
    {176, 6460},     // PVE Defense : (alternate number)
    {183, 6464},     // Skill Damage :   (added to text.bin by tools/add_combat_stat_items.py)
    {179, 200061},   // Skill Defense :  (already in text.bin)
};
constexpr unsigned kLineCount = sizeof kLines / sizeof kLines[0];

void Put8(unsigned char*& out, unsigned char value) { *out++ = value; }
void Put32(unsigned char*& out, std::uint32_t value) { std::memcpy(out, &value, 4); out += 4; }
std::uint32_t Rel(const unsigned char* from, std::uint32_t target) {   // `from` = address just after a 4 byte displacement
    return target - static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(from));
}
}

bool MatchesReference(const unsigned char*) {
    __try {
        for (const Variant& v : kVariants) {
            const auto* ja = reinterpret_cast<const unsigned char*>(v.jaSite);
            std::int32_t rel;
            std::memcpy(&rel, ja + 2, 4);
            if (ja[0] != 0x0F || ja[1] != 0x87 || v.jaSite + 6 + rel != v.defaultTarget) return false;
            if (std::memcmp(reinterpret_cast<const void*>(v.blockStart), v.block, v.blockSize) != 0) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG AttachAttributeLines() {
    if (!MatchesReference(nullptr)) return NO_ERROR;   // another client build: the tooltip stays as it is
    constexpr unsigned perVariant = 64 + kLineCount * 160;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, perVariant * (sizeof kVariants / sizeof kVariants[0]),
                                                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!memory) return static_cast<LONG>(GetLastError());
    unsigned char* out = memory;
    unsigned char* jumps[8]{};
    unsigned jumpCount = 0;
    for (const Variant& v : kVariants) {
        unsigned char* stub = out;
        // Dispatcher: at the original `ja` eax = type - 21. A type that is ours goes to its copy, everything else to the old target.
        unsigned char* je[kLineCount]{};
        for (unsigned i = 0; i < kLineCount; ++i) {
            Put8(out, 0x3D); Put32(out, static_cast<std::uint32_t>(kLines[i].type - 21));   // cmp eax, imm32
            Put8(out, 0x0F); Put8(out, 0x84); je[i] = out; Put32(out, 0);                   // je copy
        }
        Put8(out, 0xE9); Put32(out, Rel(out + 4, v.defaultTarget));                          // jmp default
        for (unsigned i = 0; i < kLineCount; ++i) {
            std::uint32_t displacement = Rel(je[i] + 4, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(out)));
            std::memcpy(je[i], &displacement, 4);
            Put8(out, 0x56);                                                                 // push esi (the original path pushed it before the case)
            unsigned char* copy = out;
            std::memcpy(copy, v.block, v.blockSize);
            const std::uint32_t type = static_cast<std::uint32_t>(kLines[i].type), text = kLines[i].textId;
            std::memcpy(copy + v.typeImm, &type, 4);
            std::memcpy(copy + v.textImm, &text, 4);
            for (unsigned c = 0; c < v.callCount; ++c) {                                     // relative calls keep their targets
                std::int32_t rel;
                std::memcpy(&rel, v.block + v.callOffsets[c] + 1, 4);
                const std::uint32_t target = v.blockStart + v.callOffsets[c] + 5 + static_cast<std::uint32_t>(rel);
                const std::uint32_t now = Rel(copy + v.callOffsets[c] + 5, target);
                std::memcpy(copy + v.callOffsets[c] + 1, &now, 4);
            }
            out += v.blockSize;
            Put8(out, 0xE9); Put32(out, Rel(out + 4, v.afterJoin));                          // continue after the join call
        }
        jumps[jumpCount++] = stub;
    }
    FlushInstructionCache(GetCurrentProcess(), memory, static_cast<SIZE_T>(out - memory));
    for (unsigned i = 0; i < jumpCount; ++i) {
        const Variant& v = kVariants[i];
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(v.jaSite), 6, PAGE_EXECUTE_READWRITE, &old)) return static_cast<LONG>(GetLastError());
        auto* site = reinterpret_cast<unsigned char*>(v.jaSite);
        // Keep the conditional `ja` (0F 87): only a type outside the game's own switch may reach the dispatcher.
        const std::uint32_t rel = Rel(site + 6, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(jumps[i])));
        std::memcpy(site + 2, &rel, 4);
        VirtualProtect(reinterpret_cast<void*>(v.jaSite), 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 6);
    }
    return NO_ERROR;
}

namespace {
// Per-item options (the database's attr bytes) are printed by a second loop in the builders: it asks the item for the option of every
// type 0..98 and a switch (type - 1, 98 cases) picks the text. Types above 98 are never asked for. Two patches per builder:
//   1. the `ja` of the switch (type too big) goes to a dispatcher that, for our types, pushes the value and the text id and
//      continues at the game's own "label +value%" code, the same one the Melee Attack case uses;
//   2. the loop tail, which counted to 98, counts 0..98 and then 176..204.
// The six sites differ in where the loop counter is (stack slot or ebx) and which register holds the value (edi or esi).
struct OptionSite {
    unsigned ja, tail, top, caseMelee, common;
    unsigned counterSlot;     // [esp + slot] holds the counter, or 0 when the counter is ebx
    unsigned char valuePush;  // 0x57 push edi / 0x56 push esi
};
constexpr OptionSite kOptionSites[] = {
    {0x6A1714, 0x6A1A70, 0x6A16F4, 0x6A1899, 0x6A1A20, 0x10, 0x57},
    {0x6A7070, 0x6A73B1, 0x6A7050, 0x6A71F5, 0x6A7361, 0x10, 0x57},
    {0x6AC260, 0x6AC5BC, 0x6AC240, 0x6AC3E5, 0x6AC56C, 0x10, 0x57},
    {0x6AC64E, 0x6AC9AD, 0x6AC634, 0x6AC7D3, 0x6AC95D, 0x00, 0x57},
    {0x8B2FD0, 0x8B3319, 0x8B2FB0, 0x8B314D, 0x8B32D1, 0x14, 0x56},
    {0x8B4BB0, 0x8B4EED, 0x8B4B90, 0x8B4D27, 0x8B4EAB, 0x14, 0x56},
};
constexpr unsigned kSiteCount = sizeof kOptionSites / sizeof kOptionSites[0];
constexpr unsigned kListCount = 2;
constexpr unsigned kFlagSites = kSiteCount + kListCount + 5;
volatile unsigned char g_seen[kFlagSites * 2]{};   // [site*2] the loop ran, [site*2+1] an option above 98 with a value was found

unsigned TailBytes(const OptionSite& s) { return s.counterSlot ? 18 : 10; }

bool OptionSiteMatches(const OptionSite& s) {
    __try {
        const auto* ja = reinterpret_cast<const unsigned char*>(s.ja);
        std::int32_t rel;
        std::memcpy(&rel, ja + 2, 4);
        if (ja[0] != 0x0F || ja[1] != 0x87 || s.ja + 6 + rel != s.tail) return false;
        const auto* tail = reinterpret_cast<const unsigned char*>(s.tail);
        unsigned jlAt;
        if (s.counterSlot) {
            const unsigned char want[12] = {0x8B, 0x44, 0x24, static_cast<unsigned char>(s.counterSlot), 0x40, 0x83, 0xF8, 0x63,
                                            0x89, 0x44, 0x24, static_cast<unsigned char>(s.counterSlot)};
            if (std::memcmp(tail, want, sizeof want) != 0) return false;
            jlAt = 12;
        } else {
            const unsigned char want[4] = {0x43, 0x83, 0xFB, 0x63};   // inc ebx; cmp ebx, 63h
            if (std::memcmp(tail, want, sizeof want) != 0) return false;
            jlAt = 4;
        }
        const auto* jl = tail + jlAt;
        std::memcpy(&rel, jl + 2, 4);
        if (jl[0] != 0x0F || jl[1] != 0x8C || s.tail + jlAt + 6 + rel != s.top) return false;
        const auto* melee = reinterpret_cast<const unsigned char*>(s.caseMelee);   // push value; push 0x30D4E; jmp common
        if (melee[0] != s.valuePush || melee[1] != 0x68 || melee[6] != 0xE9) return false;
        std::uint32_t id;
        std::memcpy(&id, melee + 2, 4);
        std::memcpy(&rel, melee + 7, 4);
        return id == 0x30D4E && s.caseMelee + 11 + rel == s.common;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void PutFlag(unsigned char*& out, volatile unsigned char* flag) {   // mov byte ptr [flag], 1
    Put8(out, 0xC6); Put8(out, 0x05); Put32(out, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(flag))); Put8(out, 1);
}

DWORD WINAPI TraceThread(LPVOID) {
    bool logged[kFlagSites * 2]{};
    for (;;) {
        Sleep(400);
        for (unsigned i = 0; i < kFlagSites * 2; ++i) {
            if (!g_seen[i] || logged[i]) continue;
            logged[i] = true;
            HANDLE file = CreateFileW(L"novaclient_tooltip.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) continue;
            char line[160];
            const int n = wsprintfA(line, "%s site %u: %s\r\n", i / 2 < kSiteCount ? "option" : i / 2 < kSiteCount + kListCount ? "list" : "formatter/loop", i / 2,
                                    (i & 1) ? "an option above 98 with a value reached the tooltip" : "tooltip loop ran");
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
            CloseHandle(file);
        }
    }
}

void LogLine(const char* text) {
    HANDLE file = CreateFileW(L"novaclient_tooltip.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    CloseHandle(file);
}

LONG AttachOptionLines() {
    for (unsigned i = 0; i < kSiteCount; ++i)
        if (!OptionSiteMatches(kOptionSites[i])) {
            char line[80];
            wsprintfA(line, "option site %u does not match this client; option lines not patched\r\n", i);
            LogLine(line);
            return NO_ERROR;   // another client build: leave the option lines as they are
        }
    constexpr unsigned perSite = 160 + kLineCount * 20;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, perSite * kSiteCount, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!memory) return static_cast<LONG>(GetLastError());
    unsigned char* out = memory;
    unsigned char* dispatcher[kSiteCount]{};
    unsigned char* tailStub[kSiteCount]{};
    unsigned n = 0;
    for (const OptionSite& s : kOptionSites) {
        dispatcher[n] = out;   // counter = type here, the value register = the value
        PutFlag(out, &g_seen[n * 2 + 1]);
        for (const Line& line : kLines) {
            if (s.counterSlot) { Put8(out, 0x3D); Put32(out, static_cast<std::uint32_t>(line.type)); }                     // cmp eax, type
            else { Put8(out, 0x81); Put8(out, 0xFB); Put32(out, static_cast<std::uint32_t>(line.type)); }                  // cmp ebx, type
            Put8(out, 0x75); Put8(out, 11);                                                                                // jne next
            Put8(out, s.valuePush);                                                                                        // push value
            Put8(out, 0x68); Put32(out, line.textId);                                                                      // push text id
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.common));                                                           // jmp "label +value%"
        }
        Put8(out, 0xE9); Put32(out, Rel(out + 4, s.tail));                                                                 // not ours: the old target
        tailStub[n] = out;
        PutFlag(out, &g_seen[n * 2]);
        if (s.counterSlot) {
            const unsigned char slot = static_cast<unsigned char>(s.counterSlot);
            const unsigned char head[] = {0x8B, 0x44, 0x24, slot, 0x40,                  // mov eax,[esp+slot]; inc eax
                                          0x83, 0xF8, 0x63, 0x75, 0x05,                  // cmp eax,63h; jne +5
                                          0xB8, 0xB0, 0x00, 0x00, 0x00,                  // mov eax,176
                                          0x3D, 0xCD, 0x00, 0x00, 0x00,                  // cmp eax,205
                                          0x7D, 0x09,                                    // jge done
                                          0x89, 0x44, 0x24, slot};                       // mov [esp+slot],eax
            std::memcpy(out, head, sizeof head);
            out += sizeof head;
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.top));                            // next type
            Put8(out, 0x89); Put8(out, 0x44); Put8(out, 0x24); Put8(out, slot);          // done: mov [esp+slot],eax
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.tail + 18));                      // what followed the loop
        } else {
            static const unsigned char head[] = {0x43,                                   // inc ebx
                                                 0x83, 0xFB, 0x63, 0x75, 0x05,           // cmp ebx,63h; jne +5
                                                 0xBB, 0xB0, 0x00, 0x00, 0x00,           // mov ebx,176
                                                 0x81, 0xFB, 0xCD, 0x00, 0x00, 0x00,     // cmp ebx,205
                                                 0x7D, 0x05};                            // jge done
            std::memcpy(out, head, sizeof head);
            out += sizeof head;
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.top));                            // next type
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.tail + 10));                      // done: what followed the loop
        }
        ++n;
    }
    FlushInstructionCache(GetCurrentProcess(), memory, static_cast<SIZE_T>(out - memory));
    n = 0;
    for (const OptionSite& s : kOptionSites) {
        DWORD old = 0;
        auto* tail = reinterpret_cast<unsigned char*>(s.tail);
        const unsigned tailBytes = TailBytes(s);
        if (!VirtualProtect(tail, tailBytes, PAGE_EXECUTE_READWRITE, &old)) return static_cast<LONG>(GetLastError());
        tail[0] = 0xE9;
        std::uint32_t rel = Rel(tail + 5, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(tailStub[n])));
        std::memcpy(tail + 1, &rel, 4);
        std::memset(tail + 5, 0x90, tailBytes - 5);
        VirtualProtect(tail, tailBytes, old, &old);
        FlushInstructionCache(GetCurrentProcess(), tail, tailBytes);
        auto* ja = reinterpret_cast<unsigned char*>(s.ja);
        if (!VirtualProtect(ja, 6, PAGE_EXECUTE_READWRITE, &old)) return static_cast<LONG>(GetLastError());
        // Keep the conditional `ja` (0F 87): only a type outside the game's own switch may reach the dispatcher.
        rel = Rel(ja + 6, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(dispatcher[n])));
        std::memcpy(ja + 2, &rel, 4);
        VirtualProtect(ja, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), ja, 6);
        ++n;
    }
    LogLine("option lines patched at 6 sites\r\n");
    if (HANDLE thread = CreateThread(nullptr, 0, TraceThread, nullptr, 0, nullptr)) CloseHandle(thread);
    return NO_ERROR;
}

// Two more builders walk a list of option entries (type at entry + 0xC, value at entry + 0x10) instead of asking per type, so only the
// switch's `ja` needs the dispatcher: edi = type, esi = value; the game's own text case is `mov ecx,[0xD44A18]; push esi; push id; call`.
struct ListSite { unsigned ja, defaultTarget, caseCall; };
constexpr ListSite kListSites[kListCount] = {{0x7AC533, 0x7ACBD5, 0x7AC81C}, {0x7AFB73, 0x7B0215, 0x7AFE5C}};

bool ListSiteMatches(const ListSite& s) {
    __try {
        const auto* ja = reinterpret_cast<const unsigned char*>(s.ja);
        std::int32_t rel;
        std::memcpy(&rel, ja + 2, 4);
        if (ja[0] != 0x0F || ja[1] != 0x87 || s.ja + 6 + rel != s.defaultTarget) return false;
        static const unsigned char want[12] = {0x8B, 0x0D, 0x18, 0x4A, 0xD4, 0x00, 0x56, 0x68, 0x4F, 0x0D, 0x03, 0x00};
        return std::memcmp(reinterpret_cast<const void*>(s.caseCall - 12), want, sizeof want) == 0 &&
               *reinterpret_cast<const unsigned char*>(s.caseCall) == 0xE8;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG AttachListLines() {
    for (const ListSite& s : kListSites)
        if (!ListSiteMatches(s)) { LogLine("list sites do not match this client; not patched\r\n"); return NO_ERROR; }
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!memory) return static_cast<LONG>(GetLastError());
    unsigned char* out = memory;
    unsigned char* stub[kListCount]{};
    for (unsigned n = 0; n < kListCount; ++n) {
        const ListSite& s = kListSites[n];
        stub[n] = out;
        PutFlag(out, &g_seen[(kSiteCount + n) * 2 + 1]);
        for (const Line& line : kLines) {
            Put8(out, 0x81); Put8(out, 0xFF); Put32(out, static_cast<std::uint32_t>(line.type));        // cmp edi, type
            Put8(out, 0x75); Put8(out, 17);                                                              // jne next
            Put8(out, 0x8B); Put8(out, 0x0D); Put32(out, 0x00D44A18);                                    // mov ecx,[0xD44A18]
            Put8(out, 0x56);                                                                             // push esi (value)
            Put8(out, 0x68); Put32(out, line.textId);                                                    // push text id
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.caseCall));                                       // jmp the case's `call GetText`
        }
        Put8(out, 0xE9); Put32(out, Rel(out + 4, s.defaultTarget));
    }
    FlushInstructionCache(GetCurrentProcess(), memory, static_cast<SIZE_T>(out - memory));
    for (unsigned n = 0; n < kListCount; ++n) {
        DWORD old = 0;
        auto* ja = reinterpret_cast<unsigned char*>(kListSites[n].ja);
        if (!VirtualProtect(ja, 6, PAGE_EXECUTE_READWRITE, &old)) return static_cast<LONG>(GetLastError());
        // Keep the conditional `ja` (0F 87): only a type outside the game's own switch may reach the dispatcher.
        const std::uint32_t rel = Rel(ja + 6, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stub[n])));
        std::memcpy(ja + 2, &rel, 4);
        VirtualProtect(ja, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), ja, 6);
    }
    LogLine("option lines patched at 2 list sites\r\n");
    return NO_ERROR;
}

// The weapon tooltip does not walk the lists above: three loops ask the item for the option of every type 0..98 and hand the type and the
// value to a line formatter (0x69E740 and 0x8ABA40, __thiscall(type, x, value, color)) that switches over type - 1. Flag slots 8, 9 are the
// two formatters and 10 to 12 the three loops (see TraceThread).
struct FuncSite { unsigned ja, defaultTarget, caseMelee, common; };
constexpr FuncSite kFuncSites[2] = {{0x69E7A4, 0x69ED59, 0x69E9E5, 0x69ED03}, {0x8ABAA4, 0x8AC03B, 0x8ABCDB, 0x8ABFE9}};
// `inc reg; cmp reg,63h; jl top` with a short jump: 6 bytes at `tail`.
struct ShortLoop { unsigned tail, top; unsigned char reg; };   // reg: 0 = esi, 1 = edi
constexpr ShortLoop kShortLoops[3] = {{0x8B10E5, 0x8B10C3, 0}, {0x8B1218, 0x8B1200, 0}, {0x6A1B08, 0x6A1AF0, 1}};
constexpr unsigned kFuncFlag = kSiteCount + kListCount;   // first flag slot of these sites

bool FuncSiteMatches(const FuncSite& s) {
    __try {
        const auto* ja = reinterpret_cast<const unsigned char*>(s.ja);
        std::int32_t rel;
        std::memcpy(&rel, ja + 2, 4);
        if (ja[0] != 0x0F || ja[1] != 0x87 || s.ja + 6 + rel != s.defaultTarget) return false;
        static const unsigned char want[10] = {0x8B, 0x4D, 0x10, 0x51, 0x68, 0x4E, 0x0D, 0x03, 0x00, 0xE9};
        const auto* melee = reinterpret_cast<const unsigned char*>(s.caseMelee);
        if (std::memcmp(melee, want, sizeof want) != 0) return false;
        std::memcpy(&rel, melee + 10, 4);
        return s.caseMelee + 14 + rel == s.common;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ShortLoopMatches(const ShortLoop& l) {
    __try {
        const unsigned char want[5] = {static_cast<unsigned char>(l.reg ? 0x47 : 0x46), 0x83, static_cast<unsigned char>(l.reg ? 0xFF : 0xFE), 0x63, 0x7C};
        const auto* t = reinterpret_cast<const unsigned char*>(l.tail);
        return std::memcmp(t, want, sizeof want) == 0 && l.tail + 6 + static_cast<signed char>(t[5]) == l.top;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG AttachFuncLines() {
    for (const FuncSite& s : kFuncSites)
        if (!FuncSiteMatches(s)) { LogLine("formatter sites do not match this client; not patched\r\n"); return NO_ERROR; }
    for (const ShortLoop& l : kShortLoops)
        if (!ShortLoopMatches(l)) { LogLine("option loops do not match this client; not patched\r\n"); return NO_ERROR; }
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!memory) return static_cast<LONG>(GetLastError());
    unsigned char* out = memory;
    unsigned char* funcStub[2]{};
    unsigned char* loopStub[3]{};
    for (unsigned n = 0; n < 2; ++n) {
        const FuncSite& s = kFuncSites[n];
        funcStub[n] = out;   // eax = type - 1
        PutFlag(out, &g_seen[(kFuncFlag + n) * 2 + 1]);
        for (const Line& line : kLines) {
            Put8(out, 0x3D); Put32(out, static_cast<std::uint32_t>(line.type - 1));                      // cmp eax, type - 1
            Put8(out, 0x75); Put8(out, 14);                                                              // jne next
            Put8(out, 0x8B); Put8(out, 0x4D); Put8(out, 0x10);                                           // mov ecx,[ebp+10h] (the value)
            Put8(out, 0x51);                                                                             // push ecx
            Put8(out, 0x68); Put32(out, line.textId);                                                    // push text id
            Put8(out, 0xE9); Put32(out, Rel(out + 4, s.common));                                         // jmp the Melee Attack case's tail
        }
        Put8(out, 0xE9); Put32(out, Rel(out + 4, s.defaultTarget));
    }
    for (unsigned n = 0; n < 3; ++n) {
        const ShortLoop& l = kShortLoops[n];
        loopStub[n] = out;
        PutFlag(out, &g_seen[(kFuncFlag + 2 + n) * 2]);
        const unsigned char inc = l.reg ? 0x47 : 0x46, modrm = l.reg ? 0xFF : 0xFE, mov = l.reg ? 0xBF : 0xBE;
        const unsigned char head[] = {inc, 0x83, modrm, 0x63, 0x75, 0x05,                  // inc reg; cmp reg,63h; jne +5
                                      mov, 0xB0, 0x00, 0x00, 0x00,                          // mov reg,176
                                      0x81, modrm, 0xCD, 0x00, 0x00, 0x00,                  // cmp reg,205
                                      0x0F, 0x8C};                                          // jl top
        std::memcpy(out, head, sizeof head);
        out += sizeof head;
        Put32(out, Rel(out + 4, l.top));
        Put8(out, 0xE9); Put32(out, Rel(out + 4, l.tail + 6));                              // after the loop
    }
    FlushInstructionCache(GetCurrentProcess(), memory, static_cast<SIZE_T>(out - memory));
    DWORD old = 0;
    for (unsigned n = 0; n < 2; ++n) {
        auto* ja = reinterpret_cast<unsigned char*>(kFuncSites[n].ja);
        if (!VirtualProtect(ja, 6, PAGE_EXECUTE_READWRITE, &old)) return static_cast<LONG>(GetLastError());
        // Keep the conditional `ja` (0F 87): only a type outside the game's own switch may reach the dispatcher.
        const std::uint32_t rel = Rel(ja + 6, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(funcStub[n])));
        std::memcpy(ja + 2, &rel, 4);
        VirtualProtect(ja, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), ja, 6);
    }
    for (unsigned n = 0; n < 3; ++n) {
        auto* t = reinterpret_cast<unsigned char*>(kShortLoops[n].tail);
        if (!VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &old)) return static_cast<LONG>(GetLastError());
        t[0] = 0xE9;
        const std::uint32_t rel = Rel(t + 5, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(loopStub[n])));
        std::memcpy(t + 1, &rel, 4);
        t[5] = 0x90;
        VirtualProtect(t, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, 6);
    }
    LogLine("option lines patched at 2 formatters and 3 loops\r\n");
    return NO_ERROR;
}

// Diagnosis: the first calls of the two option line formatters (type, value) go to the log.
using FormatterFn = void(__thiscall*)(void*, int, int, int, int);
FormatterFn originalFormatterA = reinterpret_cast<FormatterFn>(0x0069E740);
FormatterFn originalFormatterB = reinterpret_cast<FormatterFn>(0x008ABA40);
volatile LONG g_formatterCalls = 0;

void LogFormatter(const char* which, int type, int value, int color) {
    if (InterlockedIncrement(&g_formatterCalls) > 40) return;
    char line[100];
    wsprintfA(line, "%s type=%d value=%d color=%d\r\n", which, type, value, color);
    LogLine(line);
}
void __fastcall FormatterA(void* self, void*, int type, int x, int value, int color) {
    LogFormatter("69E740", type, value, color);
    originalFormatterA(self, type, x, value, color);
}
void __fastcall FormatterB(void* self, void*, int type, int x, int value, int color) {
    LogFormatter("8ABA40", type, value, color);
    originalFormatterB(self, type, x, value, color);
}

// Diagnosis: every line added to a tooltip (0x546690, __thiscall(tooltip, std::string*, int, int)) goes to the log with its text.
using AddLineFn = void(__thiscall*)(void*, const unsigned char*, int, int);
AddLineFn originalAddLine = reinterpret_cast<AddLineFn>(0x00546690);
volatile LONG g_addLineCalls = 0;

void __fastcall AddLine(void* self, void*, const unsigned char* text, int a, int b) {
    if (InterlockedIncrement(&g_addLineCalls) <= 120) {
        char line[200];
        __try {
            const unsigned length = *reinterpret_cast<const unsigned*>(text + 0x14), capacity = *reinterpret_cast<const unsigned*>(text + 0x18);
            const char* data = capacity < 16 ? reinterpret_cast<const char*>(text + 4) : *reinterpret_cast<const char* const*>(text + 4);
            char copy[80]{};
            for (unsigned i = 0; i < length && i < 79; ++i) copy[i] = data[i];
            wsprintfA(line, "  addline len=%u a=%d b=%d text=[%s]\r\n", length, a, b, copy);
        } __except (EXCEPTION_EXECUTE_HANDLER) { wsprintfA(line, "  addline (unreadable)\r\n"); }
        LogLine(line);
    }
    originalAddLine(self, text, a, b);
}
}

LONG Attach() {
    LONG error = AttachAttributeLines();
    if (error == NO_ERROR) error = AttachOptionLines();
    if (error == NO_ERROR) error = AttachListLines();
    if (error == NO_ERROR) error = AttachFuncLines();
    if (error == NO_ERROR) error = DetourAttach(&reinterpret_cast<PVOID&>(originalFormatterA), reinterpret_cast<PVOID>(FormatterA));
    if (error == NO_ERROR) error = DetourAttach(&reinterpret_cast<PVOID&>(originalFormatterB), reinterpret_cast<PVOID>(FormatterB));
    if (error == NO_ERROR) error = DetourAttach(&reinterpret_cast<PVOID&>(originalAddLine), reinterpret_cast<PVOID>(AddLine));
    return error;
}
}
