#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d9.h>
#include <dinput.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cfenv>
#include "detours.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "AhkWindow.h"
#include "EventShop/EventShopWindow.h"
#include "DropBuff/DropBuffHud.h"
#include "EventShop/ItemIcons.h"
#include "Bridge.h"
#include "Dx9Frame.h"
#include "Splash.h"
#include "CustomEvents/EventNotice.h"
#include "CustomEvents/MonsterName.h"
#include "Chats/GlobalChat.h"
#include "Auth/AutotradeLogin.h"
#include "KioskSearch.h"
#include "PriceInput.h"
#include "ClientGuard/ClientGuard.h"
#include "UiTrace.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace {
using Create9Fn=IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using InputCreateFn=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
using InputDeviceFn=HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*,REFGUID,LPDIRECTINPUTDEVICE8W*,LPUNKNOWN);
using GetStateFn=HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*,DWORD,LPVOID);
using GetDataFn=HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*,DWORD,LPDIDEVICEOBJECTDATA,LPDWORD,DWORD);
Create9Fn originalCreate9=nullptr;
CreateDeviceFn originalCreateDevice=nullptr;
PresentFn originalPresent=nullptr;
ResetFn originalReset=nullptr;
InputCreateFn originalInputCreate=nullptr;
InputDeviceFn originalInputDevice=nullptr;
GetStateFn originalGetState=nullptr;
GetDataFn originalGetData=nullptr;
auto originalAsync=GetAsyncKeyState;
auto originalKey=GetKeyState;
auto originalCursor=SetCursorPos;
auto originalCursorShape=SetCursor;
auto originalCursorVisibility=ShowCursor;
auto queryCursor=GetCursor;
auto queryCursorInfo=GetCursorInfo;
HCURSOR overlayCursor=nullptr,savedCursor=nullptr;
int cursorBumps=0;
bool cursorOwned=false;
AhkWindow window;
EventShopWindow eventShop;
DropBuffHud dropBuffHud;
IDirect3DDevice9* primaryDevice=nullptr;
HWND primaryWindow=nullptr;
WNDPROC previousWndProc=nullptr;
DWORD renderThread=0;
std::atomic<bool> visible{false};
std::atomic<bool> releaseHold{false};
ULONGLONG closedAt=0;
std::atomic<LONG> startup{0};
std::atomic<bool> inputAvailable{true};
bool guiReady=false;
bool graphicsHealthy=true,graphicsWarning=false;
thread_local bool readingUiInput=false;
HMODULE ownModule=nullptr;
HANDLE logFile=INVALID_HANDLE_VALUE;
SRWLOCK logLock=SRWLOCK_INIT;

bool BlockInput() {return visible.load() || releaseHold.load();}
void AcquireOverlayCursor() {
    if(cursorOwned) return;
    if(!overlayCursor) overlayCursor=LoadCursorW(nullptr,IDC_ARROW);
    savedCursor=queryCursor();cursorBumps=0;cursorOwned=true;
    originalCursorShape(overlayCursor);
    CURSORINFO info{};info.cbSize=sizeof info;
    if(!queryCursorInfo(&info) || !(info.flags & CURSOR_SHOWING)) {
        int count=0;
        do {count=originalCursorVisibility(TRUE);++cursorBumps;} while(count<0 && cursorBumps<32);
    }
}
void ReleaseOverlayCursor() {
    if(!cursorOwned) return;
    originalCursorShape(savedCursor);
    while(cursorBumps>0) {originalCursorVisibility(FALSE);--cursorBumps;}
    cursorOwned=false;
}
void SetVisible(bool value) {
    const bool previous=visible.exchange(value);
    if(!previous && value) AcquireOverlayCursor();
    if(previous && !value) {ReleaseOverlayCursor();releaseHold=true;closedAt=GetTickCount64();}
}
void ReleaseClosedInput() {
    if(!releaseHold || visible || GetTickCount64()-closedAt<150) return;
    for(int key=1;key<256;++key) if(originalAsync(key)&0x8000) return;
    releaseHold=false;
}

void Log(const char* text) {
    OutputDebugStringA(text);OutputDebugStringA("\r\n");
    AcquireSRWLockExclusive(&logLock);
    if(logFile!=INVALID_HANDLE_VALUE) {DWORD n=0;WriteFile(logFile,text,static_cast<DWORD>(std::strlen(text)),&n,nullptr);WriteFile(logFile,"\r\n",2,&n,nullptr);}
    ReleaseSRWLockExclusive(&logLock);
}
bool Fingerprints() {
    __try {
        if(reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr))!=0x400000) return false;
        const auto* base=reinterpret_cast<const unsigned char*>(0x400000);
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>4096) return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386 ||
           nt->FileHeader.TimeDateStamp!=0x56c6d5a4 || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
           nt->OptionalHeader.ImageBase!=0x400000 || nt->OptionalHeader.SizeOfImage<0xb0a000 ||
           nt->OptionalHeader.SizeOfImage>0xb0c000) return false;
        constexpr unsigned char send[]={0x83,0xec,0x0c,0x56,0x8b,0xf1,0x8a,0x86,0x6d,0x08,0,0,0x84,0xc0,0x74,0x09};
        constexpr unsigned char receive[]={0x6a,0xff,0x68,0xeb,0xd3,0xbe,0,0x64,0xa1,0,0,0,0,0x50,0x64,0x89};
        constexpr unsigned char serializer[]={0x8b,0x44,0x24,0x08,0x0f,0xbf,0x48,0x10,0x0f,0xbf,0x50,0x0e};
        return !std::memcmp(reinterpret_cast<void*>(0x407f50),send,sizeof send) &&
               !std::memcmp(reinterpret_cast<void*>(0x5ea790),receive,sizeof receive) &&
               !std::memcmp(reinterpret_cast<void*>(0x401300),serializer,sizeof serializer) &&
               EventNotice::MatchesReference(base) && GlobalChat::MatchesReference(base) &&
               AutotradeLogin::MatchesReference(base) && KioskSearch::MatchesReference(base) &&
               PriceInput::MatchesReference(base) && ClientGuard::MatchesReference(base);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool InputMessage(UINT message) {
    return (message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) ||
        (message>=WM_KEYFIRST && message<=WM_KEYLAST) || message==WM_INPUT ||
        message==WM_IME_CHAR || message==WM_IME_COMPOSITION || message==WM_IME_STARTCOMPOSITION || message==WM_IME_ENDCOMPOSITION;
}
LRESULT CALLBACK WndProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_APP+0x391) {Bridge::Pump();return 0;}
    if(message==WM_APP+0x392) {KioskSearch::Frame();return 0;}
    if(guiReady && GetCurrentThreadId()==renderThread) {
        if(visible && message==WM_KEYDOWN && w==VK_ESCAPE) {
            window.Close();eventShop.Close();SetVisible(false);return 0;
        }
        if(visible) {
            readingUiInput=true;
            ImGui_ImplWin32_WndProcHandler(hwnd,message,w,l);
            readingUiInput=false;
        }
        if(BlockInput()) {
            if(InputMessage(message)) {
                // WM_INPUT requires DefWindowProc for raw-input cleanup.
                return message==WM_INPUT?DefWindowProcW(hwnd,message,w,l):0;
            }
            if(message==WM_SETCURSOR && LOWORD(l)==HTCLIENT) return TRUE;
        }
        if(message==WM_KILLFOCUS) {window.Close();eventShop.Close();SetVisible(false);}
    }
    return CallWindowProcW(previousWndProc,hwnd,message,w,l);
}
SHORT WINAPI AsyncKey(int key) {return BlockInput() && !readingUiInput?0:originalAsync(key);}
SHORT WINAPI Key(int key) {return BlockInput() && !readingUiInput?0:originalKey(key);}
BOOL WINAPI Cursor(int x,int y) {return BlockInput() && !readingUiInput?TRUE:originalCursor(x,y);}
HCURSOR WINAPI CursorShape(HCURSOR cursor) {
    return originalCursorShape(visible && !readingUiInput?overlayCursor:cursor);
}
int WINAPI CursorVisibility(BOOL show) {
    // Preserve the game's hide/show loops without hiding the hardware cursor or
    // changing its counter while the editor owns it. ImGui updates shapes itself.
    if(visible && !readingUiInput) return show?0:-1;
    return originalCursorVisibility(show);
}
HRESULT STDMETHODCALLTYPE GetState(IDirectInputDevice8W* self,DWORD bytes,LPVOID data) {
    const auto result=originalGetState(self,bytes,data);
    if(BlockInput() && SUCCEEDED(result) && data) std::memset(data,0,bytes);
    return result;
}
HRESULT STDMETHODCALLTYPE GetData(IDirectInputDevice8W* self,DWORD size,LPDIDEVICEOBJECTDATA data,LPDWORD count,DWORD flags) {
    const auto result=originalGetData(self,size,data,count,BlockInput()?0:flags);
    if(BlockInput() && SUCCEEDED(result) && count) *count=0;
    return result;
}
bool CommitAttach(PVOID* target,PVOID detour,PVOID* second=nullptr,PVOID secondDetour=nullptr) {
    auto result=DetourTransactionBegin();
    if(result!=NO_ERROR) return false;
    result=DetourUpdateThread(GetCurrentThread());
    if(result==NO_ERROR) result=DetourAttach(target,detour);
    if(result==NO_ERROR && second) result=DetourAttach(second,secondDetour);
    if(result!=NO_ERROR) {DetourTransactionAbort();return false;}
    return DetourTransactionCommit()==NO_ERROR;
}
HRESULT STDMETHODCALLTYPE CreateInputDevice(IDirectInput8W* self,REFGUID guid,LPDIRECTINPUTDEVICE8W* out,LPUNKNOWN outer) {
    const auto result=originalInputDevice(self,guid,out,outer);
    if(SUCCEEDED(result) && out && *out && !originalGetState) {
        auto table=*reinterpret_cast<void***>(*out);
        originalGetState=reinterpret_cast<GetStateFn>(table[9]);
        originalGetData=reinterpret_cast<GetDataFn>(table[10]);
        if(!CommitAttach(reinterpret_cast<PVOID*>(&originalGetState),reinterpret_cast<PVOID>(GetState),
                         reinterpret_cast<PVOID*>(&originalGetData),reinterpret_cast<PVOID>(GetData))) {
            originalGetState=nullptr;originalGetData=nullptr;inputAvailable=false;Log("DirectInput polling hook failed; overlay disabled.");
        }
    }
    return result;
}
HRESULT WINAPI CreateInput(HINSTANCE module,DWORD version,REFIID iid,LPVOID* out,LPUNKNOWN outer) {
    const auto result=originalInputCreate(module,version,iid,out,outer);
    if(SUCCEEDED(result) && out && *out && !originalInputDevice) {
        auto table=*reinterpret_cast<void***>(*out);
        originalInputDevice=reinterpret_cast<InputDeviceFn>(table[3]);
        if(!CommitAttach(reinterpret_cast<PVOID*>(&originalInputDevice),reinterpret_cast<PVOID>(CreateInputDevice))) {
            originalInputDevice=nullptr;inputAvailable=false;Log("DirectInput device hook failed; overlay disabled.");
        }
    }
    return result;
}
bool InitializeGui(IDirect3DDevice9* device) {
    D3DDEVICE_CREATION_PARAMETERS parameters{};
    if(FAILED(device->GetCreationParameters(&parameters)) || !IsWindow(parameters.hFocusWindow)) return false;
    DWORD pid=0;
    if(GetWindowThreadProcessId(parameters.hFocusWindow,&pid)!=GetCurrentThreadId() || pid!=GetCurrentProcessId()) {
        Log("Overlay requires rendering and window messages on the same game thread.");return false;
    }
    primaryWindow=parameters.hFocusWindow;renderThread=GetCurrentThreadId();
    IMGUI_CHECKVERSION();ImGui::CreateContext();
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    if(!ImGui_ImplWin32_Init(primaryWindow)) {ImGui::DestroyContext();return false;}
    if(!ImGui_ImplDX9_Init(device)) {
        ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();Log("Overlay renderer initialization failed.");return false;
    }
    SetLastError(0);
    previousWndProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(primaryWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(WndProc)));
    if(!previousWndProc) {
        ImGui_ImplDX9_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();Log("Window input hook failed.");return false;
    }
    EventShopWindow::InitializeFonts();
    primaryDevice=device;guiReady=true;
    Log("Custom AHK overlay ready. Type .ahk in Say chat.");return true;
}
struct FloatEnvironment {
    std::fenv_t value{};
    bool saved=std::fegetenv(&value)==0;
    ~FloatEnvironment() {if(saved) std::fesetenv(&value);}
};
HRESULT STDMETHODCALLTYPE Present(IDirect3DDevice9* self,const RECT* source,const RECT* destination,
                                   HWND overrideWindow,const RGNDATA* dirtyRegion) {
    UiTrace::Frame(); // the Collection Book window (opened by .cbook or F9); tracing only with uitrace.on
    {
    FloatEnvironment floatingPoint;
    try {
    if(!guiReady) InitializeGui(self);
    if(guiReady && self==primaryDevice && GetCurrentThreadId()==renderThread &&
       (!overrideWindow || overrideWindow==primaryWindow)) {
        struct IconFrame { ~IconFrame() { EventShopIcons::ReleaseFrame(); } } iconFrame;
        Bridge::Frame(window,primaryWindow,&eventShop,nullptr);
        // Native chat timeout notices run on the owning window thread.
        static ULONGLONG lastKioskPump=0;
        const auto kioskNow=GetTickCount64();
        if(kioskNow-lastKioskPump>=500) {
            lastKioskPump=kioskNow;PostMessageW(primaryWindow,WM_APP+0x392,0,0);
        }
        if(!inputAvailable || !graphicsHealthy) {window.Close();eventShop.Close();}
        SetVisible(window.Visible() || eventShop.Visible());ReleaseClosedInput();
        // The countdown label is drawn without opening any window or taking input.
        if((window.Visible() || eventShop.Visible()) && SUCCEEDED(self->TestCooperativeLevel())) {
            Dx9Frame frame(self);
            if(frame.Begin()) {
                readingUiInput=true;
                ImGui_ImplDX9_NewFrame();ImGui_ImplWin32_NewFrame();
                ImGui::GetIO().MouseDrawCursor=false;
                ImGui::NewFrame();window.Draw();eventShop.Draw(self);ImGui::EndFrame();ImGui::Render();
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                EventShopIcons::ReleaseFrame();
                readingUiInput=false;
                SetVisible(window.Visible() || eventShop.Visible());
                if(!frame.Finish()) {
                    graphicsHealthy=false;window.Close();eventShop.Close();SetVisible(false);
                    Log("Graphics state restoration failed; overlay disabled until device reset.");
                }
                Bridge::Frame(window,primaryWindow,&eventShop,nullptr);
            } else {
                window.Close();eventShop.Close();SetVisible(false);
                if(!graphicsWarning) {
                    graphicsWarning=true;
                    Log("Overlay drawing skipped: graphics state could not be captured or a scene was already active.");
                }
            }
        }
    }
    } catch(...) {
        readingUiInput=false;SetVisible(false);window.Close();eventShop.Close();
        Log("Overlay frame failed; closed settings window.");
    }
    }
    const auto result=originalPresent(self,source,destination,overrideWindow,dirtyRegion);
    if(SUCCEEDED(result)) Splash::Stop();
    return result;
}
HRESULT STDMETHODCALLTYPE Reset(IDirect3DDevice9* self,D3DPRESENT_PARAMETERS* parameters) {
    const bool owns=guiReady && self==primaryDevice;
    if(owns && GetCurrentThreadId()==renderThread) EventShopIcons::ReleaseFrame();
    if(owns) ImGui_ImplDX9_InvalidateDeviceObjects();
    const auto result=originalReset(self,parameters);
    if(owns && SUCCEEDED(result)) {
        ImGui_ImplDX9_CreateDeviceObjects();graphicsHealthy=true;graphicsWarning=false;
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreateDevice(IDirect3D9* self,UINT adapter,D3DDEVTYPE type,HWND hwnd,DWORD flags,
                                        D3DPRESENT_PARAMETERS* parameters,IDirect3DDevice9** out) {
    const DWORD effectiveFlags=Dx9CompatibleDeviceFlags(flags);
    char description[128]{};
    std::snprintf(description,sizeof description,"DX9 CreateDevice flags: requested=0x%08lX, overlay-compatible=0x%08lX",flags,effectiveFlags);
    Log(description);
    const auto result=originalCreateDevice(self,adapter,type,hwnd,effectiveFlags,parameters,out);
    if(SUCCEEDED(result) && out && *out && !originalPresent) {
        auto table=*reinterpret_cast<void***>(*out);
        originalReset=reinterpret_cast<ResetFn>(table[16]);originalPresent=reinterpret_cast<PresentFn>(table[17]);
        if(!CommitAttach(reinterpret_cast<PVOID*>(&originalPresent),reinterpret_cast<PVOID>(Present),
                         reinterpret_cast<PVOID*>(&originalReset),reinterpret_cast<PVOID>(Reset))) {
            originalPresent=nullptr;originalReset=nullptr;Log("Direct3D device hook failed.");
            Splash::Stop();
        }
    }
    return result;
}
IDirect3D9* WINAPI Create9(UINT version) {
    auto result=originalCreate9(version);
    if(result && !originalCreateDevice) {
        auto table=*reinterpret_cast<void***>(result);
        originalCreateDevice=reinterpret_cast<CreateDeviceFn>(table[16]);
        if(!CommitAttach(reinterpret_cast<PVOID*>(&originalCreateDevice),reinterpret_cast<PVOID>(CreateDevice))) {
            originalCreateDevice=nullptr;Log("Direct3D creation hook failed.");
            Splash::Stop();
        }
    }
    return result;
}
}

extern "C" DWORD WINAPI NovaClient_Initialize(void* reserved) {
    if(reserved) return ERROR_INVALID_PARAMETER;
    LONG old=0;
    if(!startup.compare_exchange_strong(old,1)) return old==2?NO_ERROR:ERROR_ALREADY_INITIALIZED;
    wchar_t logPath[MAX_PATH]{};
    if(GetModuleFileNameW(ownModule,logPath,MAX_PATH)) {
        wchar_t* separator=wcsrchr(logPath,L'\\');
        if(separator && static_cast<std::size_t>(separator-logPath)+24<MAX_PATH) {
            wcscpy_s(separator+1,MAX_PATH-(separator+1-logPath),L"novaclient.log");
            logFile=CreateFileW(logPath,FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        }
    }
    if(!Fingerprints()) {Log("Unsupported client build; no hooks installed.");startup=3;return ERROR_REVISION_MISMATCH;}
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(&NovaClient_Initialize),&pinned)) {startup=3;return GetLastError();}
    Splash::Start(ownModule);
    auto d3d=LoadLibraryW(L"d3d9.dll"),input=LoadLibraryW(L"dinput8.dll");
    if(!d3d || !input) {Splash::Stop();startup=3;return ERROR_MOD_NOT_FOUND;}
    originalCreate9=reinterpret_cast<Create9Fn>(GetProcAddress(d3d,"Direct3DCreate9"));
    originalInputCreate=reinterpret_cast<InputCreateFn>(GetProcAddress(input,"DirectInput8Create"));
    if(!originalCreate9 || !originalInputCreate) {Splash::Stop();startup=3;return ERROR_PROC_NOT_FOUND;}
    LONG result=DetourTransactionBegin();
    if(result!=NO_ERROR) {Splash::Stop();startup=3;return result;}
    result=DetourUpdateThread(GetCurrentThread());
    if(result==NO_ERROR) result=Bridge::Attach();
    if(result==NO_ERROR) result=EventNotice::Attach();
    if(result==NO_ERROR) result=MonsterName::Attach();
    if(result==NO_ERROR) result=GlobalChat::Attach();
    if(result==NO_ERROR) result=AutotradeLogin::Attach();
    if(result==NO_ERROR) result=KioskSearch::Attach();
    if(result==NO_ERROR) result=PriceInput::Attach();
    if(result==NO_ERROR) result=ClientGuard::Attach();
    if(result==NO_ERROR) result=UiTrace::Attach();
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalCreate9),reinterpret_cast<PVOID>(Create9));
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalInputCreate),reinterpret_cast<PVOID>(CreateInput));
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalAsync),reinterpret_cast<PVOID>(AsyncKey));
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalKey),reinterpret_cast<PVOID>(Key));
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalCursor),reinterpret_cast<PVOID>(Cursor));
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalCursorShape),reinterpret_cast<PVOID>(CursorShape));
    if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&originalCursorVisibility),reinterpret_cast<PVOID>(CursorVisibility));
    if(result!=NO_ERROR) {DetourTransactionAbort();Splash::Stop();startup=3;return result;}
    result=DetourTransactionCommit();
    startup=result==NO_ERROR?2:3;
    if(result!=NO_ERROR) Splash::Stop();
    Log(result==NO_ERROR?"Client hooks installed before game startup.":"Client hook transaction failed.");
    if(result==NO_ERROR) Log("Event notices: native green #42C762 (stock palette 0x19).");
    if(result==NO_ERROR) Log("Autotrade login confirmation enabled.");
    Log(ClientGuard::StatusText());
    return result;
}
extern "C" DWORD WINAPI NovaClient_Status() {return static_cast<DWORD>(startup.load());}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {ownModule=module;DisableThreadLibraryCalls(module);}
    return TRUE;
}
