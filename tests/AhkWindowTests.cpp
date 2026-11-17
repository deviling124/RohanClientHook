#include "../src/AhkWindow.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned checks=0;
void Check(bool condition,const char* label) {
    ++checks;
    if(!condition) {std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}
}
std::string Token(const std::vector<std::string>& commands) {
    Check(commands.size()==1,"open produces one settings request");
    const auto words=Ahk::Words(commands[0]);
    Check(words.size()==3 && words[0]==".ahkui1" && words[1]=="get","request grammar");
    return std::string(words[2]);
}
void Snapshot(AhkWindow& window,const std::string& token,const Ahk::Settings& settings,bool eligible=true) {
    window.Receive("AHKUI1 "+token+" state "+(eligible?"1":"0")+" "+(settings.active?"1":"0"));
    for(unsigned c=0;c<Ahk::CategoryCount;++c)
        window.Receive("AHKUI1 "+token+" row "+std::to_string(c)+" "+Ahk::Encode(settings.rows[c]));
    window.Receive("AHKUI1 "+token+" end");
}
void CompleteSnapshotAndSave() {
    AhkWindow window;
    Check(!window.Visible() && !window.Ready(),"initially hidden and unsynchronized");
    window.Open(); const auto token=Token(window.TakeCommands());
    Check(window.Visible() && window.Busy() && !window.Apply(),"cannot save while loading");
    window.Receive("AHKUI1 "+token+" saved");
    Check(!window.Ready(),"save ack cannot complete a load");
    Snapshot(window,token,Ahk::Defaults());
    Check(window.Ready() && window.Eligible() && !window.Dirty(),"complete authorized snapshot ready");
    Check(!window.Apply(),"unchanged snapshot does not write");
    auto* draft=window.EditDraft(); Check(draft!=nullptr,"authorized editing available");
    draft->active=true; draft->rows[0].minimum[2]=99;
    Check(window.Dirty() && window.Apply(),"edits start save");
    auto commands=window.TakeCommands();
    Check(commands.size()==10,"atomic save begin plus eight rows plus commit");
    for(const auto& command:commands) Check(command.size()<=160,"commands fit existing Say bound");
    const auto begin=Ahk::Words(commands.front());
    Check(begin.size()==4 && begin[1]=="begin" && begin[3]=="1","active preference sent");
    const std::string saving(begin[2]);
    Check(!window.Ready() && !window.EditDraft(),"editing disabled pending save ack");
    window.Receive("AHKUI1 "+token+" saved");
    Check(window.Busy(),"old nonce ack ignored");
    window.Close(); window.Open();
    Check(window.Busy() && window.TakeCommands().empty(),"reopening preserves in-flight save");
    window.Receive("AHKUI1 "+saving+" saved");
    Check(window.Ready() && !window.Dirty() && window.Draft().active && window.Draft().rows[0].minimum[2]==99,"save commits only on matching ack");
    window.EditDraft()->active=false;window.CancelChanges();
    Check(window.Draft().active && !window.Dirty(),"cancel restores confirmed settings");
}
void RejectIncompleteAndMalformed() {
    AhkWindow window;window.Open(); auto token=Token(window.TakeCommands());
    window.Receive("AHKUI1 "+token+" state 1 0");
    window.Receive("AHKUI1 "+token+" row 0 "+Ahk::Encode(Ahk::Defaults().rows[0]));
    window.Receive("AHKUI1 "+token+" end");
    Check(!window.Ready() && !window.Busy() && !window.EditDraft(),"partial snapshot fails closed");
    window.Open();token=Token(window.TakeCommands());
    window.Receive("AHKUI1 "+token+" state 1 0");
    window.Receive("AHKUI1 "+token+" row 0 "+Ahk::Encode(Ahk::Defaults().rows[0]));
    window.Receive("AHKUI1 "+token+" row 0 "+Ahk::Encode(Ahk::Defaults().rows[0]));
    Check(!window.Busy() && !window.Ready(),"duplicate category rejects response");
    window.Open();token=Token(window.TakeCommands());
    window.Receive("AHKUI1 "+token+" state 2 0");
    Check(!window.Ready() && !window.Busy(),"out of range eligibility rejected");
    window.Open();token=Token(window.TakeCommands());
    window.Receive("AHKUI1 "+token+" state 1 0");
    window.Receive("AHKUI1 "+token+" row 8 "+Ahk::Encode(Ahk::Defaults().rows[0]));
    Check(!window.Ready() && !window.Busy(),"out of range category rejected");
}
void EligibilityAndRejection() {
    AhkWindow window;window.Open();auto token=Token(window.TakeCommands());
    Snapshot(window,token,Ahk::Defaults(),false);
    Check(window.Ready() && !window.Eligible() && !window.EditDraft() && !window.Apply(),"ineligible account can inspect but cannot edit");
    window.Open();token=Token(window.TakeCommands());
    Snapshot(window,token,Ahk::Defaults());
    window.EditDraft()->active=true;window.Apply();
    const auto commands=window.TakeCommands();const auto begin=Ahk::Words(commands.front());
    window.Receive("AHKUI1 "+std::string(begin[2])+" error storage");
    Check(!window.Ready() && !window.Busy() && !window.Apply(),"save error requires fresh snapshot");
    window.Open();token=Token(window.TakeCommands());
    Snapshot(window,token,Ahk::Defaults());
    Check(!window.Draft().active,"refresh displays actual server state after failed save");
}
void SessionResetDuringSave() {
    AhkWindow window;window.Open();auto token=Token(window.TakeCommands());
    Snapshot(window,token,Ahk::Defaults());
    window.EditDraft()->active=true;window.EditDraft()->rows[0].minimum[2]=99;
    Check(window.Apply(),"session test starts save");
    const auto commands=window.TakeCommands();const auto begin=Ahk::Words(commands.front());
    const std::string oldSaving(begin[2]);
    window.ResetSession();
    Check(!window.Visible() && !window.Busy() && !window.Ready() && !window.Eligible(),"session reset abandons UI and authorization state");
    Check(window.TakeCommands().empty() && !window.Draft().active,"session reset clears commands and account settings");
    window.Receive("AHKUI1 "+oldSaving+" saved");
    Check(!window.Ready(),"old account save acknowledgement ignored after reset");
    window.Open();token=Token(window.TakeCommands());
    Check(token!=oldSaving,"new session starts a fresh settings request");
    window.Receive("AHKUI1 "+oldSaving+" saved");
    Check(window.Busy() && !window.Ready(),"late save acknowledgement cannot complete new account load");
    auto fresh=Ahk::Defaults();fresh.rows[0].minimum[2]=45;
    Snapshot(window,token,fresh);
    Check(window.Ready() && !window.Draft().active && window.Draft().rows[0].minimum[2]==45,"new account receives only its own confirmed settings");
}
void NonceAndValidation() {
    AhkWindow window;window.Open();const auto old=Token(window.TakeCommands());
    window.Close();window.Open();const auto token=Token(window.TakeCommands());
    Check(old!=token,"reopen changes request identity");
    Snapshot(window,old,Ahk::Defaults());
    Check(window.Busy() && !window.Ready(),"stale snapshot ignored");
    Snapshot(window,token,Ahk::Defaults());
    auto* draft=window.EditDraft();draft->rows[0].minimum[2]=0;
    Check(!window.Apply() && window.TakeCommands().empty(),"enabled zero minimum cannot be submitted");
    draft->rows[0].minimum[2]=(std::numeric_limits<std::int32_t>::min)();
    draft->rows[0].minimum[1]=(std::numeric_limits<std::int32_t>::max)();
    Check(window.Apply(),"signed 32-bit boundaries accepted");
    for(const auto& command:window.TakeCommands()) Check(command.size()<=160,"boundary settings still fit chat framing");
}
}
int main() {
    CompleteSnapshotAndSave();RejectIncompleteAndMalformed();EligibilityAndRejection();NonceAndValidation();SessionResetDuringSave();
    std::printf("AHK window model: %u checks passed.\n",checks);return 0;
}
