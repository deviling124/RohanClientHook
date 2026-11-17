#include "PriceInput.h"
#include "detours.h"
#include <cstdint>
#include <cstring>

namespace PriceInput {
namespace {
static_assert(sizeof(void*)==4,"The reference edit-text ABI is Win32");
constexpr unsigned GetterRva=0x0b5a30,ConverterRva=0x0bed80;
constexpr unsigned Capacity=1024;
using EditFn=int(__thiscall*)(void*,char*);
using WideFn=int(__thiscall*)(void*,char*,int);
auto originalEdit=reinterpret_cast<EditFn>(0x004b5a30);
auto convert=reinterpret_cast<WideFn>(0x004bed80);
constexpr unsigned char Getter[]={
    0x8b,0x89,0x84,0x10,0,0,0x83,0xc8,0xff,0x85,0xc9,0x74,0x0f,0x8b,0x44,0x24,
    0x04,0x68,0,0x04,0,0,0x50,0xe8,0x34,0x93,0,0,0xc2,0x04,0,0xcc
};
constexpr unsigned char Converter[]={
    0x83,0xb9,0xb8,0,0,0,0x08,0x8b,0x81,0xb4,0,0,0,0x72,0x08,0x8b,
    0x89,0xa4,0,0,0,0xeb,0x06,0x81,0xc1,0xa4,0,0,0,0x8b,0x54,0x24
};
// Exact earlier .kpfix implementation, with its relative CALL filled below.
constexpr unsigned char LegacyStub[]={
    0x56,0x8b,0x74,0x24,0x08,0xc6,0x06,0,0x8b,0x89,0x84,0x10,0,0,0x85,0xc9,
    0x74,0x1e,0x68,0xff,0x03,0,0,0x56,0xe8,0,0,0,0,0x85,0xc0,0x7e,0x0f,
    0x3d,0xff,0x03,0,0,0x77,0x08,0xc6,0x04,0x06,0,0x5e,0xc2,0x04,0,
    0x33,0xc0,0x5e,0xc2,0x04,0
};
enum class Layout { Unsupported,Native,AlreadyFixed };
Layout Inspect(const unsigned char* base) {
    if(!base) return Layout::Unsupported;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>4096) return Layout::Unsupported;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386 ||
       nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->FileHeader.NumberOfSections>96 ||
       nt->OptionalHeader.SizeOfImage<ConverterRva+sizeof Converter) return Layout::Unsupported;
    if(std::memcmp(base+ConverterRva,Converter,sizeof Converter)) return Layout::Unsupported;
    if(!std::memcmp(base+GetterRva,Getter,sizeof Getter)) return Layout::Native;
    if(base[GetterRva]!=0xe9 || base[GetterRva+5]!=0x90 ||
       std::memcmp(base+GetterRva+6,Getter+6,sizeof Getter-6)) return Layout::Unsupported;
    std::int32_t displacement=0;
    std::memcpy(&displacement,base+GetterRva+1,4);
    const auto target=static_cast<std::int64_t>(GetterRva)+5+displacement;
    if(target<0 || target+sizeof LegacyStub>nt->OptionalHeader.SizeOfImage) return Layout::Unsupported;
    const auto* section=IMAGE_FIRST_SECTION(nt);
    bool knownSection=false;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)
        if(!std::memcmp(section[i].Name,".kpfix\0\0",8) && section[i].VirtualAddress==target &&
           section[i].Misc.VirtualSize>=sizeof LegacyStub && (section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)) knownSection=true;
    if(!knownSection) return Layout::Unsupported;
    unsigned char expected[sizeof LegacyStub];
    std::memcpy(expected,LegacyStub,sizeof expected);
    const auto call=static_cast<std::int32_t>(ConverterRva-(target+29));
    std::memcpy(expected+25,&call,4);
    return std::memcmp(base+target,expected,sizeof expected)?Layout::Unsupported:Layout::AlreadyFixed;
}
Layout GuardedInspect(const unsigned char* base) noexcept {
    __try {return Inspect(base);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return Layout::Unsupported;}
}
int __fastcall ReadEdit(void* edit,void*,char* output) noexcept {
    __try {
        if(!output) return 0;
        output[0]=0;
        if(!edit) return 0;
        void* control=nullptr;
        std::memcpy(&control,static_cast<const unsigned char*>(edit)+0x1084,sizeof control);
        if(!control) return 0;
        // The native converter receives an explicit wide-character count and
        // writes no NUL. Reserve a byte, then terminate at the returned BYTE
        // count; callers such as _atoi64 otherwise read stale stack digits.
        const int bytes=convert(control,output,Capacity-1);
        if(bytes<=0 || bytes>=static_cast<int>(Capacity)) {output[0]=0;return 0;}
        output[bytes]=0;return bytes;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
LONG AttachTo(const unsigned char* base) {
    const auto layout=GuardedInspect(base);
    if(layout==Layout::AlreadyFixed) return NO_ERROR;
    if(layout!=Layout::Native) return ERROR_REVISION_MISMATCH;
    return DetourAttach(&reinterpret_cast<PVOID&>(originalEdit),reinterpret_cast<PVOID>(ReadEdit));
}
}
bool MatchesReference(const unsigned char* base) noexcept {return GuardedInspect(base)!=Layout::Unsupported;}
LONG Attach() {return AttachTo(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)));}
#ifdef PRICE_INPUT_TESTING
void TestConverter(ConvertFn converter) {convert=converter;}
int TestRead(void* edit,char* output) {return ReadEdit(edit,nullptr,output);}
LONG TestAttach(const unsigned char* base) {return AttachTo(base);}
#endif
}
