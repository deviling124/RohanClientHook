#include "MonsterName.h"
#include "detours.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace MonsterName {
namespace {
// Reference client: CMonsterScriptTable::Insert(id, script), RET 8. The loader (0x00AD6DC0)
// builds an 800-byte script object per monster.bin record and registers it here.
constexpr unsigned InsertAddress = 0x00AD4B90;
// Script object layout (from the loader): +4 id; MSVC std::string at +0x08 (record name) and
// +0x24 (display name). A string is: +4 inline buffer or heap pointer, +0x14 length, +0x18 capacity.
constexpr unsigned NameOffsets[] = {0x08, 0x24};
constexpr unsigned StringLength = 0x14, StringCapacity = 0x18, StringData = 0x04;
constexpr std::size_t MaxName = 40;
constexpr ULONGLONG ExpireMs = 15000; // The server re-sends every 4 s while a round runs.
using InsertFn = void(__thiscall*)(void*, unsigned, void*);
InsertFn originalInsert = reinterpret_cast<InsertFn>(InsertAddress);

struct Saved { bool active = false; char original[MaxName + 1]{}; unsigned length = 0; };
struct Entry {
    void* script = nullptr;
    Saved saved[2];
    ULONGLONG lastSeen = 0;
};
std::map<unsigned, Entry> table;
SRWLOCK lock = SRWLOCK_INIT;
HANDLE worker = nullptr;

char* StringChars(unsigned char* string, unsigned& length) {
    length = *reinterpret_cast<unsigned*>(string + StringLength);
    const unsigned capacity = *reinterpret_cast<unsigned*>(string + StringCapacity);
    if (length > MaxName || capacity < length || capacity > 4096) return nullptr;
    return capacity < 16 ? reinterpret_cast<char*>(string + StringData)
                         : *reinterpret_cast<char**>(string + StringData);
}
// Overwrites a name with text of the same length, or restores the saved original.
bool WriteName(Entry& entry, int which, const char* text, unsigned size, bool restore) {
    __try {
        auto* string = static_cast<unsigned char*>(entry.script) + NameOffsets[which];
        unsigned length = 0;
        char* chars = StringChars(string, length);
        if (!chars) return false;
        auto& saved = entry.saved[which];
        if (restore) {
            if (!saved.active) return false;
            if (length == saved.length) std::memcpy(chars, saved.original, saved.length);
            saved.active = false;
            return true;
        }
        if (length != size || length == 0) return false; // Only same-length text fits safely.
        if (!saved.active) {
            std::memcpy(saved.original, chars, length);
            saved.length = length;
            saved.active = true;
        }
        std::memcpy(chars, text, length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RestoreLocked(unsigned monsterId, Entry& entry) {
    bool any = false;
    for (int which = 0; which < 2; ++which) any |= WriteName(entry, which, nullptr, 0, true);
    entry.lastSeen = 0;
    std::printf("[MonsterName] monster %u restored (%d)\n", monsterId, any);
    return any;
}
DWORD WINAPI Expire(void*) {
    for (;;) {
        Sleep(1000);
        const ULONGLONG now = GetTickCount64();
        AcquireSRWLockExclusive(&lock);
        for (auto& [id, entry] : table)
            if (entry.lastSeen && now - entry.lastSeen > ExpireMs) RestoreLocked(id, entry);
        ReleaseSRWLockExclusive(&lock);
    }
}
void Record(unsigned id, void* script) {
    AcquireSRWLockExclusive(&lock);
    try { table[id].script = script; } catch (...) {}
    ReleaseSRWLockExclusive(&lock);
}
void __fastcall Insert(void* self, void*, unsigned id, void* script) {
    if ((id >> 16) == 4 && script) Record(id, script);
    originalInsert(self, id, script);
}
}

bool MatchesReference(const unsigned char* base) {
    __try {
        if (!base) return false;
        // PUSH -1 / PUSH handler / MOV EAX,FS:[0] / PUSH EAX / MOV FS:[0],ESP / SUB ESP,0x34
        constexpr unsigned char insert[] = {0x6A,0xFF,0x68,0x18,0xA9,0xC1,0x00,0x64,0xA1,0x00,0x00,0x00,0x00,0x50,0x64,0x89,0x25,0x00,0x00,0x00,0x00,0x83,0xEC,0x34};
        return !std::memcmp(base + InsertAddress - 0x400000, insert, sizeof insert);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG Attach() {
    if (!MatchesReference(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)))) return NO_ERROR; // Feature off, nothing else affected.
    return DetourAttach(&reinterpret_cast<PVOID&>(originalInsert), reinterpret_cast<PVOID>(Insert));
}

bool Handle(std::uint32_t monsterId, std::string_view name) {
    if (name.empty() || name.size() > MaxName) return false;
    bool changed = false;
    AcquireSRWLockExclusive(&lock);
    const auto found = table.find(monsterId);
    if (found == table.end() || !found->second.script) {
        ReleaseSRWLockExclusive(&lock);
        std::printf("[MonsterName] monster %u is not in the loaded monster table\n", monsterId);
        return false;
    }
    Entry& entry = found->second;
    if (name == "=") {
        changed = RestoreLocked(monsterId, entry);
    } else {
        for (int which = 0; which < 2; ++which)
            changed |= WriteName(entry, which, name.data(), static_cast<unsigned>(name.size()), false);
        if (changed) entry.lastSeen = GetTickCount64();
        else std::printf("[MonsterName] monster %u: scrambled length does not match any name string\n", monsterId);
        if (changed && !worker) worker = CreateThread(nullptr, 0, Expire, nullptr, 0, nullptr);
    }
    ReleaseSRWLockExclusive(&lock);
    return changed;
}

#ifdef EVENT_NOTICE_TESTING
void TestRegister(std::uint32_t monsterId, void* script) { Record(monsterId, script); }
void TestExpire() {
    AcquireSRWLockExclusive(&lock);
    for (auto& [id, entry] : table) if (entry.lastSeen) entry.lastSeen = 1;
    ReleaseSRWLockExclusive(&lock);
}
#endif
}
