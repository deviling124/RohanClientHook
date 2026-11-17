// Standalone renderer for reviewing the actual overlay UI without a game.
// Its backend is deliberately in-memory; it never connects to a server.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <gdiplus.h>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"
#include "AhkWindow.h"
#include "EventShop/EventShopWindow.h"
#include "EventShop/ItemIcons.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace {
IDirect3D9* d3d=nullptr;
IDirect3DDevice9* device=nullptr;
D3DPRESENT_PARAMETERS presentation{};
UINT resizeWidth=0,resizeHeight=0;
bool initialized=false;

LRESULT CALLBACK WndProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if(initialized && ImGui_ImplWin32_WndProcHandler(hwnd,message,wparam,lparam)) return 1;
    if(message==WM_SIZE && wparam!=SIZE_MINIMIZED) {
        resizeWidth=LOWORD(lparam);resizeHeight=HIWORD(lparam);return 0;
    }
    if(message==WM_SYSCOMMAND && (wparam & 0xfff0)==SC_KEYMENU) return 0;
    if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
void Reset() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if(SUCCEEDED(device->Reset(&presentation))) ImGui_ImplDX9_CreateDeviceObjects();
}
void MockServer(AhkWindow& window) {
    static Ahk::Settings saved=Ahk::Defaults(),staging{};
    static std::uint32_t stageToken=0;
    static unsigned mask=0;
    for(const auto& command:window.TakeCommands()) {
        const auto words=Ahk::Words(command);
        std::uint32_t token=0;
        if(words.size()<3 || !Ahk::Number(words[2],token)) continue;
        const auto prefix="AHKUI1 "+std::to_string(token)+" ";
        if(words[1]=="get") {
            window.Receive(prefix+"state 1 "+(saved.active?"1":"0"));
            for(unsigned c=0;c<Ahk::CategoryCount;++c)
                window.Receive(prefix+"row "+std::to_string(c)+" "+Ahk::Encode(saved.rows[c]));
            window.Receive(prefix+"end");
        } else if(words[1]=="begin" && words.size()==4) {
            staging={};stageToken=token;mask=0;staging.active=words[3]=="1";
        } else if(words[1]=="row" && words.size()==5 && token==stageToken) {
            std::uint32_t category=0;
            if(Ahk::Number(words[3],category) && category<Ahk::CategoryCount && Ahk::Decode(words[4],staging.rows[category]))
                mask|=1u<<category;
        } else if(words[1]=="commit" && token==stageToken && mask==0xff && Ahk::Valid(staging)) {
            saved=staging;window.Receive(prefix+"saved");stageToken=0;
        }
    }
}
void MockShop(EventShopWindow& shop) {
    static long long balance=125;
    for(const auto& command:shop.TakeCommands()) {
        EventShopWire::Request r;
        if(!EventShopWire::ParseRequest(command,r)) continue;
        const auto token=std::to_string(r.nonce);
        if(r.action==EventShopWire::Action::Open) {
            shop.Receive("EPS1 opened "+token);
        } else if(r.action==EventShopWire::Action::List) {
            const bool available=r.category==1 || r.category==3;
            shop.Receive("EPS1 state "+token+" 0 "+(available?"1 1 ":"0 0 ")+std::to_string(balance)+" "+std::to_string(r.category));
            if(available) {
                const EventShopWire::Offer offer = r.category==1
                    ? EventShopWire::Offer{2,2621591,1,10,123,"Split Sword","Melee Attack +10%. Requires level 110."}
                    : EventShopWire::Offer{1,2884093,250,10,123,"Guild Stone",""};
                shop.Receive(EventShopWire::Row(r.nonce,0,offer));
                shop.Receive(EventShopWire::Detail(r.nonce,0,offer));
            }
        } else if(r.action==EventShopWire::Action::Buy) {
            const bool enough=balance>=10;
            if(enough) balance-=10;
            shop.Receive("EPS1 buy "+token+" "+r.key+" "+(enough?"0 ":"4 ")+std::to_string(balance)+(enough?" 100":" 0"));
        }
    }
}
bool SaveShopPreview() {
    IDirect3DSurface9* back=nullptr;
    IDirect3DSurface9* copy=nullptr;
    if(FAILED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back))) return false;
    D3DSURFACE_DESC desc{}; back->GetDesc(&desc);
    bool saved=false;
    if(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&copy,nullptr)) &&
       SUCCEEDED(device->GetRenderTargetData(back,copy))) {
        D3DLOCKED_RECT lock{};
        if(SUCCEEDED(copy->LockRect(&lock,nullptr,D3DLOCK_READONLY))) {
            std::vector<unsigned char> pixels(desc.Width*desc.Height*4);
            for(UINT y=0;y<desc.Height;++y) {
                auto* row=pixels.data()+y*desc.Width*4;
                memcpy(row,static_cast<unsigned char*>(lock.pBits)+y*lock.Pitch,desc.Width*4);
                for(UINT x=0;x<desc.Width;++x) row[x*4+3]=255;
            }
            copy->UnlockRect();
            Gdiplus::GdiplusStartupInput startup; ULONG_PTR token=0;
            if(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok) {
                { Gdiplus::Bitmap bitmap(desc.Width,desc.Height,desc.Width*4,PixelFormat32bppARGB,pixels.data());
                  const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};
                  saved=bitmap.Save(L"docs/event-shop-preview.png",&png,nullptr)==Gdiplus::Ok; }
                Gdiplus::GdiplusShutdown(token);
            }
        }
    }
    if(copy) copy->Release(); back->Release(); return saved;
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR arguments,int show) {
    const bool shopMode=wcsstr(arguments,L"--event-shop")!=nullptr;
    const bool snapshot=wcsstr(arguments,L"--snapshot")!=nullptr;
    ImGui_ImplWin32_EnableDpiAwareness();
    const WNDCLASSEXW wc={sizeof(WNDCLASSEXW),CS_CLASSDC,WndProc,0,0,instance,nullptr,nullptr,nullptr,nullptr,L"AhkOverlayPreview",nullptr};
    if(!RegisterClassExW(&wc)) return 1;
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"Advanced Hunter Kit - UI preview",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1080,850,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 2;
    d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if(!d3d) {DestroyWindow(hwnd);return 3;}
    presentation.Windowed=TRUE;presentation.SwapEffect=D3DSWAPEFFECT_DISCARD;
    presentation.BackBufferFormat=D3DFMT_UNKNOWN;presentation.EnableAutoDepthStencil=TRUE;
    presentation.AutoDepthStencilFormat=D3DFMT_D16;presentation.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    if(FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&presentation,&device))) {
        d3d->Release();DestroyWindow(hwnd);return 4;
    }
    IMGUI_CHECKVERSION();ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr;
    ImGui::StyleColorsDark();
    if(!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX9_Init(device)) return 5;
    if(shopMode) EventShopWindow::InitializeFonts();
    initialized=true;
    ShowWindow(hwnd,snapshot?SW_HIDE:show);UpdateWindow(hwnd);
    AhkWindow window; EventShopWindow shop;
    if(shopMode) shop.Open();
    else window.Open();
    unsigned frames=0;
    bool snapshotSaved=false;
    bool quit=false,firstFrame=true;
    while(!quit) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message);DispatchMessageW(&message);
            if(message.message==WM_QUIT) quit=true;
        }
        if(quit) break;
        if(IsIconic(hwnd)) {Sleep(10);continue;}
        if(resizeWidth && resizeHeight) {
            presentation.BackBufferWidth=resizeWidth;presentation.BackBufferHeight=resizeHeight;
            resizeWidth=resizeHeight=0;Reset();
        }
        const auto cooperative=device->TestCooperativeLevel();
        if(cooperative==D3DERR_DEVICELOST) {Sleep(10);continue;}
        if(cooperative==D3DERR_DEVICENOTRESET) {Reset();continue;}
        MockServer(window);
        if(shopMode) MockShop(shop);
        ImGui_ImplDX9_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
        const auto size=ImGui::GetIO().DisplaySize;
        auto* background=ImGui::GetBackgroundDrawList();
        background->AddRectFilledMultiColor(ImVec2(0,0),size,IM_COL32(11,23,31,255),IM_COL32(20,36,43,255),IM_COL32(9,17,24,255),IM_COL32(14,29,36,255));
        background->AddText(ImVec2(24,size.y-32),IM_COL32(145,163,171,255),"UI PREVIEW  |  In-memory example settings  |  F10 reopens the window");
        if(ImGui::IsKeyPressed(ImGuiKey_F10)) {if(shopMode) shop.Open();else window.Open();}
        if(ImGui::IsKeyPressed(ImGuiKey_Escape)) {window.Close();shop.Close();}
        if(firstFrame) {
            // The first-use position centers the independent overlay, just as in game.
            ImGui::SetNextWindowPos(ImVec2(size.x*.5f,size.y*.5f),ImGuiCond_FirstUseEver,ImVec2(.5f,.5f));
            firstFrame=false;
        }
        window.Draw();
        if(shopMode) shop.Draw(device);
        ImGui::Render();
        device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
        device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,D3DCOLOR_RGBA(10,20,27,255),1.f,0);
        if(SUCCEEDED(device->BeginScene())) {ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());device->EndScene();}
        EventShopIcons::ReleaseFrame();
        if(snapshot && ++frames>=3) {snapshotSaved=SaveShopPreview();quit=true;}
        device->Present(nullptr,nullptr,nullptr,nullptr);
    }
    initialized=false;ImGui_ImplDX9_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
    device->Release();d3d->Release();DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,instance);
    return snapshot && !snapshotSaved ? 6 : 0;
}
