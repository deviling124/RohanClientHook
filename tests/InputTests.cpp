// Exercise the actual runtime input wrappers using fake native functions.
// No window, Direct3D device, injected process, or game executable is created.
#include "../src/Runtime.cpp"
#include <array>
namespace {
unsigned checks=0,failures=0,cursorCalls=0;
std::array<SHORT,256> keys{};
HRESULT inputResult=S_OK;
DWORD observedFlags=0;
int showCount=-3;
HCURSOR cursorShape=reinterpret_cast<HCURSOR>(0x1234);
void Check(bool ok,const char* label) {++checks;if(!ok){++failures;std::printf("FAIL:%s\n",label);}}
SHORT WINAPI FakeKey(int key) {return keys[static_cast<unsigned>(key)&255];}
BOOL WINAPI FakeCursor(int,int) {++cursorCalls;return FALSE;}
HCURSOR WINAPI FakeCursorShape(HCURSOR value) {auto old=cursorShape;cursorShape=value;return old;}
HCURSOR WINAPI FakeQueryCursor() {return cursorShape;}
BOOL WINAPI FakeCursorInfo(PCURSORINFO info) {info->flags=showCount>=0?CURSOR_SHOWING:0;return TRUE;}
int WINAPI FakeCursorVisibility(BOOL show) {return showCount+=show?1:-1;}
HRESULT STDMETHODCALLTYPE FakeState(IDirectInputDevice8W*,DWORD n,LPVOID data) {
    if(SUCCEEDED(inputResult)&&data)std::memset(data,0xff,n);
    return inputResult;
}
HRESULT STDMETHODCALLTYPE FakeData(IDirectInputDevice8W*,DWORD,LPDIDEVICEOBJECTDATA,LPDWORD n,DWORD flags) {
    observedFlags=flags;if(SUCCEEDED(inputResult)&&n)*n=2;return inputResult;
}
}
int main() {
    originalAsync=FakeKey;originalKey=FakeKey;originalCursor=FakeCursor;
    originalCursorShape=FakeCursorShape;originalCursorVisibility=FakeCursorVisibility;
    queryCursor=FakeQueryCursor;queryCursorInfo=FakeCursorInfo;
    overlayCursor=reinterpret_cast<HCURSOR>(0x5678);
    originalGetState=FakeState;originalGetData=FakeData;
    keys[VK_CONTROL]=static_cast<SHORT>(0x8001);
    visible=false;releaseHold=false;readingUiInput=false;
    Check(AsyncKey(VK_CONTROL)==keys[VK_CONTROL]&&Key(VK_CONTROL)==keys[VK_CONTROL],"closed overlay preserves keys");
    Check(Cursor(1,2)==FALSE&&cursorCalls==1,"closed overlay preserves cursor API result");
    SetVisible(true);
    Check(showCount==0&&cursorShape==overlayCursor,"overlay shows OS cursor and balances a negative visibility counter");
    CursorShape(nullptr);
    Check(cursorShape==overlayCursor,"game cannot hide OS cursor while editor is open");
    Check(CursorVisibility(FALSE)<0&&CursorVisibility(TRUE)>=0&&showCount==0,"game cursor hide/show loops terminate without counter drift");
    Check(AsyncKey(VK_CONTROL)==0&&Key(VK_CONTROL)==0,"open overlay blocks game key polling");
    Check(Cursor(1,2)==TRUE&&cursorCalls==1,"open overlay blocks game cursor recenter");
    readingUiInput=true;
    CursorShape(reinterpret_cast<HCURSOR>(0x9876));
    Check(cursorShape==reinterpret_cast<HCURSOR>(0x9876),"ImGui can update hardware cursor shape");
    Check(AsyncKey(VK_CONTROL)==keys[VK_CONTROL]&&Key(VK_CONTROL)==keys[VK_CONTROL],"UI modifier polling remains available");
    Check(Cursor(1,2)==FALSE&&cursorCalls==2,"UI cursor positioning bypasses game gate");
    readingUiInput=false;
    std::array<unsigned char,256> data;
    data.fill(0xaa);
    Check(GetState(nullptr,static_cast<DWORD>(data.size()),data.data())==S_OK,"GetState preserves success");
    for(auto byte:data)Check(byte==0,"open overlay zeroes successful DirectInput state");
    DWORD count=8;
    Check(GetData(nullptr,sizeof(DIDEVICEOBJECTDATA),nullptr,&count,DIGDD_PEEK)==S_OK&&count==0&&observedFlags==0,
          "open overlay drains buffered PEEK events and reports no game input");
    inputResult=DIERR_INPUTLOST;data.fill(0xaa);count=8;
    Check(GetState(nullptr,static_cast<DWORD>(data.size()),data.data())==DIERR_INPUTLOST&&data[0]==0xaa,
          "GetState preserves native failure and untouched buffer");
    Check(GetData(nullptr,sizeof(DIDEVICEOBJECTDATA),nullptr,&count,DIGDD_PEEK)==DIERR_INPUTLOST&&count==8,
          "GetData preserves native input-lost failure");
    inputResult=S_OK;
    SetVisible(false);
    Check(showCount==-3&&cursorShape==reinterpret_cast<HCURSOR>(0x1234),"close restores original cursor and exact display counter");
    Check(releaseHold&&BlockInput()&&AsyncKey(VK_CONTROL)==0,"closing retains input gate until release");
    closedAt=GetTickCount64()-200;ReleaseClosedInput();
    Check(releaseHold.load(),"held key prevents close click-through");
    keys.fill(0);ReleaseClosedInput();
    Check(!BlockInput(),"gate releases after keys are up and grace time passes");
    count=8;
    Check(GetData(nullptr,sizeof(DIDEVICEOBJECTDATA),nullptr,&count,DIGDD_PEEK)==S_OK&&count==2&&observedFlags==DIGDD_PEEK,
          "closed overlay restores PEEK semantics");
    Check(GetState(nullptr,static_cast<DWORD>(data.size()),data.data())==S_OK&&data[0]==0xff,"closed overlay restores game state");
    const DWORD pure=D3DCREATE_PUREDEVICE|D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE;
    Check(Dx9CompatibleDeviceFlags(pure)==(pure & ~D3DCREATE_PUREDEVICE),"device compatibility removes only PURE flag");
    Check(Dx9CompatibleDeviceFlags(D3DCREATE_SOFTWARE_VERTEXPROCESSING)==D3DCREATE_SOFTWARE_VERTEXPROCESSING,"non-PURE flags remain unchanged");
    std::printf("Input wrappers:%u checks,%u failures. No game code executed.\n",checks,failures);
    return failures?1:0;
}
