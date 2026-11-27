#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/Bridge.h"
#include "../src/BridgeTest.h"
#include "../src/AhkWindow.h"
#include "../src/CustomEvents/EventNotice.h"
#include "../src/EventShop/EventShopWindow.h"
#include "../src/DropBuff/DropBuffHud.h"
#include "detours.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
unsigned checks=0;
void Check(bool ok,const char* label) {
    ++checks;
    if(!ok) {std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}
}
void* const context=reinterpret_cast<void*>(0x12345678);
void* const otherContext=reinterpret_cast<void*>(0x23456789);
struct Packet {
    void* self=nullptr;int opcode=0;unsigned a=0,b=0;
    std::array<unsigned char,181> body{};
    short length=0;
    std::string text;
};
std::vector<Packet> sent;
std::vector<std::string> localNotices;
unsigned displays=0;
std::array<unsigned,13> displayArgs{};
void* displaySelf=nullptr;
std::function<void()> afterSend;
bool CopyBody(void* body,unsigned char* output,short& length) {
    __try {
        if(!body) return false;
        const auto* p=static_cast<const unsigned char*>(body);
        std::memcpy(&length,p+14,2);
        if(length<0 || length>160) return false;
        std::memcpy(output,p,21+length);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
int __fastcall FakeSend(void* self,void*,int opcode,void* body,unsigned a,unsigned b) {
    Packet p;p.self=self;p.opcode=opcode;p.a=a;p.b=b;
    if(CopyBody(body,p.body.data(),p.length))
        p.text.assign(reinterpret_cast<const char*>(p.body.data()+21),p.length);
    sent.push_back(p);
    if(afterSend) afterSend();
    return 73;
}
void __fastcall FakeIngress(void* self,void*,unsigned mode,void* text,void* name,unsigned sender,
    unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f,void* links,void* object,unsigned count) {
    ++displays;displaySelf=self;
    displayArgs={mode,reinterpret_cast<unsigned>(text),reinterpret_cast<unsigned>(name),sender,a,b,c,d,e,f,
        reinterpret_cast<unsigned>(links),reinterpret_cast<unsigned>(object),count};
}
void Setup() {
    sent.clear();localNotices.clear();displays=0;displayArgs.fill(0);afterSend={};
    Bridge::Test::Configure(context,100,0x11223344,0xaabbccdd,
        reinterpret_cast<Bridge::Test::NativeSend>(FakeSend),
        reinterpret_cast<Bridge::Test::NativeIngress>(FakeIngress));
}
std::array<unsigned char,181> Say(const std::string& text) {
    std::array<unsigned char,181> body{};
    for(unsigned i=0;i<21;++i) body[i]=static_cast<unsigned char>(0x80+i);
    const auto length=static_cast<short>(text.size());
    std::memcpy(body.data()+14,&length,2);
    std::memcpy(body.data()+21,text.data(),text.size());return body;
}
struct NativeString {
    std::array<unsigned char,28> object{};
    std::string storage;
    explicit NativeString(std::string value,bool heap=false):storage(std::move(value)) {
        const auto length=static_cast<unsigned>(storage.size());
        const auto capacity=heap || length>=16 ? (length<16?16:length) : 15;
        std::memcpy(object.data()+20,&length,4);std::memcpy(object.data()+24,&capacity,4);
        if(capacity<16) std::memcpy(object.data()+4,storage.data(),length);
        else {const auto p=storage.data();std::memcpy(object.data()+4,&p,4);}
    }
    void* Data() {return object.data();}
};
void Receive(const std::string& text,void* caller=reinterpret_cast<void*>(0x402f14),unsigned mode=3,
    unsigned sender=0,const std::string& name="[AdvancedHK]",bool heapName=false) {
    NativeString textObject(text),nameObject(name,heapName);
    Bridge::Test::Receive(caller,context,mode,textObject.Data(),nameObject.Data(),sender);
}
std::string Open(AhkWindow& window) {
    auto body=Say(".AhK  ");
    const auto original=body;
    Check(Bridge::Test::Submit(context,0x1f03,body.data(),5,6)==0,"local command consumed");
    Check(sent.empty() && body==original,"open never sends or changes user packet");
    Bridge::Frame(window,nullptr);
    Check(window.Visible() && window.Busy() && sent.size()==1,"frame opens and sends one settings request");
    const auto words=Ahk::Words(sent[0].text);
    Check(words.size()==3 && words[0]==".ahkui1" && words[1]=="get","get request serialized as Say");
    return std::string(words[2]);
}
void Snapshot(AhkWindow& window,const std::string& token,bool eligible=true) {
    auto settings=Ahk::Defaults();
    Receive("AHKUI1 "+token+" state "+(eligible?"1":"0")+" 0");
    for(unsigned i=0;i<Ahk::CategoryCount;++i)
        Receive("AHKUI1 "+token+" row "+std::to_string(i)+" "+Ahk::Encode(settings.rows[i]));
    Receive("AHKUI1 "+token+" end");
    Check(Bridge::Test::Incoming()==10,"complete native snapshot queued");
    Bridge::Frame(window,nullptr);
    Check(window.Ready() && window.Eligible()==eligible,"native ingress reaches real window model");
    Check(displays==0 && Bridge::Test::Incoming()==0,"machine replies hidden and queue drained");
}
void CommandAndFraming() {
    Setup();AhkWindow window;const auto token=Open(window);
    const auto expected=Say(".AhK  ");
    const auto& p=sent.front();
    Check(p.self==context && p.opcode==0x1f03,"send uses current native context and Say opcode");
    Check(p.a==0x11223344 && p.b==0xaabbccdd,"native session metadata preserved");
    Check(std::memcmp(p.body.data(),expected.data(),14)==0,"authentic Say prefix preserved");
    short links=-1;std::memcpy(&links,p.body.data()+16,2);
    Check(links==0 && p.body[18]==0 && p.body[19]==0 && p.body[20]==0,"item-link fields cleared");
    Check(static_cast<std::size_t>(p.length)==p.text.size() && p.length<=160,"actual text length matches serialized body");
    Snapshot(window,token);
    window.EditDraft()->active=true;window.EditDraft()->rows[0].minimum[2]=61;
    Check(window.Apply(),"real model queues edited snapshot");
    Bridge::Frame(window,nullptr);
    Check(sent.size()==11,"apply sends begin eight rows and commit in order");
    const auto begin=Ahk::Words(sent[1].text);const std::string saveToken(begin[2]);
    Check(begin.size()==4 && begin[1]=="begin" && begin[3]=="1","begin carries active state");
    for(unsigned i=0;i<8;++i) {
        const auto words=Ahk::Words(sent[2+i].text);Ahk::Row row;
        Check(words.size()==5 && words[1]=="row" && words[2]==saveToken && words[3]==std::to_string(i) && Ahk::Decode(words[4],row),"ordered category row valid");
        Check(sent[2+i].text.size()<=160,"complete category fits native Say bound");
    }
    Check(sent.back().text==".ahkui1 commit "+saveToken,"commit is last");
    Check(window.Busy() && !window.Ready(),"transport success alone does not acknowledge disk save");
    Receive("AHKUI1 "+token+" saved");Bridge::Frame(window,nullptr);
    Check(window.Busy(),"stale acknowledgement does not commit draft");
    Receive("AHKUI1 "+saveToken+" saved");Bridge::Frame(window,nullptr);
    Check(window.Ready() && !window.Dirty() && window.Draft().active && window.Draft().rows[0].minimum[2]==61,"matching native acknowledgement commits edited model");
}
void PassThroughAndBounds() {
    Setup();AhkWindow window;
    for(const auto& text:{"hello",".ahk on",".ahkui1 get 1",".ahkx"," .ahk"}) {
        auto body=Say(text);
        Check(Bridge::Test::Submit(context,0x1f03,body.data(),7,8)==73,"non-open chat forwards original return");
        Check(sent.back().text==text && sent.back().a==7 && sent.back().b==8,"normal Say and metadata unchanged");
    }
    auto body=Say(".ahk");
    Check(Bridge::Test::Submit(context,0x1f04,body.data(),7,8)==73,"other opcode not intercepted");
    Check(Bridge::Test::Submit(otherContext,0x1f03,body.data(),7,8)==73,"wrong connection not intercepted");
    short invalid=161;std::memcpy(body.data()+14,&invalid,2);
    Check(Bridge::Test::Submit(context,0x1f03,body.data(),7,8)==73,"oversized Say safely forwards");
    invalid=-1;std::memcpy(body.data()+14,&invalid,2);
    Check(Bridge::Test::Submit(context,0x1f03,body.data(),7,8)==73,"negative Say safely forwards");
    Check(Bridge::Test::Submit(context,0x1f03,nullptr,7,8)==73,"null body safely forwards");
    body=Say(std::string(160,'x'));
    Check(Bridge::Test::Submit(context,0x1f03,body.data(),7,8)==73 && sent.back().text.size()==160,"exact native maximum remains valid ordinary chat");
    Check(!window.Visible() && !Bridge::SessionMatches(),"ordinary traffic never opens window or seeds context");
}
void IngressGuardsAndAbi() {
    Setup();AhkWindow window;const auto token=Open(window);
    NativeString text("AHKUI1 "+token+" state 1 0"),name("[AdvancedHK]");
    const auto marker=reinterpret_cast<void*>(0x76543210);
    Bridge::Test::Receive(nullptr,otherContext,99,text.Data(),name.Data(),123,11,12,13,14,15,16,marker,context,17);
    Check(displays==1 && displaySelf==otherContext,"non-machine ingress forwards this pointer");
    Check(displayArgs==std::array<unsigned,13>{99,reinterpret_cast<unsigned>(text.Data()),reinterpret_cast<unsigned>(name.Data()),123,11,12,13,14,15,16,reinterpret_cast<unsigned>(marker),reinterpret_cast<unsigned>(context),17},"all thirteen native ingress DWORD slots preserved");
    Receive(text.storage,reinterpret_cast<void*>(0x402f15));
    Receive(text.storage,reinterpret_cast<void*>(0x402f14),2);
    Receive(text.storage,reinterpret_cast<void*>(0x402f14),3,123);
    Receive(text.storage,reinterpret_cast<void*>(0x402f14),3,0,"OtherName");
    Receive("ordinary message");
    Check(displays==6 && Bridge::Test::Incoming()==0,"wrong source mode sender name and prefix pass through");
    unsigned huge=256;std::memcpy(text.object.data()+20,&huge,4);
    Bridge::Test::Receive(reinterpret_cast<void*>(0x402f14),context,3,text.Data(),name.Data(),0);
    Check(displays==7 && Bridge::Test::Incoming()==0,"oversized native string rejected before copying");
    const unsigned size=1,cap=16,bad=1;
    std::memcpy(text.object.data()+20,&size,4);std::memcpy(text.object.data()+24,&cap,4);std::memcpy(text.object.data()+4,&bad,4);
    Bridge::Test::Receive(reinterpret_cast<void*>(0x402f14),context,3,text.Data(),name.Data(),0);
    Check(displays==8,"invalid heap pointer guarded without crashing");
    Receive("AHKUI1 "+token+" state 1 0",reinterpret_cast<void*>(0x402f14),3,0,"[AdvancedHK]",true);
    Check(Bridge::Test::Incoming()==1,"heap-backed native sender name supported");
    Bridge::Test::Session(otherContext,101);
    Receive("AHKUI1 "+token+" end");
    Check(displays==9 && Bridge::Test::Incoming()==1,"disconnected machine replies not consumed or queued");
}
void ThreadSessionAndQueue() {
    Setup();AhkWindow window;const auto token=Open(window);Snapshot(window,token);
    window.EditDraft()->active=true;Check(window.Apply(),"queue test edits real model");
    const auto prior=sent.size();
    std::thread wrongThread([&]{Bridge::Frame(window,nullptr);Bridge::Pump();});wrongThread.join();
    Check(sent.size()==prior && Bridge::Test::Outgoing()==10,"render worker cannot send on wrong native thread");
    afterSend=[] {Bridge::Test::Session(otherContext,101);};
    Bridge::Pump();afterSend={};
    Check(sent.size()==prior+1 && Bridge::Test::Outgoing()==0,"reentrant session change stops remaining atomic snapshot messages");
    Bridge::Frame(window,nullptr);
    Check(!window.Visible() && !Bridge::SessionMatches() && Bridge::Test::Incoming()==0 && Bridge::Test::Outgoing()==0,"disconnect hides editor and discards pending transport");
    Setup();AhkWindow bounded;const auto fresh=Open(bounded);
    for(unsigned i=0;i<40;++i) Receive("AHKUI1 "+fresh+" unknown");
    Check(Bridge::Test::Incoming()==32,"incoming queue bounded to32 replies");
    Bridge::Disconnect(bounded);
    Check(!bounded.Visible() && Bridge::Test::Incoming()==0 && Bridge::Test::Outgoing()==0,"explicit disconnect clears bounded queues");
}
void AccountTransitionWhileHidden() {
    Setup();AhkWindow window;auto token=Open(window);Snapshot(window,token);
    window.EditDraft()->active=true;Check(window.Apply(),"account transition starts pending save");
    Bridge::Frame(window,nullptr);
    const auto saving=Ahk::Words(sent[1].text);const std::string saveToken(saving[2]);
    window.Close();
    Receive("AHKUI1 "+saveToken+" saved");
    const auto before=sent.size();
    auto command=Say(".ahk");
    Check(Bridge::Test::Submit(context,0x1f03,command.data(),0,0)==0,"same-account reopen intercepted");
    Bridge::Frame(window,nullptr);
    Check(window.Ready() && window.Draft().active && sent.size()==before,"same-account reopen keeps already queued save acknowledgement");
    window.EditDraft()->active=false;Check(window.Apply(),"second account transition save starts");
    Bridge::Frame(window,nullptr);window.Close();
    const auto oldSave=Ahk::Words(sent[sent.size()-10].text);const std::string stale(oldSave[2]);
    Bridge::Test::Session(otherContext,101);
    const auto prior=sent.size();
    Check(Bridge::Test::Submit(otherContext,0x1f03,command.data(),0,0)==0,"new-account reopen captured before an intervening frame");
    Bridge::Frame(window,nullptr);
    Check(window.Visible() && window.Busy() && sent.size()==prior+1,"new session resets hidden old save and requests fresh settings");
    const auto words=Ahk::Words(sent.back().text);token=std::string(words[2]);
    Check(words[1]=="get" && token!=stale && !window.Ready(),"new session has a fresh load nonce");
    Receive("AHKUI1 "+stale+" saved");Bridge::Frame(window,nullptr);
    Check(!window.Ready(),"old account save acknowledgement cannot complete new load");
}
void LocalOpenEcho() {
    Setup();AhkWindow window;
    Receive(".ahk",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Check(displays==1,"local echo is unchanged before an open command captures a session");
    Setup();const auto token=Open(window);
    Receive(".AhK  ",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Check(displays==0 && Bridge::Test::Incoming()==0,"own exact open command local echo is suppressed without enqueuing a response");
    Receive(".ahk",reinterpret_cast<void*>(0x5ece1e),0,101,"OtherPlayer");
    Receive(".ahk",reinterpret_cast<void*>(0x5ece1e),3,100,"LocalPlayer");
    Receive(".ahk",reinterpret_cast<void*>(0x5ece1f),0,100,"LocalPlayer");
    Receive(".ahk",reinterpret_cast<void*>(0x402f14),0,100,"LocalPlayer");
    Check(displays==4,"other sender mode caller and decoded chat are never hidden as local echoes");
    Receive(".ahk on",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Receive(".ahkx",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Receive("normal text",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Check(displays==7,"other own local Say text remains visible");
    Bridge::Test::Session(otherContext,101);
    Receive(".ahk",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Check(displays==8,"stale captured session does not hide local echo");
}

void GlobalChatLocalEcho() {
    Setup();
    auto* local = reinterpret_cast<void*>(0x5ece1e);
    auto world = Say(".WoRLd hello");
    auto trade = Say(".trade");
    Check(Bridge::Test::Submit(context,0x1f03,world.data(),5,6)==73 &&
          Bridge::Test::Submit(context,0x1f03,trade.data(),5,6)==73 &&
          sent.size()==2 && sent[0].text==".WoRLd hello" && sent[1].text==".trade",
          "global chat commands reach the server through Say");
    Receive(".WoRLd hello",local,0,100,"LocalPlayer");
    Receive(".trade",local,0,100,"LocalPlayer");
    Check(displays==0,"own global chat command echoes are hidden without opening AHK");
    Receive(".worldwide hello",local,0,100,"LocalPlayer");
    Receive(".trade buy",local,0,101,"OtherPlayer");
    Receive(".world hello",local,3,100,"LocalPlayer");
    Receive(".trade buy",reinterpret_cast<void*>(0x402f14),0,100,"LocalPlayer");
    Check(displays==4,"ordinary Say, other senders, and received chat are displayed");
    Bridge::Test::Session(otherContext,101);
    Receive(".trade buy",local,0,100,"LocalPlayer");
    Receive(".trade buy",local,0,101,"OtherPlayer");
    Check(displays==5,"global chat echo suppression uses the current character");
}
void DropBuffProtocolAndCountdown() {
    using namespace DropBuffWire;
    State state;
    Check(Parse("DBF1 50 1790 0",state) && state.percent==50 && state.remainingSeconds==1790 && !state.paused,"parses a running buff");
    Check(Parse("DBF1 0 0 0",state) && state.percent==0,"parses no buff");
    Check(Parse("DBF1 100 86400 1",state) && state.paused,"parses a paused buff");
    for(const char* bad:{"DBF1","DBF1 50 1790","DBF1 50 1790 0 0","DBF1 -1 5 0","DBF1 50 5 2","DBF1 1001 5 0",
                         "DBF1 50 86401 0","DBF1 5x 5 0","DBF1  50 5 0","DBF2 50 5 0","dbf1 50 5 0",""})
        Check(!Parse(bad,state),"rejects malformed reply");
    State round{75,901,true};State again;
    Check(Parse(Encode(round),again) && again==round,"encode and parse round trip");

    DropBuffHud hud;using Clock=DropBuffHud::Clock;const auto t0=Clock::now();
    Check(!hud.Receive("DBF1 50 x 0",t0) && !hud.Active(t0),"malformed reply ignored");
    Check(hud.Receive("DBF1 50 100 0",t0) && hud.Active(t0),"reply starts the countdown");
    Check(hud.Current(t0+std::chrono::seconds(30)).remainingSeconds==70,"running buff counts down");
    Check(!hud.Active(t0+std::chrono::seconds(100)),"countdown ends at zero");
    hud.Receive("DBF1 50 100 1",t0);
    Check(hud.Current(t0+std::chrono::seconds(60)).remainingSeconds==100 && hud.Current(t0).paused,"paused buff does not count down");
    hud.Receive("DBF1 0 0 0",t0);
    Check(!hud.Active(t0),"no-buff reply clears it");
    hud.Reset(t0);
    Check(hud.PollDue(t0) && !hud.PollDue(t0+std::chrono::seconds(10)) && hud.PollDue(t0+DropBuffHud::IdlePoll),"idle poll interval");
    hud.Receive("DBF1 50 100 0",t0);
    Check(!hud.PollDue(t0+std::chrono::seconds(4)) && hud.PollDue(t0+std::chrono::seconds(5)),"active poll interval is shorter");
}
void DropBuffTransport() {
    Setup(); AhkWindow window; DropBuffHud hud;
    {
        auto early=Say("hello");
        Check(Bridge::Test::Submit(context,0x1f03,early.data(),5,6)==73 && !Bridge::SessionMatches(),
            "chat before the countdown exists teaches nothing");
        sent.clear();
    }
    Bridge::Frame(window,nullptr,nullptr,&hud);
    Check(sent.empty(),"no poll before the Say header is known");
    auto chat=Say("hello");const auto original=chat;
    Check(Bridge::Test::Submit(context,0x1f03,chat.data(),5,6)==73 && chat==original,"ordinary chat still sent unchanged");
    Check(sent.size()==1 && sent[0].text=="hello","ordinary chat reaches the native send once");
    sent.clear();
    Bridge::Frame(window,nullptr,nullptr,&hud);
    Check(sent.size()==1 && sent[0].text==".dbf1" && sent[0].opcode==0x1f03,"first frame polls with the learned header");
    Check(std::memcmp(sent[0].body.data(),original.data(),14)==0,"poll uses the player's own Say header");
    Bridge::Frame(window,nullptr,nullptr,&hud);
    Check(sent.size()==1,"no second poll inside the interval");
    const auto source=reinterpret_cast<void*>(0x402f14);
    Receive("DBF1 50 1790 0",source,3,100,"[DropBuff]");
    Receive("DBF1 50 1790 0",reinterpret_cast<void*>(0x402f15),3,0,"[DropBuff]");
    Receive("DBF1 50 1790 0",source,0,0,"[DropBuff]");
    Receive("DBF1 50 1790 0",source,3,0,"[EventShop]");
    Check(Bridge::Test::Incoming()==0 && displays==4,"spoofed sender, caller, mode and name are not intercepted");
    Receive("DBF1 50 1790 0",source,3,0,"[DropBuff]");
    Check(Bridge::Test::Incoming()==1 && displays==4,"authentic reply is hidden and queued");
    Bridge::Frame(window,nullptr,nullptr,&hud);
    Check(hud.Current().percent==50 && hud.Current().remainingSeconds>=1789,"frame delivers the reply to the countdown");
    Receive("DBF1 nonsense",source,3,0,"[DropBuff]");
    Bridge::Frame(window,nullptr,nullptr,&hud);
    Check(hud.Current().percent==50,"malformed reply leaves the countdown alone");
    Bridge::Test::Session(otherContext,100);
    Bridge::Frame(window,nullptr,nullptr,&hud);
    Check(!hud.Active(),"a connection change clears the countdown");
}
unsigned attacked=0;
void CountAttacked() {++attacked;}
void HunterKitStopSignal() {
    Setup(); attacked=0;
    Bridge::SetAttackedByPlayerHandler(CountAttacked);
    const auto source=reinterpret_cast<void*>(0x402f14);
    Check(!Bridge::SessionMatches(),"no Say header yet");
    Receive("HKS1 stop",source,3,0,"[HunterKit]");
    Check(attacked==1 && displays==0 && Bridge::Test::Incoming()==0,"stop signal handled at once, hidden, no prior chat needed");
    Receive("HKS1 stop",source,3,100,"[HunterKit]");
    Receive("HKS1 stop",reinterpret_cast<void*>(0x402f15),3,0,"[HunterKit]");
    Receive("HKS1 stop",source,0,0,"[HunterKit]");
    Receive("HKS1 stop",source,3,0,"[EventShop]");
    Receive("HKS1 stop now",source,3,0,"[HunterKit]");
    Receive("hello",source,3,0,"[HunterKit]");
    Check(attacked==1 && displays==6,"spoofed sender, caller, mode, name and other text pass through as chat");
    Bridge::SetAttackedByPlayerHandler(nullptr);
}
void EventShopTransport() {
    Setup(); AhkWindow window; EventShopWindow shop;
    auto command = Say(".EPShop  ");
    Check(Bridge::Test::Submit(context,0x1f03,command.data(),5,6)==0, "shop open is local");
    Bridge::Frame(window,nullptr,&shop);
    Check(!shop.Visible() && !window.Visible() && sent.size()==1, "shop stays hidden awaiting server access check");
    EventShopWire::Request request;
    Check(EventShopWire::ParseRequest(sent.back().text,request) && request.action==EventShopWire::Action::Open,
        "shop open check serialized");
    const auto opened="EPS1 opened "+std::to_string(request.nonce);
    Receive(opened,reinterpret_cast<void*>(0x402f14),3,100,"[EventShop]");
    Receive(opened,reinterpret_cast<void*>(0x402f15),3,0,"[EventShop]");
    Receive(opened,reinterpret_cast<void*>(0x402f14),0,0,"[EventShop]");
    Receive(opened,reinterpret_cast<void*>(0x402f14),3,0,"[AdvancedHK]");
    Check(Bridge::Test::Incoming()==0 && displays==4, "shop rejects spoofed sender caller mode and name");
    Receive("EPS1 error "+std::to_string(request.nonce)+" 5",reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    Bridge::Frame(window,nullptr,&shop);
    Check(!shop.Visible() && sent.size()==1 && localNotices==std::vector<std::string>{
        "[Event Shop] The Event Point Shop is only available in safe zones."}, "denial shows chat only");
    Receive(opened,reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    Bridge::Frame(window,nullptr,&shop);
    Check(!shop.Visible() && sent.size()==1,"approval for denied request is stale");

    Bridge::Test::Submit(context,0x1f03,command.data(),5,6);
    Bridge::Frame(window,nullptr,&shop);
    Check(EventShopWire::ParseRequest(sent.back().text,request) && request.action==EventShopWire::Action::Open,
        "retry checks current position");
    Receive("EPS1 opened "+std::to_string(request.nonce),reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    Check(Bridge::Test::Incoming()==1 && displays==4, "authentic shop reply hidden and queued");
    Bridge::Frame(window,nullptr,&shop);
    Check(!shop.Visible() && EventShopWire::ParseRequest(sent.back().text,request) && request.action==EventShopWire::Action::List,
        "approved open waits for safe catalog");
    const auto reply="EPS1 state "+std::to_string(request.nonce)+" 0 1 1 30 1";
    Receive(reply,reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    Bridge::Frame(window,nullptr,&shop);
    const EventShopWire::Offer sword{2,2621591,1,10,123,"Split Sword","Melee Attack +10%"};
    Receive(EventShopWire::Row(request.nonce,0,sword),reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    Bridge::Frame(window,nullptr,&shop);
    Check(!shop.Ready() && !shop.Buy(0), "bridge row waits for description");
    Receive(EventShopWire::Detail(request.nonce,0,sword),reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    Bridge::Frame(window,nullptr,&shop);
    Check(shop.Visible() && shop.Ready() && shop.Balance()==30 && !window.Ready() && shop.Offers()[0].description=="Melee Attack +10%",
        "shop details reach only shop");
    shop.Load(0); Bridge::Frame(window,nullptr,&shop);
    Check(EventShopWire::ParseRequest(sent.back().text,request), "full page request serialized");
    Receive("EPS1 state "+std::to_string(request.nonce)+" 0 1 6 30 1",reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    for(unsigned i=0;i<6;++i) {
        auto full=sword; full.id=static_cast<int>(i+1); full.description.assign(80,'X');
        Receive(EventShopWire::Row(request.nonce,i,full),reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
        Receive(EventShopWire::Detail(request.nonce,i,full),reinterpret_cast<void*>(0x402f14),3,0,"[EventShop]");
    }
    Check(Bridge::Test::Incoming()==13,"full six-offer page with descriptions fits receive queue");
    Bridge::Frame(window,nullptr,&shop);
    Check(shop.Ready() && shop.Offers().size()==6 && shop.Offers()[5].description.size()==80,"full description page assembled");
    Receive(".epshop",reinterpret_cast<void*>(0x5ece1e),0,100,"LocalPlayer");
    Check(displays==4,"shop command local echo suppressed");
    auto ahk=Say(".ahk"); Bridge::Test::Submit(context,0x1f03,ahk.data(),5,6);
    Bridge::Frame(window,nullptr,&shop);
    Check(window.Visible() && !shop.Visible(), "AHK switch closes shop");
    Bridge::Test::Session(otherContext,101); Bridge::Frame(window,nullptr,&shop);
    Check(!shop.Ready() && !shop.Visible() && shop.Balance()==0,"account transition clears shop state");
}
}

void EventNotice::ShowLocal(const char* message) { localNotices.emplace_back(message); }

extern "C" LONG WINAPI DetourAttach(PVOID*,PVOID) {return NO_ERROR;}
int main() {
    static_assert(sizeof(void*)==4,"These tests verify the actual Win32 bridge ABI");
    CommandAndFraming();PassThroughAndBounds();IngressGuardsAndAbi();ThreadSessionAndQueue();AccountTransitionWhileHidden();LocalOpenEcho();
    GlobalChatLocalEcho();
    EventShopTransport();
    DropBuffProtocolAndCountdown();DropBuffTransport();
    HunterKitStopSignal();
    std::printf("AHK native chat bridge: %u checks passed. No game code executed.\n",checks);
    return 0;
}
