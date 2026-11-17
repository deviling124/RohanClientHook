#pragma once
#include <d3d9.h>
#include <array>

// PURE devices cannot report the transforms/state required by an injected UI.
// Keep all other requested device flags intact.
inline DWORD Dx9CompatibleDeviceFlags(DWORD requested) {return requested & ~D3DCREATE_PUREDEVICE;}

// One overlay scene, bound only to the primary backbuffer. Captures BEFORE any
// UI/backend work. Failed capture never permits drawing; Finish also runs on
// scope exit, including partial setup failures and C++ exceptions.
class Dx9Frame final {
public:
    explicit Dx9Frame(IDirect3DDevice9* device):device_(device) {}
    ~Dx9Frame() {Finish();}
    Dx9Frame(const Dx9Frame&)=delete;
    Dx9Frame& operator=(const Dx9Frame&)=delete;
    bool Begin();
    bool Finish();
private:
    bool Capture();
    void ReleaseReferences();
    IDirect3DDevice9* device_=nullptr;
    IDirect3DStateBlock9* state_=nullptr;
    std::array<IDirect3DSurface9*,4> targets_{};
    IDirect3DSurface9* depth_=nullptr;
    IDirect3DSurface9* backbuffer_=nullptr;
    IDirect3DVertexShader9* vertexShader_=nullptr;
    IDirect3DPixelShader9* pixelShader_=nullptr;
    IDirect3DVertexDeclaration9* declaration_=nullptr;
    IDirect3DVertexBuffer9* vertices_=nullptr;
    IDirect3DIndexBuffer9* indices_=nullptr;
    D3DMATRIX world_{},view_{},projection_{};
    D3DVIEWPORT9 viewport_{};
    RECT scissor_{};
    unsigned targetCount_=0;
    UINT streamOffset_=0,streamStride_=0,streamFrequency_=1;
    DWORD fvf_=0;
    bool attempted_=false,changed_=false,scene_=false,finished_=false,restoreOk_=true;
};
