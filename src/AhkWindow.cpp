#include "AhkWindow.h"
#include <algorithm>
#include <utility>
#ifndef AHK_WINDOW_MODEL_ONLY
#include "imgui.h"
#endif

namespace {
bool Same(const Ahk::Settings& a,const Ahk::Settings& b) {
    if(a.active!=b.active) return false;
    for(unsigned c=0;c<Ahk::CategoryCount;++c) {
        if(a.rows[c].mask!=b.rows[c].mask) return false;
        for(unsigned s=0;s<Ahk::StatCount;++s)
            if((a.rows[c].mask & (1u<<s)) && a.rows[c].minimum[s]!=b.rows[c].minimum[s]) return false;
    }
    return true;
}
std::string ErrorMessage(std::string_view code) {
    if(code=="vip" || code=="denied" || code=="ineligible" || code=="vip_required") return "VIP access is required. Refresh after your access changes.";
    if(code=="disabled") return "Advanced Hunter Kit is disabled by the server.";
    if(code=="storage" || code=="save" || code=="io" || code=="save_failed") return "The server could not save your settings. Refresh and try again.";
    if(code=="expired") return "The settings request expired. Refresh and try again.";
    if(code=="transaction") return "The settings update was incomplete or expired. Refresh and try again.";
    if(code=="busy") return "The server is busy saving settings. Refresh and try again shortly.";
    if(code=="unavailable") return "Account settings are temporarily unavailable. Refresh and try again.";
    return "The server rejected the settings request. Refresh and try again.";
}
}

std::uint32_t AhkWindow::NextNonce() {
    static std::uint32_t next=static_cast<std::uint32_t>(Clock::now().time_since_epoch().count());
    if(++next==0) ++next;
    return next;
}
void AhkWindow::Open() {
    visible_=true;
    if(phase_!=Phase::Saving) StartLoad();
}
void AhkWindow::Close() {
    visible_=false;
    if(phase_!=Phase::Saving) draft_=accepted_;
}
void AhkWindow::ResetSession() {
    visible_=false;eligible_=false;synchronized_=false;stateReceived_=false;
    phase_=Phase::Idle;nonce_=0;rowsReceived_=0;category_=0;
    accepted_=Ahk::Defaults();draft_=accepted_;incoming_={};pending_={};
    deadline_={};commands_.clear();
    status_="Type .ahk in Say chat to open settings.";
}
void AhkWindow::StartLoad() {
    nonce_=NextNonce();
    phase_=Phase::Loading; synchronized_=false; eligible_=false; stateReceived_=false; rowsReceived_=0;
    incoming_={}; commands_.clear();
    status_="Loading your account settings...";
    deadline_=Clock::now()+std::chrono::seconds(10);
    commands_.push_back(".ahkui1 get "+std::to_string(nonce_));
}
void AhkWindow::Fail(const std::string& message) {
    status_=message; phase_=Phase::Failed; synchronized_=false;
}
bool AhkWindow::Dirty() const { return Ready() && !Same(draft_,accepted_); }
void AhkWindow::Tick() {
    if(Busy() && Clock::now()>deadline_) {
        const bool saving=phase_==Phase::Saving;
        Fail(saving?"No save confirmation received. Refresh to check the server's saved settings.":
            "The server did not return settings. Check that both hooks are installed, then refresh.");
        commands_.clear();
    }
}
std::vector<std::string> AhkWindow::TakeCommands() {
    Tick();
    auto result=std::move(commands_); commands_.clear(); return result;
}
void AhkWindow::Receive(std::string_view text) {
    Tick();
    if(text.size()>256 || text.substr(0,7)!="AHKUI1 ") return;
    const auto words=Ahk::Words(text);
    std::uint32_t token=0;
    if(words.size()<3 || !Ahk::Number(words[1],token) || token!=nonce_ || !Busy()) return;
    if(words[2]=="error" && words.size()==4) { Fail(ErrorMessage(words[3])); return; }
    if(phase_==Phase::Saving) {
        if(words[2]=="saved" && words.size()==3) {
            accepted_=pending_; draft_=pending_; synchronized_=true; phase_=Phase::Ready;
            status_="Settings saved to your account.";
        }
        return;
    }
    if(words[2]=="state" && words.size()==5) {
        std::uint32_t access=0,active=0;
        if(stateReceived_ || !Ahk::Number(words[3],access) || access>1 || !Ahk::Number(words[4],active) || active>1) {
            Fail("Invalid settings response. Refresh to try again."); return;
        }
        stateReceived_=true; eligible_=access!=0; incoming_.active=active!=0;
    } else if(words[2]=="row" && words.size()==5) {
        std::uint32_t category=0;
        Ahk::Row row;
        if(!stateReceived_ || !Ahk::Number(words[3],category) || category>=Ahk::CategoryCount ||
           (rowsReceived_ & (1u<<category)) || !Ahk::Decode(words[4],row)) {
            Fail("Invalid or incomplete filter response. Refresh to try again."); return;
        }
        incoming_.rows[category]=row;
        rowsReceived_|=static_cast<std::uint16_t>(1u<<category);
    } else if(words[2]=="end" && words.size()==3) {
        if(!stateReceived_ || rowsReceived_!=0xff || !Ahk::Valid(incoming_)) {
            Fail("Some filters were not received. Refresh before editing."); return;
        }
        accepted_=incoming_; draft_=incoming_; synchronized_=true; phase_=Phase::Ready;
        status_=eligible_?"Your saved settings are loaded.":"VIP access is required to edit these settings.";
    }
}
bool AhkWindow::Apply() {
    Tick();
    if(!Ready() || !eligible_ || !Ahk::Valid(draft_) || !Dirty()) return false;
    nonce_=NextNonce(); pending_=draft_; phase_=Phase::Saving;
    deadline_=Clock::now()+std::chrono::seconds(10);
    status_="Saving your settings...";
    const auto token=std::to_string(nonce_);
    commands_.push_back(".ahkui1 begin "+token+" "+(pending_.active?"1":"0"));
    for(unsigned c=0;c<Ahk::CategoryCount;++c)
        commands_.push_back(".ahkui1 row "+token+" "+std::to_string(c)+" "+Ahk::Encode(pending_.rows[c]));
    commands_.push_back(".ahkui1 commit "+token);
    return true;
}
void AhkWindow::RestoreDefaults() {
    if(Ready() && eligible_) {
        const bool active=draft_.active;
        draft_=Ahk::Defaults(); draft_.active=active;
        status_="Default filters restored in the editor. Click Apply to save.";
    }
}
void AhkWindow::CancelChanges() {
    if(!Busy()) { draft_=accepted_; status_="Unsaved changes discarded."; }
}

#ifndef AHK_WINDOW_MODEL_ONLY
void AhkWindow::Draw() {
    Tick();
    if(!visible_) return;
    ImGui::SetNextWindowSize(ImVec2(730,670),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(600,580),ImVec2(1100,1000));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(22,20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,10);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,5);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10,9));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.055f,.070f,.085f,.98f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,ImVec4(.065f,.085f,.105f,1));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,ImVec4(.09f,.12f,.145f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.12f,.15f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.14f,.21f,.23f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.19f,.34f,.35f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(.34f,.83f,.72f,1));
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.12f,.26f,.27f,1));
    bool open=true;
    if(ImGui::Begin("Advanced Hunter Kit###AhkSettings",&open,ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(.88f,.74f,.43f,1),"ADVANCED HUNTER KIT");
        ImGui::TextDisabled("Choose which option items your pet should collect.");
        ImGui::Separator();
        const bool editable=Ready() && eligible_;
        ImGui::BeginDisabled(!editable);
        ImGui::Checkbox("Enable automatic pickup filtering",&draft_.active);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(Ready()) {
            ImGui::TextColored(eligible_?ImVec4(.34f,.83f,.72f,1):ImVec4(.96f,.64f,.36f,1),
                eligible_?"  ACCESS AVAILABLE":"  ACCESS REQUIRED");
        }
        ImGui::Spacing();
        ImGui::TextWrapped("An item qualifies when any one selected stat meets its minimum. Unchecked stats are ignored.");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##Category",&category_,Ahk::Categories,Ahk::CategoryCount);
        auto& row=draft_.rows[static_cast<unsigned>(category_)];
        if(row.mask==0)
            ImGui::TextColored(ImVec4(.96f,.67f,.37f,1),"No stats selected: this category will be skipped while filtering is on.");
        else ImGui::TextDisabled("Minimums use the same stat units as the item options.");
        const float footer=150.f;
        ImGui::BeginChild("##StatEditor",ImVec2(0,-footer),ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(!editable);
        if(ImGui::BeginTable("##StatThresholds",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_PadOuterX|ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Use",ImGuiTableColumnFlags_WidthFixed,42);
            ImGui::TableSetupColumn("Item stat",ImGuiTableColumnFlags_WidthStretch,1.f);
            ImGui::TableSetupColumn("Minimum",ImGuiTableColumnFlags_WidthFixed,180);
            ImGui::TableHeadersRow();
            for(unsigned s=0;s<Ahk::StatCount;++s) {
                ImGui::PushID(static_cast<int>(s));
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                bool use=(row.mask & (1u<<s))!=0;
                if(ImGui::Checkbox("##Use",&use)) {
                    if(use) {
                        row.mask|=static_cast<std::uint16_t>(1u<<s);
                        if(row.minimum[s]==0) row.minimum[s]=1;
                    } else row.mask&=static_cast<std::uint16_t>(~(1u<<s));
                }
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(Ahk::Stats[s]);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!use);
                ImGui::SetNextItemWidth(-1);
                static_assert(sizeof(int)==sizeof(std::int32_t),"32-bit InputInt required");
                int value=row.minimum[s];
                if(ImGui::InputInt("##Minimum",&value,1,10)) row.minimum[s]=value;
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        if(!Ahk::Valid(draft_)) ImGui::TextColored(ImVec4(.98f,.52f,.42f,1),"Selected stats must have a nonzero minimum.");
        else if(Dirty()) ImGui::TextColored(ImVec4(.88f,.74f,.43f,1),"You have unsaved changes.");
        else ImGui::TextDisabled("Settings belong to your account and are saved on the server.");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(status_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::BeginDisabled(!editable || !Dirty() || !Ahk::Valid(draft_));
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.12f,.39f,.34f,1));
        if(ImGui::Button("Apply",ImVec2(105,32))) Apply();
        ImGui::PopStyleColor();
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(!editable);
        if(ImGui::Button("Defaults",ImVec2(105,32))) RestoreDefaults();
        ImGui::SetItemTooltip("Restore the standard filter thresholds. Your server's initial settings may differ.");
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(Busy());
        if(ImGui::Button("Refresh",ImVec2(105,32))) StartLoad();
        ImGui::EndDisabled(); ImGui::SameLine();
        if(ImGui::Button(Busy()?"Close":"Cancel",ImVec2(105,32))) {CancelChanges();Close();}
    }
    ImGui::End();
    if(!open) Close();
    ImGui::PopStyleColor(8); ImGui::PopStyleVar(4);
}
#else
void AhkWindow::Draw() { Tick(); }
#endif
