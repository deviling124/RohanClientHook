#include "NpcMenu.h"
#include "../Bridge.h"
#include "detours.h"
#include <cstdio>

// Client internals (rohanclient_unpacked.exe), see docs/COLLECTION_BOOK_NPC.md:
//  - 0x5b1290 builds the NPC dialog menu. At 0x5b12f2 it reads the NPC id from its packet ([esp+0x114]); the long run of
//    `cmp [esi+0xd8], <npc job>` blocks adds the job specific lines with AddMenuItem(window=ebp, id, text) = 0x5b1160 (thiscall).
//  - A click on a line runs 0x5acff0: it takes the id stored for the selected line (vector at window+0xf4) at 0x5ad093 and switches
//    on it (ids 2..0x10A, everything else ends in 0x5b0b2f which does nothing).
namespace {
constexpr DWORD kCaptureSite = 0x5b12f2;   // mov ecx,[esp+0x114]  (NPC id)
constexpr DWORD kInsertSite = 0x5b2c19;    // cmp [esi+0xd8],0x35  (start of the Nova Points Merchant line)
constexpr DWORD kClickSite = 0x5ad093;     // mov eax,[eax] (eax -> id of the clicked line)
constexpr DWORD kClickIgnore = 0x5b0b2f;   // the dispatcher's "unknown id" exit
constexpr DWORD kAddMenuItem = 0x5b1160;
constexpr unsigned kDefaultNpc = 328057;   // Obivos
constexpr unsigned kDefaultJob = 80;       // unused by npc.xls (jobs 49..73 are the custom ones)
constexpr unsigned kCommandId = 0x1C5A;    // outside the 2..0x10A range the stock dispatcher handles

unsigned currentNpc = 0;
unsigned configuredNpc = kDefaultNpc;
unsigned bookJob = kDefaultJob;   // npc.bin job value that shows the line (0 = off)
unsigned commandId = kCommandId;
unsigned enabled = 0;
unsigned logIds = 0;
char menuText[128] = "I wish to open the Collection Book.";
PVOID captureTrampoline = reinterpret_cast<PVOID>(kCaptureSite);
PVOID insertTrampoline = reinterpret_cast<PVOID>(kInsertSite);
PVOID clickTrampoline = reinterpret_cast<PVOID>(kClickSite);

void __cdecl Note() {
    if (!logIds) return;
    FILE* file = std::fopen("novaclient_npc.log", "a");
    if (!file) return;
    std::fprintf(file, "npc menu for id %u (configured %u)\n", currentNpc, configuredNpc);
    std::fclose(file);
}
void __cdecl OpenBook() { Bridge::RequestBook(); }

__declspec(naked) void CaptureNpc() {
    __asm {
        mov ecx, dword ptr [esp + 0x114]
        mov currentNpc, ecx
        jmp dword ptr [captureTrampoline]
    }
}
__declspec(naked) void InsertLine() {
    __asm {
        pushfd
        pushad
        call Note
        cmp enabled, 0
        je skip
        mov eax, currentNpc
        cmp eax, configuredNpc
        je show
        cmp bookJob, 0
        je skip
        mov eax, dword ptr [esi + 0xd8]
        cmp eax, bookJob
        jne skip
    show:
        push offset menuText
        push commandId
        mov ecx, ebp
        mov eax, kAddMenuItem
        call eax
    skip:
        popad
        popfd
        jmp dword ptr [insertTrampoline]
    }
}
__declspec(naked) void ClickLine() {
    __asm {
        mov ecx, dword ptr [eax]
        cmp enabled, 0
        je original
        cmp ecx, commandId
        jne original
        pushad
        call OpenBook
        popad
        mov eax, kClickIgnore
        jmp eax
    original:
        jmp dword ptr [clickTrampoline]
    }
}
}

namespace NpcMenu {
LONG Attach() {
    const wchar_t* ini = L".\\novaclient.ini";
    const int npc = GetPrivateProfileIntW(L"collection_book", L"npc", static_cast<int>(kDefaultNpc), ini);
    logIds = GetPrivateProfileIntW(L"collection_book", L"npc_log", 0, ini) != 0;
    bookJob = static_cast<unsigned>(GetPrivateProfileIntW(L"collection_book", L"npc_job", static_cast<int>(kDefaultJob), ini));
    if (npc <= 0 && bookJob == 0) return NO_ERROR;
    configuredNpc = npc > 0 ? static_cast<unsigned>(npc) : 0;
    wchar_t wide[128] = {};
    if (GetPrivateProfileStringW(L"collection_book", L"npc_text", L"", wide, 128, ini) && wide[0])
        WideCharToMultiByte(CP_ACP, 0, wide, -1, menuText, sizeof menuText, nullptr, nullptr);
    LONG result = DetourAttach(&captureTrampoline, reinterpret_cast<PVOID>(CaptureNpc));
    if (result == NO_ERROR) result = DetourAttach(&insertTrampoline, reinterpret_cast<PVOID>(InsertLine));
    if (result == NO_ERROR) result = DetourAttach(&clickTrampoline, reinterpret_cast<PVOID>(ClickLine));
    if (result == NO_ERROR) enabled = 1;
    return result;
}
}
