// Native price reader regression tests; no game executable is started.
#include "../src/PriceInput.h"
#include "../vendor/detours/src/detours.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
unsigned checks=0,failed=0,conversions=0,attachments=0;
LONG attachResult=NO_ERROR;
PVOID installed=nullptr;
std::wstring text;
unsigned codepage=1252;
int forcedResult=0;
bool force=false;
constexpr auto Control=static_cast<std::uintptr_t>(0x1234);
void Check(bool ok,const char* label) {++checks;if(!ok){++failed;std::printf("FAIL:%s\n",label);}}
int __fastcall Convert(void* control,void*,char* output,int capacity) {
    ++conversions;
    Check(control==reinterpret_cast<void*>(Control),"native control pointer forwarded");
    Check(capacity==1023,"one byte reserved for NUL");
    if(force) return forcedResult;
    return WideCharToMultiByte(codepage,0,text.data(),static_cast<int>(text.size()),output,capacity,nullptr,nullptr);
}
struct Buffer {
    unsigned char before=0xa5;
    std::array<char,1024> bytes;
    unsigned char after=0x5a;
    Buffer() {bytes.fill('Q');bytes[0]='x';bytes[1]='4';bytes[2]=0;}
};
using ReadFn=int(__thiscall*)(void*,char*);
int Read(void* edit,Buffer& output) {
    // Invoke the actual detour through the native thiscall ABI (RET 4).
    auto function=reinterpret_cast<ReadFn>(installed);
    const int result=function(edit,output.bytes.data());
    Check(output.before==0xa5&&output.after==0x5a,"output guard bytes unchanged");
    return result;
}
std::vector<unsigned char> Reference() {
    std::ifstream file("00_reference/rohanclient_unpacked.exe",std::ios::binary);
    std::vector<unsigned char> image((std::istreambuf_iterator<char>(file)),{});
    Check(image.size()>0xb0a000-1,"reference fixture available");return image;
}
std::vector<unsigned char> Legacy(const std::vector<unsigned char>& input) {
    auto image=input;image.resize(0xb0b000);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data()+dos->e_lfanew);
    auto* section=IMAGE_FIRST_SECTION(nt)+nt->FileHeader.NumberOfSections;
    std::memset(section,0,sizeof *section);std::memcpy(section->Name,".kpfix",6);
    section->VirtualAddress=0xb0a000;section->Misc.VirtualSize=54;section->Characteristics=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;
    ++nt->FileHeader.NumberOfSections;nt->OptionalHeader.SizeOfImage=0xb0b000;
    // Byte-exact previous standalone patch, independently retained as fixture.
    const char* hex="568b742408c606008b898410000085c9741e68ff03000056e80000000085c07e0f3dff0300007708c60406005ec2040033c05ec20400";
    for(unsigned i=0;i<54;++i) {
        char pair[]={hex[2*i],hex[2*i+1],0};image[0xb0a000+i]=static_cast<unsigned char>(std::strtoul(pair,nullptr,16));
    }
    const std::int32_t call=0x0bed80-(0xb0a000+29);
    std::memcpy(image.data()+0xb0a000+25,&call,4);
    const std::int32_t jump=0xb0a000-(0xb5a30+5);
    image[0xb5a30]=0xe9;std::memcpy(image.data()+0xb5a30+1,&jump,4);image[0xb5a30+5]=0x90;
    return image;
}
}
extern "C" LONG WINAPI DetourAttach(PVOID*,PVOID function) {++attachments;installed=function;return attachResult;}

int main() {
    auto image=Reference();
    Check(PriceInput::MatchesReference(image.data()),"unpatched reference accepted");
    Check(!PriceInput::MatchesReference(nullptr)&&!PriceInput::MatchesReference(reinterpret_cast<unsigned char*>(1)),"invalid reference pointers rejected");
    image[0xb5a30]^=1;Check(!PriceInput::MatchesReference(image.data()),"foreign getter rejected");image[0xb5a30]^=1;
    image[0xbed80]^=1;Check(!PriceInput::MatchesReference(image.data()),"foreign converter rejected");image[0xbed80]^=1;
    Check(PriceInput::TestAttach(image.data())==NO_ERROR&&attachments==1&&installed,"native getter attaches");
    auto fixed=Legacy(image);
    Check(PriceInput::MatchesReference(fixed.data()),"exact earlier executable fix accepted");
    Check(PriceInput::TestAttach(fixed.data())==NO_ERROR&&attachments==1,"earlier fix is not hooked twice");
    fixed[0xb0a000+43]^=1;
    Check(!PriceInput::MatchesReference(fixed.data()),"modified earlier patch rejected");
    attachResult=ERROR_ACCESS_DENIED;
    Check(PriceInput::TestAttach(image.data())==ERROR_ACCESS_DENIED,"Detours installation failure propagated");
    attachResult=NO_ERROR;
    PriceInput::TestConverter(reinterpret_cast<PriceInput::ConvertFn>(Convert));
    std::array<unsigned char,0x1090> edit{};
    void* control=reinterpret_cast<void*>(Control);std::memcpy(edit.data()+0x1084,&control,4);

    for(int digit=0;digit<=9;++digit) {
        text=std::to_wstring(digit);
        Buffer old;
        const int n=WideCharToMultiByte(1252,0,text.data(),1,old.bytes.data(),1024,nullptr,nullptr);
        Check(n==1&&_atoi64(old.bytes.data())==digit*10+4,"unfixed conversion reproduces trailing 4");
        Buffer output;
        Check(Read(edit.data(),output)==1&&output.bytes[1]==0&&_atoi64(output.bytes.data())==digit,"single-digit input remains exact");
    }
    for(const auto* number:{L"10",L"11",L"14",L"24",L"99",L"100",L"999",L"123456789012345678"}) {
        text=number;Buffer output;
        Check(Read(edit.data(),output)==static_cast<int>(text.size())&&output.bytes[text.size()]==0,"multi-digit text terminated");
        Check(_atoi64(output.bytes.data())==_wtoi64(number),"multi-digit number unchanged");
    }
    text=L"\u4e2d\u6587";codepage=CP_UTF8;Buffer unicode;
    Check(Read(edit.data(),unicode)==6&&std::strcmp(unicode.bytes.data(),"\xe4\xb8\xad\xe6\x96\x87")==0,"NUL follows converted bytes, not wide-character count");
    codepage=1252;text.assign(1023,L'9');Buffer maximum;
    Check(Read(edit.data(),maximum)==1023&&maximum.bytes[1023]==0,"maximum payload fits with terminator");
    text.assign(1024,L'9');Buffer tooLong;
    Check(Read(edit.data(),tooLong)==0&&tooLong.bytes[0]==0,"failed full-buffer conversion returns empty text");
    text.clear();Buffer empty;
    Check(Read(edit.data(),empty)==0&&empty.bytes[0]==0,"empty input does not read stale text");
    force=true;
    for(int result:{-1,0,1024,0x7fffffff}) {
        forcedResult=result;Buffer output;
        Check(Read(edit.data(),output)==0&&output.bytes[0]==0,"bad conversion result returns empty text");
    }
    force=false;const auto calls=conversions;
    std::memset(edit.data()+0x1084,0,4);Buffer missing;
    Check(Read(edit.data(),missing)==0&&missing.bytes[0]==0&&conversions==calls,"missing control does not call converter");
    Buffer nullEdit;
    Check(Read(nullptr,nullEdit)==0&&nullEdit.bytes[0]==0,"missing edit returns empty text");
    Check(PriceInput::TestRead(edit.data(),nullptr)==0,"null output rejected");
    Buffer badEdit;
    Check(Read(reinterpret_cast<void*>(1),badEdit)==0&&badEdit.bytes[0]==0,"bad native edit pointer contained");
    std::printf("Price input:%u checks,%u failures. No game code executed.\n",checks,failed);
    return failed?1:0;
}
