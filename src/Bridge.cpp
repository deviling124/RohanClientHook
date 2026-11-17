#include "Bridge.h"
#include "AhkWindow.h"
#include "CustomEvents/EventNotice.h"
#include "EventShop/EventShopWindow.h"
#include "DropBuff/DropBuffHud.h"
#include "detours.h"
#include "../shared/GlobalChatWire.h"
#include <intrin.h>
#include <atomic>
#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#ifdef AHK_BRIDGE_TESTING
#include "BridgeTest.h"
#endif

namespace {
using SendFn=int(__thiscall*)(void*,int,void*,unsigned,unsigned);
using IngressFn=void(__thiscall*)(void*,unsigned,void*,void*,unsigned,unsigned,unsigned,unsigned,
                                unsigned,unsigned,unsigned,void*,void*,unsigned);
auto sendOriginal=reinterpret_cast<SendFn>(0x407f50);
auto ingressOriginal=reinterpret_cast<IngressFn>(0x5ea790);
std::mutex queueMutex;
std::deque<std::string> incoming,outgoing;
std::atomic<bool> openRequested{false};
std::atomic<bool> shopRequested{false};
std::atomic<bool> sessionResetRequested{false};
std::atomic<DWORD> gameThread{0};
std::array<unsigned char,21> sayPrefix{};
std::atomic<void*> connection{nullptr};
std::atomic<unsigned> character{0};
std::atomic<bool> prefixReady{false};
// Set only when the runtime owns a drop-buff countdown: then any Say also teaches the Say header.
std::atomic<bool> learnFromChat{false};
HWND gameHwnd=nullptr;
#ifdef AHK_BRIDGE_TESTING
void* testConnection=nullptr;
unsigned testCharacter=0,testMetadataA=0,testMetadataB=0;
thread_local void* testCaller=nullptr;
#endif

bool ReadString(void* object,char* output,std::size_t capacity,std::size_t& length) {
    __try {
        if(!object || !output || !capacity) return false;
        const auto* p=static_cast<unsigned char*>(object);
        unsigned size=0,storage=0;
        std::memcpy(&size,p+0x14,4);std::memcpy(&storage,p+0x18,4);
        if(size>=capacity || size>storage) return false;
        const char* source=reinterpret_cast<const char*>(p+4);
        if(storage>=16) std::memcpy(&source,p+4,4);
        std::memcpy(output,source,size);output[size]=0;length=size;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ReadSay(void* body,char* output,short& length,unsigned char* prefix) {
    __try {
        if(!body) return false;
        const auto* p=static_cast<unsigned char*>(body);
        std::memcpy(&length,p+14,2);
        if(length<1 || length>160) return false;
        std::memcpy(output,p+21,length);output[length]=0;
        std::memcpy(prefix,p,21);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool Current(void*& current,unsigned& id) {
#ifdef AHK_BRIDGE_TESTING
    current=testConnection;id=testCharacter;
    return current && id;
#else
    __try {
        current=*reinterpret_cast<void**>(0xd3ab54);
        const auto* player=*reinterpret_cast<unsigned char**>(0xd3ffc0);
        if(!current || !player) return false;
        std::memcpy(&id,player+8,4);return id!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#endif
}
bool Metadata(unsigned& a,unsigned& b) {
#ifdef AHK_BRIDGE_TESTING
    a=testMetadataA;b=testMetadataB;return true;
#else
    __try {
        a=*reinterpret_cast<unsigned*>(0xd7b8e8);b=*reinterpret_cast<unsigned*>(0xd7b8ec);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#endif
}
bool IsOpenCommand(const char* text,short length) {
    while(length && (text[length-1]==' ' || text[length-1]=='\0')) --length;
    return length==4 && text[0]=='.' && (text[1]=='a'||text[1]=='A') &&
        (text[2]=='h'||text[2]=='H') && (text[3]=='k'||text[3]=='K');
}
bool IsShopCommand(const char* text,short length) {
    while(length && (text[length-1]==' ' || text[length-1]=='\0')) --length;
    return length==7 && _strnicmp(text,".epshop",7)==0;
}
// Remembers the connection and the 21-byte Say header so queued commands can be sent as Say later.
void Learn(void* self,unsigned id,const unsigned char* prefix) {
    std::lock_guard<std::mutex> guard(queueMutex);
    const bool changed=!prefixReady.load() || connection.load()!=self || character.load()!=id;
    if(changed) {
        outgoing.clear();incoming.clear();sessionResetRequested=true;
    }
    connection=self;character=id;prefixReady=true;
    std::memcpy(sayPrefix.data(),prefix,21);
    gameThread=GetCurrentThreadId();
}
int __fastcall Send(void* self,void*,int opcode,void* body,unsigned a,unsigned b) {
    if(opcode==0x1f03) {
        char text[161]{};short length=0;unsigned char prefix[21]{};
        if(ReadSay(body,text,length,prefix)) {
            const bool command=IsOpenCommand(text,length) || IsShopCommand(text,length);
            void* current=nullptr;unsigned id=0;
            if(Current(current,id) && current==self && (command || learnFromChat.load())) {
                // With the countdown active any ordinary Say teaches the header, so the background
                // poll needs no typed command. Without it only the explicit commands do.
                Learn(self,id,prefix);
                if(command) {
                    if(IsShopCommand(text,length)) shopRequested=true;
                    else openRequested=true;
                    return 0; // Local command: do not broadcast .ahk into chat.
                }
            }
        }
    }
    return sendOriginal(self,opcode,body,a,b);
}
void __fastcall Ingress(void* self,void*,unsigned mode,void* text,void* name,unsigned sender,
                       unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f,
                       void* links,void* linkObject,unsigned count) {
    // Bind the machine reply to the native Shout decoder, mode and server sender.
#ifdef AHK_BRIDGE_TESTING
    const auto caller=testCaller;
#else
    const auto caller=_ReturnAddress();
#endif
    // ChatSubmit can add a local Say echo after its native send returns.
    // Suppress only our own consumed open command on that verified call site.
    if(caller==reinterpret_cast<void*>(0x5ece1e) && mode==0) {
        char local[161]{};std::size_t length=0;
        if(ReadString(text,local,sizeof local,length)) {
            if(sender==character.load() && Bridge::SessionMatches() &&
               (IsOpenCommand(local,static_cast<short>(length)) || IsShopCommand(local,static_cast<short>(length)))) return;
            void* current=nullptr;
            unsigned id=0;
            std::string_view message;
            if(GlobalChatWire::Parse({local,length},message)!=GlobalChatWire::Channel::None &&
               Current(current,id) && sender==id) return;
        }
    }
    if(caller==reinterpret_cast<void*>(0x402f14) && mode==3 && sender==0 && Bridge::SessionMatches()) {
        char message[256]{},source[32]{};std::size_t n=0,s=0;
        if(ReadString(text,message,sizeof message,n) && ReadString(name,source,sizeof source,s) &&
           ((s==12 && std::memcmp(source,"[AdvancedHK]",12)==0 && n>=7 && std::memcmp(message,"AHKUI1 ",7)==0) ||
            (s==11 && std::memcmp(source,"[EventShop]",11)==0 && n>=5 && std::memcmp(message,"EPS1 ",5)==0) ||
            (s==DropBuffWire::Source.size() && std::memcmp(source,DropBuffWire::Source.data(),s)==0 &&
             n>=DropBuffWire::Prefix.size() && std::memcmp(message,DropBuffWire::Prefix.data(),DropBuffWire::Prefix.size())==0))) {
            std::lock_guard<std::mutex> guard(queueMutex);
            try {if(incoming.size()<32) incoming.emplace_back(message,n);} catch(...) {}
            return;
        }
    }
    ingressOriginal(self,mode,text,name,sender,a,b,c,d,e,f,links,linkObject,count);
}
}

namespace Bridge {
LONG Attach() {
    LONG error=DetourAttach(&reinterpret_cast<PVOID&>(sendOriginal),reinterpret_cast<PVOID>(Send));
    if(error==NO_ERROR) error=DetourAttach(&reinterpret_cast<PVOID&>(ingressOriginal),reinterpret_cast<PVOID>(Ingress));
    return error;
}
bool SessionMatches() {
    void* current=nullptr;unsigned id=0;
    return prefixReady.load() && Current(current,id) && current==connection.load() && id==character.load();
}
void Pump() {
    if(GetCurrentThreadId()!=gameThread || !SessionMatches()) return;
    // One small ordered snapshot (10 messages max) on the original game thread.
    std::deque<std::string> commands;
    std::array<unsigned char,21> prefix;
    {std::lock_guard<std::mutex> guard(queueMutex);commands.swap(outgoing);prefix=sayPrefix;}
    for(const auto& command:commands) {
        if(!SessionMatches()) break;
        if(command.empty() || command.size()>160 ||
           (command.compare(0,8,".ahkui1 ")!=0 && command.compare(0,6,".eps1 ")!=0 &&
            command!=DropBuffWire::PollCommand)) continue;
        std::array<unsigned char,181> body{};
        std::memcpy(body.data(),prefix.data(),21);
        const short length=static_cast<short>(command.size());
        std::memcpy(body.data()+14,&length,2);
        std::memset(body.data()+16,0,5); // no item-link payload or link flags
        std::memcpy(body.data()+21,command.data(),command.size());
        // Preserve the verified native send route/serializer and session arguments.
        unsigned a=0,b=0;
        if(!Metadata(a,b)) break;
        sendOriginal(connection.load(),0x1f03,body.data(),a,b);
    }
}
void Disconnect(AhkWindow& window,EventShopWindow* shop,DropBuffHud* hud) {
    if(shop) shop->ResetSession();
    if(hud) hud->Reset();
    shopRequested=false;
    window.ResetSession();prefixReady=false;openRequested=false;sessionResetRequested=false;
    connection=nullptr;character=0;gameThread=0;
    std::lock_guard<std::mutex> guard(queueMutex);
    incoming.clear();outgoing.clear();
}
void Frame(AhkWindow& window,HWND hwnd,EventShopWindow* shop,DropBuffHud* hud) {
    gameHwnd=hwnd;
    if(hud) learnFromChat=true;
    if(sessionResetRequested.exchange(false)) {window.ResetSession();if(shop) shop->ResetSession();if(hud) hud->Reset();}
    if(openRequested.exchange(false)) {if(shop) shop->Close();window.Open();}
    if(shopRequested.exchange(false) && shop) shop->Open();
    if((window.Visible() || (shop && shop->Visible()) || prefixReady.load()) && !SessionMatches()) Disconnect(window,shop,hud);
    std::deque<std::string> replies;
    {std::lock_guard<std::mutex> guard(queueMutex);replies.swap(incoming);}
    for(const auto& reply:replies) {
        if(reply.compare(0,5,"EPS1 ")==0) {if(shop) shop->Receive(reply);}
        else if(reply.compare(0,DropBuffWire::Prefix.size(),DropBuffWire::Prefix)==0) {if(hud) hud->Receive(reply);}
        else window.Receive(reply);
    }
    auto commands=window.TakeCommands();
    if(shop) {
        auto extra=shop->TakeCommands();
        for(auto& command:extra) commands.push_back(std::move(command));
        const auto notice=shop->TakeChatNotice();
        if(!notice.empty() && SessionMatches()) EventNotice::ShowLocal(("[Event Shop] " + notice).c_str());
        if(shop->Visible() && window.Visible()) window.Close();
    }
    if(hud && SessionMatches() && hud->PollDue()) commands.emplace_back(DropBuffWire::PollCommand);
    if(!commands.empty() && SessionMatches()) {
        std::lock_guard<std::mutex> guard(queueMutex);
        if(outgoing.size()+commands.size()<=32)
            for(auto& command:commands) outgoing.emplace_back(std::move(command));
    }
    if(GetCurrentThreadId()==gameThread) Pump();
    else if(!commands.empty()) PostMessageW(gameHwnd,WM_APP+0x391,0,0);
}
#ifdef AHK_BRIDGE_TESTING
namespace Test {
void Configure(void* current,unsigned id,unsigned a,unsigned b,NativeSend sender,NativeIngress receiver) {
    std::lock_guard<std::mutex> guard(queueMutex);
    incoming.clear();outgoing.clear();openRequested=false;shopRequested=false;sessionResetRequested=false;gameThread=0;
    prefixReady=false;learnFromChat=false;connection=nullptr;character=0;sayPrefix.fill(0);gameHwnd=nullptr;
    testConnection=current;testCharacter=id;testMetadataA=a;testMetadataB=b;
    sendOriginal=sender;ingressOriginal=receiver;
}
void Session(void* current,unsigned id) {testConnection=current;testCharacter=id;}
int Submit(void* self,int opcode,void* body,unsigned a,unsigned b) {return Send(self,nullptr,opcode,body,a,b);}
void Receive(void* caller,void* self,unsigned mode,void* text,void* name,unsigned sender,
             unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f,void* links,void* object,unsigned count) {
    testCaller=caller;
    Ingress(self,nullptr,mode,text,name,sender,a,b,c,d,e,f,links,object,count);
    testCaller=nullptr;
}
std::size_t Incoming() {std::lock_guard<std::mutex> guard(queueMutex);return incoming.size();}
std::size_t Outgoing() {std::lock_guard<std::mutex> guard(queueMutex);return outgoing.size();}
}
#endif
}
