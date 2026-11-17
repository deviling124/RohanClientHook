#include "Splash.h"

#include <objbase.h>
#include <gdiplus.h>
#include <atomic>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr WORD kSplashResource = 101;
constexpr int kSplashWidth = 400;
constexpr int kSplashHeight = 250;
constexpr wchar_t kWindowClass[] = L"NovaClientSplashWindow";

std::atomic<bool> stopping{false};
std::atomic<HWND> window{nullptr};
std::atomic<DWORD> threadId{0};
HANDLE threadHandle=nullptr;
HANDLE readyEvent=nullptr;
HMODULE moduleHandle=nullptr;

struct ImageData {
    Gdiplus::Image* image=nullptr;
    IStream* stream=nullptr;
};

void ReleaseImage(ImageData& data) {
    delete data.image;
    data.image=nullptr;
    if(data.stream) {data.stream->Release();data.stream=nullptr;}
}

bool LoadEmbeddedImage(ImageData& data) {
    const HRSRC resource=FindResourceW(moduleHandle,MAKEINTRESOURCEW(kSplashResource),RT_RCDATA);
    if(!resource) return false;
    const HGLOBAL loaded=LoadResource(moduleHandle,resource);
    const DWORD size=SizeofResource(moduleHandle,resource);
    if(!loaded || !size) return false;
    const HGLOBAL buffer=GlobalAlloc(GMEM_MOVEABLE,size);
    if(!buffer) return false;
    void* destination=GlobalLock(buffer);
    const void* source=LockResource(loaded);
    if(!destination || !source) {
        if(destination) GlobalUnlock(buffer);
        GlobalFree(buffer);
        return false;
    }
    CopyMemory(destination,source,size);
    GlobalUnlock(buffer);
    if(FAILED(CreateStreamOnHGlobal(buffer,TRUE,&data.stream))) {
        GlobalFree(buffer);
        return false;
    }
    data.image=Gdiplus::Image::FromStream(data.stream,TRUE);
    if(!data.image || data.image->GetLastStatus()!=Gdiplus::Ok) {
        ReleaseImage(data);
        return false;
    }
    return true;
}

Gdiplus::Rect ArtworkBounds(Gdiplus::Image& image) {
    const int height = MulDiv(kSplashWidth, static_cast<int>(image.GetHeight()),
                              static_cast<int>(image.GetWidth()));
    return Gdiplus::Rect(0, (kSplashHeight - height) / 2, kSplashWidth, height);
}

void DrawLoadingStatus(Gdiplus::Graphics& graphics) {
    const Gdiplus::RectF bounds(64.0f, 205.0f, 142.0f, 26.0f);
    const Gdiplus::RectF shadowBounds(bounds.X + 1.0f, bounds.Y + 1.0f, bounds.Width, bounds.Height);
    const Gdiplus::SolidBrush shadow(Gdiplus::Color(255, 15, 12, 12));
    const Gdiplus::SolidBrush foreground(Gdiplus::Color(255, 255, 234, 188));
    const Gdiplus::Font font(L"Segoe UI", 13.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.DrawString(L"Initializing Game...", -1, &font, shadowBounds, &format, &shadow);
    graphics.DrawString(L"Initializing Game...", -1, &font, bounds, &format, &foreground);
}

LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_NCHITTEST) return HTTRANSPARENT;
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc=BeginPaint(hwnd,&paint);
        RECT bounds{};GetClientRect(hwnd,&bounds);
        FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        auto* image=reinterpret_cast<Gdiplus::Image*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(image) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            const int imageWidth = static_cast<int>(image->GetWidth());
            const int imageHeight = static_cast<int>(image->GetHeight());
            graphics.DrawImage(image, ArtworkBounds(*image), 0, 0, imageWidth, imageHeight,
                               Gdiplus::UnitPixel);
            DrawLoadingStatus(graphics);
        }
        EndPaint(hwnd,&paint);
        return 0;
    }
    if(message==WM_CLOSE) {DestroyWindow(hwnd);return 0;}
    if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

void SignalReady() {
    if(readyEvent) SetEvent(readyEvent);
}

DWORD WINAPI ThreadProc(void*) {
    threadId=GetCurrentThreadId();
    // Apply DPI awareness only to the splash thread so Windows keeps its pixel size.
    using SetThreadDpiContext = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto setDpiContext = reinterpret_cast<SetThreadDpiContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if(setDpiContext) setDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    Gdiplus::GdiplusStartupInput startupInput{};
    ULONG_PTR gdiplusToken=0;
    ImageData image{};
    HWND hwnd=nullptr;
    WNDCLASSEXW klass{sizeof klass};
    bool classRegistered=false;

    if(Gdiplus::GdiplusStartup(&gdiplusToken,&startupInput,nullptr)==Gdiplus::Ok &&
       !stopping && LoadEmbeddedImage(image)) {
        klass.hInstance=moduleHandle;
        klass.lpfnWndProc=WindowProc;
        klass.lpszClassName=kWindowClass;
        klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        klass.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        classRegistered=RegisterClassExW(&klass)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
        if(classRegistered && !stopping) {
            RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
            const int x=work.left+(work.right-work.left-kSplashWidth)/2;
            const int y=work.top+(work.bottom-work.top-kSplashHeight)/2;
            hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,
                                 kWindowClass,L"NovaGuard — Rohan Online",
                                 WS_POPUP,x,y,kSplashWidth,kSplashHeight,nullptr,nullptr,moduleHandle,nullptr);
            if(hwnd) {
                const auto artwork=ArtworkBounds(*image.image);
                const HRGN region=CreateRectRgn(artwork.GetLeft(),artwork.GetTop(),
                                                artwork.GetRight(),artwork.GetBottom());
                if(region && !SetWindowRgn(hwnd,region,FALSE)) DeleteObject(region);
                SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(image.image));
                window=hwnd;
                ShowWindow(hwnd,SW_SHOWNOACTIVATE);
                UpdateWindow(hwnd);
                SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
            }
        }
    }
    SignalReady();

    if(hwnd) {
        MSG message{};
        while(!stopping.load() && GetMessageW(&message,nullptr,0,0)>0) {
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        window=nullptr;
        if(IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    if(classRegistered) UnregisterClassW(kWindowClass,moduleHandle);
    ReleaseImage(image);
    if(gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
    threadId=0;
    return 0;
}

}

namespace Splash {

void Start(HINSTANCE module) {
    if(threadHandle) return;
    moduleHandle=module;
    stopping=false;
    readyEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!readyEvent) return;
    threadHandle=CreateThread(nullptr,0,ThreadProc,nullptr,0,nullptr);
    if(!threadHandle) {
        CloseHandle(readyEvent);readyEvent=nullptr;
        return;
    }
    WaitForSingleObject(readyEvent,5000);
}

void Stop() {
    if(stopping.load() || stopping.exchange(true)) return;
    if(!threadHandle) return;
    if(const HWND hwnd=window.load()) PostMessageW(hwnd,WM_CLOSE,0,0);
    else if(const DWORD id=threadId.load()) PostThreadMessageW(id,WM_QUIT,0,0);
    if(WaitForSingleObject(threadHandle,5000)!=WAIT_OBJECT_0) return;
    CloseHandle(threadHandle);threadHandle=nullptr;
    if(readyEvent) {CloseHandle(readyEvent);readyEvent=nullptr;}
    moduleHandle=nullptr;
}

}
