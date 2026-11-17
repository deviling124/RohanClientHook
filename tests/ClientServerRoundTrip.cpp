// Real client model + real server settings/file backend. No game/network needed.
#include "../src/AhkWindow.h"
#include "../../RohanMapServ_Hook/src/AdvancedHunterKit/UserSettings.h"
#include <windows.h>
#include <filesystem>
#include <cstdio>
namespace {
unsigned checks=0,failed=0;
void Check(bool ok,const char* label) {++checks;if(!ok){++failed;std::printf("FAIL:%s\n",label);}}
void Exchange(AhkWindow& client,int account,bool eligible=true) {
    for(const auto& command:client.TakeCommands()) {
        std::vector<std::string> replies;
        Check(command.size()<=160,"command fits native Say limit");
        Check(AdvancedHunterKit::HandleSettingsCommand(command,account,true,eligible,replies),"server consumes UI command");
        for(const auto& reply:replies)client.Receive(reply);
    }
}
}
int main() {
    const auto defaults=AdvancedHunterKit::ReferenceDefaults();
    const auto folder=std::filesystem::absolute(L"obj/Tests/RoundTrip-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Check(AdvancedHunterKit::ConfigureUserSettings(defaults,false,folder.wstring()),"configure actual folder backend");
    AhkWindow client;client.Open();Exchange(client,42);
    Check(client.Ready()&&client.Eligible()&&!client.Draft().active,"load complete actual server snapshot");
    auto* draft=client.EditDraft();Check(draft!=nullptr,"eligible account can edit");
    if(!draft)return 1;
    draft->active=true;draft->rows[6].minimum[11]=55;
    Check(client.Apply(),"begin real ten-command Apply");Exchange(client,42);
    Check(client.Ready()&&!client.Dirty()&&client.Status().find("saved")!=std::string::npos,"actual save acknowledgement commits client state");
    Check(std::filesystem::exists(folder/L"42.ini"),"account file persisted");
    Check(AdvancedHunterKit::UserActive(42),"server activated account42");
    Check(!AdvancedHunterKit::UserActive(43),"account43 activation independent");
    std::array<AdvancedHunterKit::Stat,14> stats{};stats[0]={0x1c,54};
    Check(!AdvancedHunterKit::MatchesUser(42,13,stats),"actual pickup rule rejects below new minimum");
    Check(AdvancedHunterKit::MatchesUser(43,13,stats),"other account retains XML rule");
    stats[0].value=55;
    Check(AdvancedHunterKit::MatchesUser(42,13,stats),"actual pickup rule accepts new minimum");
    Check(AdvancedHunterKit::ConfigureUserSettings(defaults,false,folder.wstring()),"simulate server restart and reload files");
    Check(AdvancedHunterKit::UserActive(42)&&AdvancedHunterKit::UserSettings(42).rows[6].minimum[11]==55,"saved activation and rules survive restart");
    client.ResetSession();client.Open();Exchange(client,43);
    Check(client.Ready()&&!client.Draft().active&&client.Draft().rows[6].minimum[11]==30,"new account loads its own untouched defaults");
    client.EditDraft()->rows[6].minimum[11]=999;client.CancelChanges();
    Check(!client.Dirty()&&client.TakeCommands().empty(),"Cancel sends no settings writes");
    client.ResetSession();client.Open();Exchange(client,42,false);
    Check(client.Ready()&&!client.Eligible()&&client.EditDraft()==nullptr,"actual entitlement response makes editor read-only");
    Check(!client.Apply()&&AdvancedHunterKit::UserSettings(42).rows[6].minimum[11]==55,"ineligible editor cannot overwrite saved rules");
    std::printf("Client/server folder round trip:%u checks,%u failures. No game/network executed.\n",checks,failed);
    return failed?1:0;
}
