#pragma once
#ifndef AHK_BRIDGE_TESTING
#error This header is only for isolated bridge tests.
#endif
#include <windows.h>
#include <cstddef>
namespace Bridge::Test {
using NativeSend=int(__thiscall*)(void*,int,void*,unsigned,unsigned);
using NativeIngress=void(__thiscall*)(void*,unsigned,void*,void*,unsigned,unsigned,unsigned,unsigned,
                                    unsigned,unsigned,unsigned,void*,void*,unsigned);
void Configure(void* current,unsigned id,unsigned a,unsigned b,NativeSend sender,NativeIngress receiver);
void Session(void* current,unsigned id);
int Submit(void* self,int opcode,void* body,unsigned a,unsigned b);
void Receive(void* caller,void* self,unsigned mode,void* text,void* name,unsigned sender,
             unsigned a=0,unsigned b=0,unsigned c=0,unsigned d=0,unsigned e=0,unsigned f=0,
             void* links=nullptr,void* object=nullptr,unsigned count=0);
std::size_t Incoming();
std::size_t Outgoing();
}
