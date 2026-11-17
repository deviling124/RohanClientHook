#include <windows.h>
#include <cstdio>
int wmain() {
    HMODULE module=LoadLibraryW(L"bin/Release/novaclient.dll");
    if(!module) {std::printf("LoadLibrary failed:%lu\n",GetLastError());return 1;}
    using Initialize=DWORD(WINAPI*)(void*);using Status=DWORD(WINAPI*)();
    auto initialize=reinterpret_cast<Initialize>(GetProcAddress(module,"NovaClient_Initialize"));
    auto status=reinterpret_cast<Status>(GetProcAddress(module,"NovaClient_Status"));
    const bool ok=initialize && status && status()==0 &&
        initialize(reinterpret_cast<void*>(1))==ERROR_INVALID_PARAMETER && status()==0 &&
        initialize(nullptr)==ERROR_REVISION_MISMATCH && status()==3;
    FreeLibrary(module);
    std::printf("Client startup wrong-host rejection:%s\n",ok?"passed":"FAILED");return ok?0:1;
}
