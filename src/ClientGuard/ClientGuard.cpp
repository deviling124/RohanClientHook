// Detour layer for the client side of the encrypted channel. Hook sites and structures are
// documented in docs/TRANSPORT_MAPPING.md ("Client receive chain", "Client send chain").
//
//   OnRecvBytes 0x00aa8d70  __thiscall(conn, ctx, bytes) ret 8   worker thread: remembers the connection
//   ReadFrame   0x00aa9b20  __thiscall(ring, Frame*)     ret 4   worker thread: returns one framed packet
//   PushSend    0x00aa8b50  __thiscall(conn, Frame*)     ret 4   under the connection critical section
//   SendLocked  0x00aa8bc0  __thiscall(conn, Frame*)     ret 4   used to push our own frames
//
// Frame = { u16 opcode; u16 length; u8* body }. Before ReadFrame, `length` holds the maximum body
// size; after a successful call it holds the real length. Nothing here is active unless
// novaclient.ini says [client_guard] mode=audit|enforce and a server key is pinned.
#include "ClientGuard.h"
#include "Engine.h"
#include "PinnedKeys.h"
#include "detours.h"
#include <cstdio>
#include <cstring>
#include <memory>

namespace ClientGuard {
namespace {
struct Frame {
    std::uint16_t opcode;
    std::uint16_t length;
    std::uint8_t* body;
};
static_assert(sizeof(Frame) == 8 || sizeof(void*) != 4, "Frame must be 8 bytes on the 32-bit client");

using ReadFrameFn = int(__thiscall*)(void* ring, Frame* out);
using PushSendFn = int(__thiscall*)(void* conn, Frame* in);
using SendLockedFn = int(__thiscall*)(void* conn, Frame* in);
using OnRecvBytesFn = int(__thiscall*)(void* conn, void* ctx, int bytes);

constexpr std::uintptr_t ReadFrameAddress = 0x00aa9b20;
constexpr std::uintptr_t PushSendAddress = 0x00aa8b50;
constexpr std::uintptr_t SendLockedAddress = 0x00aa8bc0;
constexpr std::uintptr_t OnRecvBytesAddress = 0x00aa8d70;
constexpr std::uintptr_t ImageBase = 0x00400000;

ReadFrameFn originalReadFrame = reinterpret_cast<ReadFrameFn>(ReadFrameAddress);
PushSendFn originalPushSend = reinterpret_cast<PushSendFn>(PushSendAddress);
SendLockedFn originalSendLocked = reinterpret_cast<SendLockedFn>(SendLockedAddress);
OnRecvBytesFn originalOnRecvBytes = reinterpret_cast<OnRecvBytesFn>(OnRecvBytesAddress);

std::unique_ptr<Engine> engine;
thread_local void* currentConnection = nullptr;
char statusText[160] = "client guard: not initialized";

// PushSend results: 2, 3 and 4 mean the frame was queued or posted. 0 is "not connected",
// 1 "send ring full", -1 an error.
bool Queued(int result) { return result == 2 || result == 3 || result == 4; }

int __fastcall OnRecvBytesHook(void* conn, void*, void* ctx, int bytes) {
    void* const previous = currentConnection;
    currentConnection = conn;
    const int result = originalOnRecvBytes(conn, ctx, bytes);
    currentConnection = previous;
    return result;
}

int __fastcall ReadFrameHook(void* ring, void*, Frame* out) {
    // The caller stores the maximum body size in `length` before every call and ReadFrame
    // overwrites it with the real length, so restore it before reading the next frame.
    const std::uint16_t maxLength = out->length;
    for (;;) {
        out->length = maxLength;
        const int result = originalReadFrame(ring, out);
        if (result != 0 || !engine || !currentConnection) return result;
        NovaChannel::Bytes replacement;
        switch (engine->OnInbound(currentConnection, out->opcode, out->body, out->length, replacement)) {
        case Engine::Inbound::Pass:
            return 0;
        case Engine::Inbound::Consume:
            continue; // our own handshake frame: read the next one
        case Engine::Inbound::Replace:
            if (!replacement.empty()) std::memcpy(out->body, replacement.data(), replacement.size());
            out->length = static_cast<std::uint16_t>(replacement.size());
            return 0;
        case Engine::Inbound::Fail:
            return -3; // the receive loop treats this as a bad frame and drops the connection
        }
        return -3;
    }
}

int __fastcall PushSendHook(void* conn, void*, Frame* in) {
    if (engine) {
        NovaChannel::Bytes replacement;
        switch (engine->OnOutbound(conn, in->opcode, in->body, in->length, replacement)) {
        case Engine::Outbound::Pass:
            break;
        case Engine::Outbound::Replace: {
            Frame sealed{in->opcode, static_cast<std::uint16_t>(replacement.size()),
                         replacement.empty() ? in->body : replacement.data()};
            return originalPushSend(conn, &sealed);
        }
        case Engine::Outbound::Refuse:
            return 1;
        }
    }
    return originalPushSend(conn, in);
}

bool BytesAre(const unsigned char* base, std::uintptr_t address, const unsigned char* expected, std::size_t n) {
    return std::memcmp(base + (address - ImageBase), expected, n) == 0;
}

void Configure(Mode mode, std::vector<NovaChannel::PublicKey> pinned) {
    Config config;
    config.mode = mode;
    config.pinned = std::move(pinned);
    engine = std::make_unique<Engine>(
        std::move(config),
        [](void* conn, std::uint16_t opcode, const std::uint8_t* body, std::size_t n) {
            Frame frame{opcode, static_cast<std::uint16_t>(n), const_cast<std::uint8_t*>(body)};
            return Queued(originalSendLocked(conn, &frame));
        },
        [](const char* text) { OutputDebugStringA(text); OutputDebugStringA("\r\n"); });
}

Mode ReadConfiguredMode() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ReadConfiguredMode), &self) ||
        !GetModuleFileNameW(self, path, MAX_PATH)) return Mode::Disabled;
    wchar_t* separator = wcsrchr(path, L'\\');
    if (!separator || static_cast<std::size_t>(separator - path) + 16 >= MAX_PATH) return Mode::Disabled;
    wcscpy_s(separator + 1, MAX_PATH - (separator + 1 - path), L"novaclient.ini");
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"client_guard", L"mode", L"disabled", value, 16, path);
    char narrow[16]{};
    for (int i = 0; i < 15 && value[i]; ++i) narrow[i] = value[i] < 128 ? static_cast<char>(value[i]) : '?';
    return ParseMode(narrow);
}
} // namespace

bool MatchesReference(const unsigned char* base) noexcept {
    static constexpr unsigned char readFrame[] = {0x83, 0xec, 0x08, 0x8b, 0xc1, 0x53, 0x8b, 0x58, 0x18, 0x83, 0xfb, 0x04};
    static constexpr unsigned char pushSend[] = {0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x30, 0x02, 0x74, 0x06, 0x33, 0xc0, 0x5e};
    static constexpr unsigned char sendLocked[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x08, 0x57, 0x83, 0xc0, 0x30, 0x50, 0xff};
    static constexpr unsigned char onRecvBytes[] = {0x83, 0xec, 0x0c, 0x53, 0x55, 0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x38, 0x57};
    __try {
        return BytesAre(base, ReadFrameAddress, readFrame, sizeof readFrame) &&
               BytesAre(base, PushSendAddress, pushSend, sizeof pushSend) &&
               BytesAre(base, SendLockedAddress, sendLocked, sizeof sendLocked) &&
               BytesAre(base, OnRecvBytesAddress, onRecvBytes, sizeof onRecvBytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* StatusText() { return statusText; }

LONG Attach() {
    const Mode mode = ReadConfiguredMode();
    if (mode == Mode::Disabled) {
        std::snprintf(statusText, sizeof statusText, "client guard: disabled (no hooks installed)");
        return NO_ERROR;
    }
    if (PinnedKeyCount == 0) {
        std::snprintf(statusText, sizeof statusText,
                      "client guard: mode=%s requested but no server key is pinned; disabled", ModeName(mode));
        return NO_ERROR;
    }
    std::vector<NovaChannel::PublicKey> pinned(PinnedKeys, PinnedKeys + PinnedKeyCount);
    Configure(mode, std::move(pinned));
    LONG result = DetourAttach(&reinterpret_cast<PVOID&>(originalOnRecvBytes), reinterpret_cast<PVOID>(OnRecvBytesHook));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalReadFrame), reinterpret_cast<PVOID>(ReadFrameHook));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalPushSend), reinterpret_cast<PVOID>(PushSendHook));
    std::snprintf(statusText, sizeof statusText, "client guard: mode=%s, %s", ModeName(mode),
                  result == NO_ERROR ? "hooks attached" : "hook attach failed");
    return result;
}

#ifdef CLIENT_GUARD_TESTING
namespace Test {
void Install(Mode mode, std::vector<NovaChannel::PublicKey> pinned, ReadFrameFn read, PushSendFn push,
             SendLockedFn locked, OnRecvBytesFn recv) {
    originalReadFrame = read;
    originalPushSend = push;
    originalSendLocked = locked;
    originalOnRecvBytes = recv;
    Configure(mode, std::move(pinned));
}
void Reset() { engine.reset(); currentConnection = nullptr; }
int Recv(void* conn, void* ctx, int bytes) { return OnRecvBytesHook(conn, nullptr, ctx, bytes); }
int Read(void* ring, Frame* out) { return ReadFrameHook(ring, nullptr, out); }
int Push(void* conn, Frame* in) { return PushSendHook(conn, nullptr, in); }
}
#endif
} // namespace ClientGuard
