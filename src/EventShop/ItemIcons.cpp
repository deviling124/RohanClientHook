#include "ItemIcons.h"
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include "imgui.h"

namespace EventShopIcons {
namespace {
// Static evidence and full signature bytes: docs/evidence/event-shop-icons/.
// These globals belong only to the hash-gated supported client, never the DLL.
constexpr std::uintptr_t ItemManager = 0x00d3b07c;
constexpr std::uintptr_t ResourceManager = 0x00d3d3c4;
constexpr std::uintptr_t ResolveAddress = 0x00a08390;
using ResolveFn = void* (__thiscall*)(void*, const std::uint32_t*);
IDirect3DTexture9* frameTextures[128]{};
unsigned frameCount = 0;
std::atomic<DWORD> ownerThread{0};

template<std::size_t N>
bool Matches(std::uintptr_t address, const unsigned char (&expected)[N]) noexcept {
    unsigned char actual[N]{};
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
               actual, N, &read) && read == N && !std::memcmp(actual, expected, N);
}

bool Supported() noexcept {
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != 0x00400000) return false;
    // Complete instructions at the resolver entry, item-ID handle lookup,
    // and native ID3DXSprite::Draw texture argument. No wildcard signatures.
    static constexpr unsigned char resolver[] = {
        0x83,0xec,0x20,0x53,0x8b,0x5c,0x24,0x28,0x55,0x56,0x57,0x8b,0xf9,
        0x53,0x8d,0x44,0x24,0x38,0x8d,0x6f,0x14,0x50,0x8b,0xcd,0xe8,0x93,0xa6,0xff,0xff};
    static constexpr unsigned char itemLookup[] = {
        0x8b,0x56,0x04,0x8d,0x44,0x24,0x14,0x50,0x8b,0xcf,0x89,0x54,0x24,0x18,
        0xe8,0xe7,0xaa,0xf8,0xff,0x8b,0x10,0x8b,0x4d,0x08,0x89,0x11,
        0x8b,0x40,0x04,0x89,0x41,0x04};
    static constexpr unsigned char textureDraw[] = {
        0x8b,0x54,0x24,0x14,0x8b,0x4c,0x24,0x10,0x6a,0xff,0x56,
        0x89,0x94,0x24,0x94,0x01,0x00,0x00,0x56,0x89,0x8c,0x24,0x94,0x01,0x00,0x00,
        0x8b,0x0f,0x8d,0x94,0x24,0x8c,0x01,0x00,0x00,0x89,0xb4,0x24,0x8c,0x01,0x00,0x00,
        0x89,0xb4,0x24,0x90,0x01,0x00,0x00,0x8b,0x40,0x70,0x52,0x50,0x57,0xff,0x51,0x24};
    return Matches(ResolveAddress, resolver) && Matches(0x008b6816, itemLookup) &&
        Matches(0x00a4827d, textureDraw);
}

template<class T> T Read(const void* p, unsigned offset = 0) noexcept {
    return *reinterpret_cast<const T*>(static_cast<const unsigned char*>(p) + offset);
}

// No C++ objects requiring unwinding in this SEH boundary. The icon map is
// read only (unlike the game's operator[]); malformed/missing trees fail closed.
IDirect3DTexture9* Acquire(IDirect3DDevice9* device, unsigned type, float* u, float* v) noexcept {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DDevice9* textureDevice = nullptr;
    bool retained = false;
    __try {
        auto* manager = Read<void*>(reinterpret_cast<void*>(ItemManager));
        auto* resources = Read<void*>(reinterpret_cast<void*>(ResourceManager));
        if (!manager || !resources) return nullptr;
        auto* head = Read<void*>(manager, 0x18); // map at +0x14, head at map+4
        if (!head) return nullptr;
        auto* node = Read<void*>(head, 4);
        for (unsigned depth = 0; node && node != head && depth != 64; ++depth) {
            if (Read<unsigned char>(node, 0x19)) return nullptr;
            const auto key = Read<unsigned>(node, 0x0c);
            if (key != type) { node = Read<void*>(node, type < key ? 0 : 8); continue; }
            const std::uint32_t handle[2] = {Read<std::uint32_t>(node, 0x10), Read<std::uint32_t>(node, 0x14)};
            if (handle[0] == 0xffffffff && handle[1] == 0xffffffff) return nullptr;
            auto* resource = reinterpret_cast<ResolveFn>(ResolveAddress)(resources, handle);
            if (!resource) return nullptr;
            texture = Read<IDirect3DTexture9*>(resource, 0x70);
            const auto width = Read<unsigned>(resource, 0x68);
            const auto height = Read<unsigned>(resource, 0x6c);
            if (!texture || !width || !height || width > 4096 || height > 4096) return nullptr;
            if (FAILED(texture->GetDevice(&textureDevice)) || !textureDevice) return nullptr;
            const bool sameDevice = textureDevice == device;
            textureDevice->Release(); textureDevice = nullptr;
            if (!sameDevice) return nullptr;
            D3DSURFACE_DESC desc{};
            if (FAILED(texture->GetLevelDesc(0, &desc)) || desc.Width < width || desc.Height < height) return nullptr;
            texture->AddRef(); retained = true;
            *u = static_cast<float>(width) / desc.Width;
            *v = static_cast<float>(height) / desc.Height;
            return texture;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Native state can be unavailable during a login/zone transition.
        // Never substitute a guessed icon or allow an access violation to escape.
    }
    // The normal path reaches here without acquiring either COM reference.
    // Exceptions after acquiring one are extremely rare; release only known refs.
    __try {
        if (textureDevice) textureDevice->Release();
        if (retained) texture->Release();
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
}

void Draw(IDirect3DDevice9* device, int itemType, float size) {
    if (size <= 0) return;
    static const bool supported = Supported();
    const DWORD thread = GetCurrentThreadId();
    DWORD unset = 0;
    ownerThread.compare_exchange_strong(unset, thread);
    float u = 1, v = 1;
    auto* texture = supported && device && itemType > 0 && ownerThread.load() == thread && frameCount < 128
        ? Acquire(device, static_cast<unsigned>(itemType), &u, &v) : nullptr;
    if (texture) {
        frameTextures[frameCount++] = texture;
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)),
                     ImVec2(size,size), ImVec2(0,0), ImVec2(u,v));
    } else {
        const auto pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size,size));
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, ImVec2(pos.x+size,pos.y+size), IM_COL32(35,39,45,255), 4);
        draw->AddRect(pos, ImVec2(pos.x+size,pos.y+size), IM_COL32(78,85,96,255), 4);
        const auto label = ImGui::CalcTextSize("?");
        draw->AddText(ImVec2(pos.x+(size-label.x)*.5f,pos.y+(size-label.y)*.5f), IM_COL32(160,168,180,255), "?");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Item %d\nIcon unavailable", itemType);
    }
}

void ReleaseFrame() noexcept {
    // D3D detours also see secondary devices. Their thread must not touch
    // references currently being consumed by the owning ImGui renderer.
    if (GetCurrentThreadId() != ownerThread.load()) return;
    for (unsigned i=0; i<frameCount; ++i) {
        frameTextures[i]->Release();
        frameTextures[i] = nullptr;
    }
    frameCount = 0;
}
}
