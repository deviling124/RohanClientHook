#pragma once
struct IDirect3DDevice9;

namespace EventShopIcons {
// Call on the game's existing render/window thread, inside the overlay's
// saved DX9 state scope. The preview and unsupported images show a placeholder.
void Draw(IDirect3DDevice9* device, int itemType, float size);
// Release temporary texture references after ImGui draw data is consumed,
// including failed/aborted frames, and before Reset/shutdown.
void ReleaseFrame() noexcept;
}
