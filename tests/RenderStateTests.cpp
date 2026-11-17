// Actual Direct3D 9 integration tests. Owns a hidden test window/device only.
// Never launches, attaches to, or modifies the game client.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/Dx9Frame.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

namespace {
unsigned checks=0,skips=0;
void Check(bool ok,const char* label) {
    ++checks;
    if(!ok) {std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}
}
void Hr(HRESULT result,const char* label) {
    if(FAILED(result)) std::fprintf(stderr,"HRESULT for %s: 0x%08lx\n",label,static_cast<unsigned long>(result));
    Check(SUCCEEDED(result),label);
}
void Skip(const char* reason) {++skips;std::printf("SKIP: %s\n",reason);}
template<class T> struct Com {
    T* p=nullptr;
    Com()=default;
    ~Com() {if(p) p->Release();}
    Com(const Com&)=delete;
    Com& operator=(const Com&)=delete;
    T** Out() {Check(!p,"COM output is initially empty");return &p;}
    T* operator->() const {return p;}
};
constexpr D3DRENDERSTATETYPE RenderStates[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,
    D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_ALPHATESTENABLE,D3DRS_CULLMODE,D3DRS_SCISSORTESTENABLE,
    D3DRS_COLORWRITEENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_LIGHTING,
    D3DRS_FOGENABLE,D3DRS_CLIPPING,D3DRS_STENCILENABLE,D3DRS_TEXTUREFACTOR};
constexpr D3DSAMPLERSTATETYPE SamplerStates[]={D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_MINFILTER,
    D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_SRGBTEXTURE};
constexpr D3DTEXTURESTAGESTATETYPE TextureStates[]={D3DTSS_COLOROP,D3DTSS_COLORARG1,D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP,D3DTSS_TEXCOORDINDEX,D3DTSS_TEXTURETRANSFORMFLAGS};
constexpr D3DTRANSFORMSTATETYPE Transforms[]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION,D3DTS_TEXTURE0};

struct State {
    std::array<Com<IDirect3DSurface9>,4> targets;
    Com<IDirect3DSurface9> depth;
    Com<IDirect3DVertexShader9> vertex;
    Com<IDirect3DPixelShader9> pixel;
    Com<IDirect3DVertexDeclaration9> declaration;
    Com<IDirect3DVertexBuffer9> stream;
    Com<IDirect3DIndexBuffer9> indices;
    std::array<Com<IDirect3DBaseTexture9>,2> textures;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    std::array<D3DMATRIX,4> transforms{};
    std::array<DWORD,std::size(RenderStates)> render{};
    std::array<std::array<DWORD,std::size(SamplerStates)>,2> samplers{};
    std::array<std::array<DWORD,std::size(TextureStates)>,2> textureStages{};
    std::array<float,4> vertexConstant{},pixelConstant{};
    DWORD fvf=0;
    UINT frequency=0;
    UINT offset=0,stride=0,targetCount=0;
    void Capture(IDirect3DDevice9* device,unsigned availableTargets) {
        targetCount=(std::min)(availableTargets,4u);
        for(unsigned i=0;i<targetCount;++i) {
            const auto hr=device->GetRenderTarget(i,targets[i].Out());
            Check(SUCCEEDED(hr) || hr==D3DERR_NOTFOUND,"capture render target binding");
        }
        const auto depthResult=device->GetDepthStencilSurface(depth.Out());
        Check(SUCCEEDED(depthResult) || depthResult==D3DERR_NOTFOUND,"capture depth binding");
        Hr(device->GetViewport(&viewport),"capture viewport");
        Hr(device->GetScissorRect(&scissor),"capture scissor");
        for(unsigned i=0;i<transforms.size();++i) Hr(device->GetTransform(Transforms[i],&transforms[i]),"capture transform");
        Hr(device->GetVertexShader(vertex.Out()),"capture vertex shader");
        Hr(device->GetPixelShader(pixel.Out()),"capture pixel shader");
        Hr(device->GetVertexDeclaration(declaration.Out()),"capture vertex declaration");
        Hr(device->GetFVF(&fvf),"capture FVF");
        Hr(device->GetStreamSource(0,stream.Out(),&offset,&stride),"capture stream source");
        Hr(device->GetStreamSourceFreq(0,&frequency),"capture stream frequency");
        Hr(device->GetIndices(indices.Out()),"capture index buffer");
        Hr(device->GetVertexShaderConstantF(3,vertexConstant.data(),1),"capture vertex constant");
        Hr(device->GetPixelShaderConstantF(5,pixelConstant.data(),1),"capture pixel constant");
        for(unsigned i=0;i<std::size(RenderStates);++i) Hr(device->GetRenderState(RenderStates[i],&render[i]),"capture render state");
        for(unsigned stage=0;stage<2;++stage) {
            Hr(device->GetTexture(stage,textures[stage].Out()),"capture texture binding");
            for(unsigned i=0;i<std::size(SamplerStates);++i) Hr(device->GetSamplerState(stage,SamplerStates[i],&samplers[stage][i]),"capture sampler state");
            for(unsigned i=0;i<std::size(TextureStates);++i) Hr(device->GetTextureStageState(stage,TextureStates[i],&textureStages[stage][i]),"capture texture stage state");
        }
    }
    void SameAs(const State& other) const {
        Check(targetCount==other.targetCount,"render target count preserved");
        for(unsigned i=0;i<targetCount;++i) Check(targets[i].p==other.targets[i].p,"render target pointer restored exactly");
        Check(depth.p==other.depth.p,"depth surface restored exactly");
        Check(std::memcmp(&viewport,&other.viewport,sizeof viewport)==0,"viewport restored exactly");
        Check(std::memcmp(&scissor,&other.scissor,sizeof scissor)==0,"scissor restored exactly");
        Check(std::memcmp(transforms.data(),other.transforms.data(),sizeof transforms)==0,"world/view/projection/texture transforms restored exactly");
        Check(vertex.p==other.vertex.p,"vertex shader restored exactly");
        Check(pixel.p==other.pixel.p,"pixel shader restored exactly");
        if(declaration.p!=other.declaration.p) {
            std::fprintf(stderr,"Declaration mismatch: before=%p after=%p fvf-before=%08lx fvf-after=%08lx\n",declaration.p,other.declaration.p,static_cast<unsigned long>(fvf),static_cast<unsigned long>(other.fvf));
            D3DVERTEXELEMENT9 left[MAXD3DDECLLENGTH+1]{},right[MAXD3DDECLLENGTH+1]{};UINT nl=MAXD3DDECLLENGTH+1,nr=MAXD3DDECLLENGTH+1;
            const HRESULT hl=declaration.p?declaration->GetDeclaration(left,&nl):E_FAIL;
            const HRESULT hr=other.declaration.p?other.declaration->GetDeclaration(right,&nr):E_FAIL;
            std::fprintf(stderr,"Declaration contents: before-hr=%08lx after-hr=%08lx before-count=%u after-count=%u equal=%d\n",static_cast<unsigned long>(hl),static_cast<unsigned long>(hr),nl,nr,nl==nr && std::memcmp(left,right,nl*sizeof(D3DVERTEXELEMENT9))==0);
        }
        Check(declaration.p==other.declaration.p,"vertex declaration restored exactly");
        Check(fvf==other.fvf,"FVF restored exactly");
        Check(stream.p==other.stream.p && offset==other.offset && stride==other.stride,"stream source/offset/stride restored exactly");
        Check(frequency==other.frequency,"stream source frequency restored exactly");
        Check(indices.p==other.indices.p,"index buffer restored exactly");
        Check(vertexConstant==other.vertexConstant,"vertex shader constant restored exactly");
        Check(pixelConstant==other.pixelConstant,"pixel shader constant restored exactly");
        Check(render==other.render,"render states restored exactly");
        Check(samplers==other.samplers,"sampler states restored exactly");
        Check(textureStages==other.textureStages,"texture stage states restored exactly");
        for(unsigned i=0;i<textures.size();++i) Check(textures[i].p==other.textures[i].p,"texture binding restored exactly");
    }
};

std::uint64_t SurfaceHash(IDirect3DDevice9* device,IDirect3DSurface9* surface) {
    D3DSURFACE_DESC desc{};Hr(surface->GetDesc(&desc),"read render surface description");
    Check(desc.Format==D3DFMT_A8R8G8B8 || desc.Format==D3DFMT_X8R8G8B8,"readback surface is 32-bit RGB");
    Com<IDirect3DSurface9> readback;
    Hr(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,readback.Out(),nullptr),"create GPU readback surface");
    Hr(device->GetRenderTargetData(surface,readback.p),"read GPU render target pixels");
    D3DLOCKED_RECT locked{};Hr(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"lock GPU readback");
    std::uint64_t hash=14695981039346656037ull;
    for(UINT y=0;y<desc.Height;++y) {
        const auto* row=static_cast<const unsigned char*>(locked.pBits)+y*locked.Pitch;
        for(UINT x=0;x<desc.Width*4;++x) {hash^=row[x];hash*=1099511628211ull;}
    }
    Hr(readback->UnlockRect(),"unlock GPU readback");return hash;
}
struct HostResources {
    Com<IDirect3DTexture9> targetTexture,secondTargetTexture,texture0,texture1;
    Com<IDirect3DSurface9> target,secondTarget,depth,backbuffer;
    Com<IDirect3DVertexBuffer9> stream;
    Com<IDirect3DIndexBuffer9> indices;
    Com<IDirect3DVertexDeclaration9> declaration;
    Com<IDirect3DVertexShader9> vertex;
    Com<IDirect3DPixelShader9> pixel;
    bool mrt=false,shaders=false;
    void Create(IDirect3DDevice9* device,const D3DCAPS9& caps) {
        Hr(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.Out()),"get test device backbuffer");
        Hr(device->CreateTexture(256,256,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,targetTexture.Out(),nullptr),"create offscreen render texture");
        Hr(targetTexture->GetSurfaceLevel(0,target.Out()),"get offscreen render surface");
        Hr(device->ColorFill(target.p,nullptr,D3DCOLOR_ARGB(255,17,83,139)),"fill offscreen sentinel pixels");
        if(caps.NumSimultaneousRTs>1) {
            const auto result=device->CreateTexture(256,256,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,secondTargetTexture.Out(),nullptr);
            if(SUCCEEDED(result)) {
                Hr(secondTargetTexture->GetSurfaceLevel(0,secondTarget.Out()),"get secondary render surface");
                Hr(device->ColorFill(secondTarget.p,nullptr,D3DCOLOR_ARGB(255,71,19,121)),"fill secondary sentinel pixels");mrt=true;
            } else Skip("driver cannot create a secondary offscreen render texture");
        } else Skip("multiple render targets unsupported on this test device");
        auto depthResult=device->CreateDepthStencilSurface(256,256,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,depth.Out(),nullptr);
        if(FAILED(depthResult)) depthResult=device->CreateDepthStencilSurface(256,256,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,depth.Out(),nullptr);
        Hr(depthResult,"create offscreen depth surface");
        Hr(device->CreateTexture(4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture0.Out(),nullptr),"create host texture zero");
        Hr(device->CreateTexture(4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture1.Out(),nullptr),"create host texture one");
        Hr(device->CreateVertexBuffer(256,0,D3DFVF_XYZ|D3DFVF_DIFFUSE,D3DPOOL_MANAGED,stream.Out(),nullptr),"create host vertex buffer");
        Hr(device->CreateIndexBuffer(64,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,indices.Out(),nullptr),"create host index buffer");
        const D3DVERTEXELEMENT9 elements[]={
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
        Hr(device->CreateVertexDeclaration(elements,declaration.Out()),"create explicit host declaration");
        if(caps.VertexShaderVersion>=D3DVS_VERSION(2,0) && caps.PixelShaderVersion>=D3DPS_VERSION(2,0)) {
            const char vs[]="struct I {float4 p:POSITION0;float4 c:COLOR0;};struct O {float4 p:POSITION0;float4 c:COLOR0;};O main(I i){O o;o.p=i.p;o.c=i.c;return o;}";
            const char ps[]="float4 main(float4 color:COLOR0):COLOR0{return color;}";
            Com<ID3DBlob> vsCode,psCode,error;
            Hr(D3DCompile(vs,sizeof vs-1,"host-vs",nullptr,nullptr,"main","vs_2_0",0,0,vsCode.Out(),error.Out()),"compile host vertex shader");
            if(error.p) {error.p->Release();error.p=nullptr;}
            Hr(D3DCompile(ps,sizeof ps-1,"host-ps",nullptr,nullptr,"main","ps_2_0",0,0,psCode.Out(),error.Out()),"compile host pixel shader");
            Hr(device->CreateVertexShader(static_cast<const DWORD*>(vsCode->GetBufferPointer()),vertex.Out()),"create host vertex shader");
            Hr(device->CreatePixelShader(static_cast<const DWORD*>(psCode->GetBufferPointer()),pixel.Out()),"create host pixel shader");shaders=true;
        } else Skip("shader-model 2 unsupported; fixed-function preservation still tested");
    }
    void Bind(IDirect3DDevice9* device,const D3DCAPS9& caps,bool useShaders) {
        Hr(device->SetDepthStencilSurface(nullptr),"unbind depth before target switch");
        for(unsigned i=1;i<(std::min<DWORD>)(caps.NumSimultaneousRTs,4u);++i) Hr(device->SetRenderTarget(i,nullptr),"clear extra render target");
        Hr(device->SetRenderTarget(0,target.p),"bind host offscreen target");
        if(mrt) Hr(device->SetRenderTarget(1,secondTarget.p),"bind host second offscreen target");
        Hr(device->SetDepthStencilSurface(depth.p),"bind host offscreen depth");
        D3DVIEWPORT9 viewport={11,17,193,181,.125f,.875f};Hr(device->SetViewport(&viewport),"set nondefault host viewport");
        RECT scissor={19,29,143,151};Hr(device->SetScissorRect(&scissor),"set nondefault host scissor");
        for(unsigned i=0;i<std::size(Transforms);++i) {
            D3DMATRIX matrix{};matrix._11=1.f+i;matrix._22=2.f+i;matrix._33=3.f+i;matrix._44=1.f;
            matrix._41=.25f+i;matrix._42=-.5f-i;matrix._43=.75f;
            Hr(device->SetTransform(Transforms[i],&matrix),"set nondefault host transform");
        }
        Hr(device->SetVertexShader(useShaders?vertex.p:nullptr),"bind host vertex shader mode");
        Hr(device->SetPixelShader(useShaders?pixel.p:nullptr),"bind host pixel shader mode");
        if(useShaders) Hr(device->SetVertexDeclaration(declaration.p),"bind explicit host vertex declaration");
        else Hr(device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE),"bind host fixed-function FVF");
        Hr(device->SetStreamSource(0,stream.p,16,16),"bind nondefault host stream");
        Hr(device->SetStreamSourceFreq(0,1),"set host stream frequency");
        Hr(device->SetIndices(indices.p),"bind host indices");
        const float vc[]={.125f,.25f,.5f,.75f},pc[]={.9f,.8f,.7f,.6f};
        Hr(device->SetVertexShaderConstantF(3,vc,1),"set host vertex shader constant");
        Hr(device->SetPixelShaderConstantF(5,pc,1),"set host pixel shader constant");
        const DWORD values[]={D3DZB_TRUE,TRUE,FALSE,D3DBLEND_ONE,D3DBLEND_ZERO,TRUE,D3DCULL_CW,TRUE,
            D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN,TRUE,TRUE,TRUE,TRUE,FALSE,TRUE,0x12345678};
        static_assert(std::size(values)==std::size(RenderStates),"host render state table size");
        for(unsigned i=0;i<std::size(RenderStates);++i) Hr(device->SetRenderState(RenderStates[i],values[i]),"set nondefault host render state");
        const DWORD samplerValues[]={D3DTADDRESS_MIRROR,D3DTADDRESS_CLAMP,D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_NONE,TRUE};
        const DWORD stageValues[]={D3DTOP_ADD,D3DTA_TFACTOR,D3DTA_CURRENT,D3DTOP_SELECTARG2,1,D3DTTFF_COUNT2};
        for(unsigned stage=0;stage<2;++stage) {
            Hr(device->SetTexture(stage,stage?texture1.p:texture0.p),"bind host texture");
            for(unsigned i=0;i<std::size(SamplerStates);++i) Hr(device->SetSamplerState(stage,SamplerStates[i],samplerValues[i]),"set nondefault host sampler state");
            for(unsigned i=0;i<std::size(TextureStates);++i) Hr(device->SetTextureStageState(stage,TextureStates[i],stageValues[i]),"set nondefault host texture stage");
        }
    }
};
void DrawOverlay() {
    ImGui_ImplDX9_NewFrame();ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(5,5));ImGui::SetNextWindowSize(ImVec2(230,130));
    ImGui::Begin("AHK render-state regression",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
    ImGui::TextUnformatted("Actual ImGui DX9 geometry");ImGui::Button("State preservation");ImGui::End();
    ImGui::Render();Check(ImGui::GetDrawData()->TotalVtxCount>0,"overlay generated actual vertices");
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}
void Compare(IDirect3DDevice9* device,const D3DCAPS9& caps,const State& before) {
    State after;after.Capture(device,caps.NumSimultaneousRTs);before.SameAs(after);
}
void Exercise(IDirect3DDevice9* device,const D3DCAPS9& caps,HostResources& host,bool shaderMode) {
    host.Bind(device,caps,shaderMode);
    Hr(device->ColorFill(host.backbuffer.p,nullptr,D3DCOLOR_ARGB(255,9,23,37)),"fill test backbuffer");
    const auto offscreen=SurfaceHash(device,host.target.p);
    const auto second=host.mrt?SurfaceHash(device,host.secondTarget.p):0;
    const auto backbuffer=SurfaceHash(device,host.backbuffer.p);
    State before;before.Capture(device,caps.NumSimultaneousRTs);
    { State control;control.Capture(device,caps.NumSimultaneousRTs);before.SameAs(control); }
    std::printf("START: %s; host-decl=%p queried-decl=%p fvf=%08lx\n",shaderMode?"shader":"FVF",host.declaration.p,before.declaration.p,static_cast<unsigned long>(before.fvf));std::fflush(stdout);
    {
        Dx9Frame frame(device);Check(frame.Begin(),"overlay owns a final backbuffer scene");
        Com<IDirect3DSurface9> bound;
        Hr(device->GetRenderTarget(0,bound.Out()),"query overlay target while active");
        Check(bound.p==host.backbuffer.p,"active overlay target is primary backbuffer");
        D3DVIEWPORT9 activeViewport{};Hr(device->GetViewport(&activeViewport),"query overlay viewport while active");
        Check(activeViewport.X==0 && activeViewport.Y==0 && activeViewport.Width==320 && activeViewport.Height==240,"active overlay viewport covers backbuffer");
        DrawOverlay();Check(frame.Finish(),"overlay scene finishes and state restores");
    }
    Compare(device,caps,before);
    Check(SurfaceHash(device,host.target.p)==offscreen,"offscreen host pixels unchanged by overlay");
    if(host.mrt) Check(SurfaceHash(device,host.secondTarget.p)==second,"secondary host target pixels unchanged by overlay");
    Check(SurfaceHash(device,host.backbuffer.p)!=backbuffer,"overlay rendered into backbuffer rather than skipping");
    {
        Dx9Frame frame(device);Check(frame.Begin(),"empty overlay pass begins");
        Check(frame.Finish(),"empty overlay pass finishes");
    }
    Compare(device,caps,before);
    {
        Dx9Frame frame(device);Check(frame.Begin(),"RAII overlay pass begins");DrawOverlay();
    }
    Compare(device,caps,before);
    {
        Dx9Frame frame(device); // no Begin: destructor must leave host untouched
    }
    Compare(device,caps,before);
    Hr(device->BeginScene(),"host begins its own scene");
    {
        Dx9Frame frame(device);Check(!frame.Begin(),"nested host scene prevents overlay Begin");
    }
    // If the rejected overlay wrongly EndScene'd the host, this fails.
    Hr(device->EndScene(),"host still owns and can finish its original scene");
    Compare(device,caps,before);
    Check(SurfaceHash(device,host.target.p)==offscreen,"offscreen pixels intact after empty/nested/RAII cases");
    std::printf("PASS: %s mode preserves exact GPU state and offscreen pixels.\n",shaderMode?"shader/declaration":"fixed-function/FVF");
}
LRESULT CALLBACK TestWndProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {return DefWindowProcW(hwnd,message,wparam,lparam);}
void PureDevice(IDirect3D9* d3d,HWND hwnd,const D3DCAPS9& caps) {
    if(!(caps.DevCaps&D3DDEVCAPS_PUREDEVICE) || !(caps.DevCaps&D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
        Skip("PURE hardware device unsupported");return;
    }
    D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.BackBufferWidth=320;p.BackBufferHeight=240;
    p.BackBufferFormat=D3DFMT_X8R8G8B8;p.hDeviceWindow=hwnd;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    Com<IDirect3DDevice9> device;
    const auto create=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_PUREDEVICE|D3DCREATE_FPU_PRESERVE,&p,device.Out());
    if(FAILED(create)) {Skip("driver rejected PURE-device creation");return;}
    D3DMATRIX matrix{};
    const auto query=device->GetTransform(D3DTS_WORLD,&matrix);
    if(SUCCEEDED(query)) {Skip("driver exposes GetTransform on PURE device; getter-failure case unavailable");return;}
    {
        Dx9Frame frame(device.p);Check(!frame.Begin(),"PURE getter failure skips overlay cleanly");
    }
    Hr(device->BeginScene(),"PURE rejected overlay left no scene active");
    Hr(device->EndScene(),"PURE host can end its own scene");
    std::puts("PASS: PURE-device getter failure skips rendering cleanly.");
}
}
int main() {
    const auto instance=GetModuleHandleW(nullptr);
    const WNDCLASSEXW wc={sizeof(WNDCLASSEXW),CS_CLASSDC,TestWndProc,0,0,instance,nullptr,nullptr,nullptr,nullptr,L"AhkHiddenRenderRegression",nullptr};
    Check(RegisterClassExW(&wc)!=0,"register hidden owned test window");
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"AHK render regression (hidden)",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,instance,nullptr);
    Check(hwnd!=nullptr,"create hidden owned test window");
    Check(!IsWindowVisible(hwnd),"test window stays hidden");
    {
        Com<IDirect3D9> d3d;d3d.p=Direct3DCreate9(D3D_SDK_VERSION);Check(d3d.p!=nullptr,"create actual Direct3D9 runtime");
        D3DCAPS9 caps{};Hr(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,&caps),"query actual HAL capabilities");
        D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.BackBufferWidth=320;p.BackBufferHeight=240;
        p.BackBufferFormat=D3DFMT_X8R8G8B8;p.hDeviceWindow=hwnd;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        Com<IDirect3DDevice9> device;
        Hr(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,device.Out()),"create actual hidden HAL test device");
        {
            HostResources host;host.Create(device.p,caps);
            IMGUI_CHECKVERSION();ImGui::CreateContext();
            ImGui::GetIO().DisplaySize=ImVec2(320,240);ImGui::GetIO().DeltaTime=1.f/60.f;ImGui::GetIO().IniFilename=nullptr;
            Check(ImGui_ImplDX9_Init(device.p),"initialize actual ImGui Direct3D9 renderer");
            Exercise(device.p,caps,host,false);
            if(host.shaders) Exercise(device.p,caps,host,true);
            ImGui_ImplDX9_Shutdown();ImGui::DestroyContext();
            for(unsigned i=1;i<(std::min<DWORD>)(caps.NumSimultaneousRTs,4u);++i) Hr(device->SetRenderTarget(i,nullptr),"release extra host binding");
            Hr(device->SetDepthStencilSurface(nullptr),"release host depth binding");
            Hr(device->SetRenderTarget(0,host.backbuffer.p),"restore test device backbuffer");
        }
        // Separate device so PURE flags cannot influence the ordinary cases.
        device.p->Release();device.p=nullptr;
        PureDevice(d3d.p,hwnd,caps);
    }
    DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,instance);
    std::printf("Render state tests: %u checks passed, %u capability-dependent skips. No game client executed.\n",checks,skips);
    return 0;
}
