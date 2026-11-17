#include "Dx9Frame.h"

namespace {
template<class T> void Release(T*& value) {if(value) {value->Release();value=nullptr;}}
}
bool Dx9Frame::Capture() {
    D3DCAPS9 caps{};
    if(!device_ || FAILED(device_->GetDeviceCaps(&caps)) || !caps.NumSimultaneousRTs || caps.NumSimultaneousRTs>targets_.size()) return false;
    targetCount_=caps.NumSimultaneousRTs;
    if(FAILED(device_->CreateStateBlock(D3DSBT_ALL,&state_)) || !state_ || FAILED(state_->Capture())) return false;
    // Unlike the old backend-only path, every state query must succeed. In
    // particular GetTransform fails on a PURE device: never restore garbage.
    if(FAILED(device_->GetTransform(D3DTS_WORLD,&world_)) ||
       FAILED(device_->GetTransform(D3DTS_VIEW,&view_)) ||
       FAILED(device_->GetTransform(D3DTS_PROJECTION,&projection_)) ||
       FAILED(device_->GetViewport(&viewport_)) || FAILED(device_->GetScissorRect(&scissor_)) ||
       FAILED(device_->GetFVF(&fvf_)) || FAILED(device_->GetVertexDeclaration(&declaration_)) ||
       FAILED(device_->GetVertexShader(&vertexShader_)) || FAILED(device_->GetPixelShader(&pixelShader_)) ||
       FAILED(device_->GetStreamSource(0,&vertices_,&streamOffset_,&streamStride_)) ||
       FAILED(device_->GetStreamSourceFreq(0,&streamFrequency_)) || FAILED(device_->GetIndices(&indices_))) return false;
    for(unsigned i=0;i<targetCount_;++i) {
        const HRESULT result=device_->GetRenderTarget(i,&targets_[i]);
        if(i==0 && (FAILED(result)||!targets_[i])) return false;
        if(i>0 && FAILED(result) && result!=D3DERR_NOTFOUND) return false;
    }
    const HRESULT depthResult=device_->GetDepthStencilSurface(&depth_);
    if(FAILED(depthResult) && depthResult!=D3DERR_NOTFOUND) return false;
    return SUCCEEDED(device_->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer_)) && backbuffer_;
}
bool Dx9Frame::Begin() {
    if(attempted_) return false;
    attempted_=true;
    if(!Capture()) {Finish();return false;}
    D3DSURFACE_DESC description{};
    if(FAILED(backbuffer_->GetDesc(&description))) {Finish();return false;}
    changed_=true;
    bool ok=SUCCEEDED(device_->SetDepthStencilSurface(nullptr));
    for(unsigned i=1;i<targetCount_;++i) if(FAILED(device_->SetRenderTarget(i,nullptr))) ok=false;
    if(FAILED(device_->SetRenderTarget(0,backbuffer_))) ok=false;
    const D3DVIEWPORT9 viewport{0,0,description.Width,description.Height,0.f,1.f};
    if(FAILED(device_->SetViewport(&viewport)) || FAILED(device_->SetStreamSourceFreq(0,1))) ok=false;
    // The stock backend assumes these FFP defaults, but a legacy engine can
    // leave texture coordinate transforms, clip planes or color masks enabled.
    if(FAILED(device_->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0))) ok=false;
    if(FAILED(device_->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE))) ok=false;
    if(FAILED(device_->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_ALPHA))) ok=false;
    if(FAILED(device_->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE))) ok=false;
    if(FAILED(device_->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE))) ok=false;
    if(FAILED(device_->SetRenderState(D3DRS_CLIPPLANEENABLE,0))) ok=false;
    if(FAILED(device_->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_DISABLE))) ok=false;
    if(FAILED(device_->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE))) ok=false;
    if(!ok || FAILED(device_->BeginScene())) {Finish();return false;}
    scene_=true;return true;
}
bool Dx9Frame::Finish() {
    if(finished_) return restoreOk_;
    finished_=true;
    if(scene_) {
        if(FAILED(device_->EndScene())) restoreOk_=false;
        scene_=false;
    }
    if(changed_) {
        // SetRenderTarget resets viewport; restore surfaces first, then captured
        // state and explicit query-backed states. All refs stay alive until done.
        if(FAILED(device_->SetDepthStencilSurface(nullptr))) restoreOk_=false;
        for(unsigned i=1;i<targetCount_;++i) if(FAILED(device_->SetRenderTarget(i,nullptr))) restoreOk_=false;
        if(FAILED(device_->SetRenderTarget(0,targets_[0]))) restoreOk_=false;
        for(unsigned i=1;i<targetCount_;++i) if(FAILED(device_->SetRenderTarget(i,targets_[i]))) restoreOk_=false;
        if(FAILED(device_->SetDepthStencilSurface(depth_))) restoreOk_=false;
        if(FAILED(state_->Apply())) restoreOk_=false;
        const auto restore=[&](HRESULT result) {if(FAILED(result)) restoreOk_=false;};
        restore(device_->SetTransform(D3DTS_WORLD,&world_));
        restore(device_->SetTransform(D3DTS_VIEW,&view_));
        restore(device_->SetTransform(D3DTS_PROJECTION,&projection_));
        restore(device_->SetVertexShader(vertexShader_));restore(device_->SetPixelShader(pixelShader_));
        // GetFVF can retain its previous value after a custom declaration was
        // installed. SetFVF creates/replaces a declaration, so restore it first
        // and the exact original declaration last (shader-based host pipelines).
        if(fvf_) restore(device_->SetFVF(fvf_));
        restore(device_->SetVertexDeclaration(declaration_));
        restore(device_->SetStreamSource(0,vertices_,streamOffset_,streamStride_));
        restore(device_->SetStreamSourceFreq(0,streamFrequency_));restore(device_->SetIndices(indices_));
        restore(device_->SetViewport(&viewport_));restore(device_->SetScissorRect(&scissor_));
    }
    ReleaseReferences();return restoreOk_;
}
void Dx9Frame::ReleaseReferences() {
    Release(state_);for(auto& target:targets_) Release(target);
    Release(depth_);Release(backbuffer_);Release(vertexShader_);Release(pixelShader_);
    Release(declaration_);Release(vertices_);Release(indices_);
}
