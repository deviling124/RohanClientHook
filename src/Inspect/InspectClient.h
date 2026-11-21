#pragma once
#include "../../shared/InspectProtocol.h"
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Client side of the inspect protocol (shared/InspectProtocol.h): one request at a time, reply collection, timeout. No game or Windows
// dependencies, so it is unit tested on its own (tests/InspectClientTests.cpp). The window code reads it.
namespace InspectClient {

enum class State { Idle, Waiting, Ready, Failed };
// Reasons a request does not complete that the server never sends: 100 = no answer in time, 101 = asked again too soon, 102 = bad name.
constexpr int ErrorTimeout = 100, ErrorTooSoon = 101, ErrorBadName = 102;
constexpr unsigned long long TimeoutMs = 6000;

class Client {
public:
    // Starts a request for `name`. Returns false (the state is left alone, see rejected()) when the name is invalid or the last request was less than a second ago.
    bool Request(const std::string& name, unsigned long long nowMs) {
        InspectWire::Request parsed;
        if (!InspectWire::ParseRequest(InspectWire::BuildRequest(1, name), parsed) || parsed.name != name) { rejected_ = ErrorBadName; return false; }
        if (state_ == State::Waiting || (started_ && nowMs >= startedAt_ && nowMs - startedAt_ < InspectWire::MinRequestGapMs)) { rejected_ = ErrorTooSoon; return false; }
        rejected_ = 0;
        nonce_ = nonce_ >= 0x7fffffffu ? 1u : nonce_ + 1;
        profile_ = InspectWire::Profile{};
        profile_.nonce = nonce_;
        profile_.name = name;
        state_ = State::Waiting;
        started_ = true;
        startedAt_ = nowMs;
        error_ = 0;
        commands_.push_back(InspectWire::BuildRequest(nonce_, name));
        return true;
    }
    // One reply line from the server ("INS1 ...").
    void Receive(std::string_view line) {
        if (state_ != State::Waiting) return;
        if (!InspectWire::Apply(profile_, line)) return;
        if (!profile_.done) return;
        if (InspectWire::Complete(profile_)) state_ = State::Ready;
        else { error_ = profile_.error ? profile_.error : 2; state_ = State::Failed; }
        update_ = true;
    }
    void Tick(unsigned long long nowMs) {
        if (state_ == State::Waiting && nowMs >= startedAt_ && nowMs - startedAt_ > TimeoutMs) Fail(ErrorTimeout);
    }
    std::vector<std::string> TakeCommands() {
        std::vector<std::string> out(commands_.begin(), commands_.end());
        commands_.clear();
        return out;
    }
    // True once after a request finished (Ready or Failed).
    bool TakeUpdate() { const bool value = update_; update_ = false; return value; }
    void Reset() { state_ = State::Idle; commands_.clear(); update_ = false; error_ = 0; started_ = false; }
    // Debug: the next finished request also reports the equipment slots that hold something (`.inspect <name> slots`).
    void SetDebug(bool value) { debug_ = value; }
    bool TakeDebug() { const bool value = debug_; debug_ = false; return value; }
    State state() const { return state_; }
    int error() const { return error_; }
    // Why the last Request() returned false (ErrorBadName / ErrorTooSoon); the running request, if any, is not touched.
    int rejected() const { return rejected_; }
    const InspectWire::Profile& profile() const { return profile_; }

private:
    void Fail(int code) { error_ = code; state_ = State::Failed; update_ = true; }
    State state_ = State::Idle;
    InspectWire::Profile profile_;
    std::deque<std::string> commands_;
    std::uint32_t nonce_ = 0;
    unsigned long long startedAt_ = 0;
    bool started_ = false, update_ = false, debug_ = false;
    int error_ = 0, rejected_ = 0;
};

inline Client& Shared() {
    static Client client;
    return client;
}

// Names for the numbers the server sends (inspect_names.txt in the game folder; the numbers have no names in any table the client can read):
//   type   <live character type>  <Race>|<Class>     e.g. type 0x30104 Human|Guardian
//   dbtype <TCharacter.ctype_id>  <Race>|<Class>      (used when `type` has no line)
//   grade  <guild grade>          <text>              shown as Rank
//   title  <title id>             <text>              shown as Title
// '#' starts a comment; numbers are decimal or 0x hex. `.inspect <name> slots` prints the numbers of a character.
class NameTable {
public:
    void Parse(std::string_view text) {
        types_.clear(); dbTypes_.clear(); grades_.clear(); titles_.clear();
        while (!text.empty()) {
            const auto end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == text.npos ? std::string_view() : text.substr(end + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (line.empty() || line.front() == '#') continue;
            const auto a = line.find(' ');
            if (a == line.npos) continue;
            const std::string kind(line.substr(0, a));
            line.remove_prefix(a + 1);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            const auto b = line.find(' ');
            if (b == line.npos) continue;
            const std::string number(line.substr(0, b));
            std::string value(line.substr(b + 1));
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            char* stop = nullptr;
            const long long n = std::strtoll(number.c_str(), &stop, 0);
            if (!stop || *stop || value.empty()) continue;
            if (kind == "type" || kind == "dbtype") {
                const auto bar = value.find('|');
                Pair pair{bar == value.npos ? value : value.substr(0, bar), bar == value.npos ? std::string() : value.substr(bar + 1)};
                (kind == "type" ? types_ : dbTypes_)[static_cast<unsigned>(n)] = pair;
            } else if (kind == "grade") grades_[static_cast<int>(n)] = value;
            else if (kind == "title") titles_[static_cast<unsigned>(n)] = value;
        }
    }
    // Race and class of a character ("" when the table does not know it): the live type first, then the database type.
    std::string Race(unsigned type, int dbType) const { const Pair* p = Find(type, dbType); return p ? p->race : std::string(); }
    std::string Class(unsigned type, int dbType) const { const Pair* p = Find(type, dbType); return p ? p->klass : std::string(); }
    std::string Rank(int grade) const { const auto it = grades_.find(grade); return it == grades_.end() ? std::string() : it->second; }
    std::string Title(unsigned id) const { const auto it = titles_.find(id); return it == titles_.end() ? std::string() : it->second; }

private:
    struct Pair { std::string race, klass; };
    const Pair* Find(unsigned type, int dbType) const {
        auto it = types_.find(type);
        if (it != types_.end()) return &it->second;
        auto jt = dbTypes_.find(static_cast<unsigned>(dbType));
        return jt == dbTypes_.end() ? nullptr : &jt->second;
    }
    std::map<unsigned, Pair> types_, dbTypes_;
    std::map<int, std::string> grades_;
    std::map<unsigned, std::string> titles_;
};

// The text shown for a failed request.
inline const char* ErrorText(int code) {
    switch (code) {
    case 1: return "Inspect is turned off on this server.";
    case 2: return "That player was not found (they must be online on this map server).";
    case 3: case ErrorTooSoon: return "Please wait a moment before inspecting again.";
    case 4: case ErrorBadName: return "Usage: .inspect <player name>";
    case 5: return "That player does not allow inspection.";
    case 6: return "The player is busy, try again.";
    case ErrorTimeout: return "The server did not answer. Is the server updated?";
    default: return "Inspect failed.";
    }
}
}  // namespace InspectClient
