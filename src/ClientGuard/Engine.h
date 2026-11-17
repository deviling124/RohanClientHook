#pragma once
// Client side of the encrypted channel, independent of any game address or detour so it can be
// tested with fakes. The detour layer (ClientGuard.cpp) feeds it frames; see
// docs/ENCRYPTED_CHANNEL_DESIGN.md and docs/TRANSPORT_MAPPING.md.
#include "../../shared/NovaChannel.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ClientGuard {

enum class Mode {
    Disabled, // no hooks, no behavior change
    Audit,    // take part when the server offers the channel; play on without it otherwise
    Enforce,  // additionally refuse a server that does not offer the channel
};
const char* ModeName(Mode mode);
Mode ParseMode(const char* text); // anything unrecognized is Disabled

struct Config {
    Mode mode = Mode::Disabled;
    std::vector<NovaChannel::PublicKey> pinned;          // trusted server identity keys
    std::function<NovaChannel::Bytes()> attestation;     // opaque bytes sent in the client hello
};

class Engine {
public:
    // `send` pushes one plaintext frame through the game's own send path (the hook then sees it
    // again as outbound traffic). It is never called while the engine's lock is held.
    using SendFn = std::function<bool(void* conn, std::uint16_t opcode, const std::uint8_t* body, std::size_t n)>;
    using LogFn = std::function<void(const char*)>;
    Engine(Config config, SendFn send, LogFn log);

    enum class Inbound {
        Pass,    // leave the frame as the game received it
        Consume, // drop the frame; the game never sees it
        Replace, // use `replacement` as the frame body (length shrinks)
        Fail,    // the connection cannot continue; the caller reports a read error
    };
    Inbound OnInbound(void* conn, std::uint16_t opcode, const std::uint8_t* body, std::size_t n,
                      NovaChannel::Bytes& replacement);

    enum class Outbound {
        Pass,    // send the frame unchanged
        Replace, // send `replacement` as the body (length grows by the tag)
        Refuse,  // do not send
    };
    Outbound OnOutbound(void* conn, std::uint16_t opcode, const std::uint8_t* body, std::size_t n,
                        NovaChannel::Bytes& replacement);

    void Forget(void* conn);
    Mode mode() const { return config_.mode; }
    // For tests and status lines.
    bool Active(void* conn) const;

private:
    enum class Phase { Idle, HelloSent, Keyed, Active, Failed };
    struct Conn {
        explicit Conn(const std::vector<NovaChannel::PublicKey>& pinned)
            : session(NovaChannel::Session::MakeClient(pinned)) {}
        NovaChannel::Session session;
        Phase phase = Phase::Idle;
    };
    struct Pending { std::uint16_t opcode; NovaChannel::Bytes body; bool critical; };

    void Say(const char* text) const { if (log_) log_(text); }
    static bool OfferPresent(const std::uint8_t* body, std::size_t n);

    Config config_;
    SendFn send_;
    LogFn log_;
    mutable std::mutex lock_;
    std::map<void*, std::unique_ptr<Conn>> conns_;
};

} // namespace ClientGuard
