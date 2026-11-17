#include "Engine.h"
#include <cstdio>
#include <cstring>

namespace ClientGuard {
using namespace NovaChannel;

const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Disabled: return "disabled";
    case Mode::Audit: return "audit";
    case Mode::Enforce: return "enforce";
    }
    return "disabled";
}
Mode ParseMode(const char* text) {
    if (!text) return Mode::Disabled;
    if (!std::strcmp(text, "audit")) return Mode::Audit;
    if (!std::strcmp(text, "enforce")) return Mode::Enforce;
    return Mode::Disabled;
}

Engine::Engine(Config config, SendFn send, LogFn log)
    : config_(std::move(config)), send_(std::move(send)), log_(std::move(log)) {}

bool Engine::OfferPresent(const std::uint8_t* body, std::size_t n) {
    if (!body || n != StockKeyFrameBytes) return false;
    const std::uint8_t* p = body + OfferOffset;
    return p[0] == OfferMagic[0] && p[1] == OfferMagic[1] && p[2] == OfferMagic[2] && p[3] == Version;
}

bool Engine::Active(void* conn) const {
    std::lock_guard<std::mutex> guard(lock_);
    const auto it = conns_.find(conn);
    return it != conns_.end() && it->second->phase == Phase::Active;
}

void Engine::Forget(void* conn) {
    std::lock_guard<std::mutex> guard(lock_);
    conns_.erase(conn);
}

Engine::Inbound Engine::OnInbound(void* conn, std::uint16_t opcode, const std::uint8_t* body, std::size_t n,
                                  Bytes& replacement) {
    if (config_.mode == Mode::Disabled) return Inbound::Pass;
    std::vector<Pending> sends;
    Inbound result = Inbound::Pass;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (opcode == OpcodeStockKey) {
            // First frame of every connection: start from a clean state.
            auto fresh = std::make_unique<Conn>(config_.pinned);
            if (OfferPresent(body, n)) {
                Bytes hello;
                const Bytes attestation = config_.attestation ? config_.attestation() : Bytes{};
                if (fresh->session.ClientBuildHello(attestation, hello) == Status::Ok) {
                    fresh->phase = Phase::HelloSent;
                    sends.push_back({OpcodeClientHello, std::move(hello), false});
                } else {
                    fresh->phase = Phase::Failed;
                    result = Inbound::Fail;
                    Say("channel: could not build the client hello");
                }
            } else if (config_.mode == Mode::Enforce) {
                fresh->phase = Phase::Failed;
                result = Inbound::Fail;
                Say("channel: the server does not offer the encrypted channel (enforce)");
            }
            conns_[conn] = std::move(fresh);
        } else {
            const auto it = conns_.find(conn);
            if (it == conns_.end()) return Inbound::Pass;
            Conn& c = *it->second;
            switch (c.phase) {
            case Phase::Idle:
                break;
            case Phase::HelloSent:
                if (opcode == OpcodeServerHello) {
                    const Status s = c.session.ClientAcceptServerHello(body, n);
                    if (s == Status::Ok) {
                        c.phase = Phase::Keyed;
                        sends.push_back({OpcodeChannelReady, {}, true});
                        result = Inbound::Consume;
                    } else {
                        c.phase = Phase::Failed;
                        result = Inbound::Fail;
                        char text[96];
                        std::snprintf(text, sizeof text, "channel: server hello rejected (%s)", StatusName(s));
                        Say(text);
                    }
                }
                break;
            case Phase::Keyed:
            case Phase::Active:
                // Everything after the server hello is sealed, whatever its opcode.
                if (opcode == OpcodeServerHello) {
                    c.phase = Phase::Failed;
                    result = Inbound::Fail;
                    Say("channel: unexpected second server hello");
                } else {
                    const Status s = c.session.Open(opcode, body, n, replacement);
                    if (s == Status::Ok) {
                        result = Inbound::Replace;
                    } else {
                        c.phase = Phase::Failed;
                        result = Inbound::Fail;
                        char text[96];
                        std::snprintf(text, sizeof text, "channel: inbound frame rejected (%s)", StatusName(s));
                        Say(text);
                    }
                }
                break;
            case Phase::Failed:
                result = Inbound::Fail;
                break;
            }
        }
    }
    // Pushed outside the lock: the send goes back through the outbound hook.
    for (const Pending& p : sends) {
        const bool sent = send_ && send_(conn, p.opcode, p.body.data(), p.body.size());
        if (sent) continue;
        std::lock_guard<std::mutex> guard(lock_);
        const auto it = conns_.find(conn);
        if (it == conns_.end()) continue;
        if (p.critical) {
            it->second->phase = Phase::Failed; // the server now expects sealed frames we cannot start
            result = Inbound::Fail;
            Say("channel: could not send the channel-ready frame");
        } else {
            it->second->phase = Phase::Idle;   // the server never saw a hello; it keeps playing plain
            Say("channel: could not send the client hello; continuing without the channel");
        }
    }
    return result;
}

Engine::Outbound Engine::OnOutbound(void* conn, std::uint16_t opcode, const std::uint8_t* body, std::size_t n,
                                    Bytes& replacement) {
    if (config_.mode == Mode::Disabled) return Outbound::Pass;
    std::lock_guard<std::mutex> guard(lock_);
    const auto it = conns_.find(conn);
    if (it == conns_.end()) return Outbound::Pass;
    Conn& c = *it->second;
    switch (c.phase) {
    case Phase::Idle:
    case Phase::HelloSent:
        return Outbound::Pass;
    case Phase::Keyed:
        // Frames pushed before the ready frame stay plaintext (the server accepts them);
        // the ready frame itself is the first sealed one and starts sealing for everything after.
        if (opcode != OpcodeChannelReady) return Outbound::Pass;
        if (c.session.Seal(opcode, nullptr, 0, replacement) != Status::Ok) {
            c.phase = Phase::Failed;
            return Outbound::Refuse;
        }
        c.phase = Phase::Active;
        return Outbound::Replace;
    case Phase::Active: {
        if (opcode == OpcodeChannelReady) return Outbound::Refuse; // sent exactly once
        const Status s = c.session.Seal(opcode, body, n, replacement);
        if (s == Status::Ok) return Outbound::Replace;
        if (s == Status::TooLarge) {
            Say("channel: outbound frame too large to seal; not sent");
            return Outbound::Refuse; // does not end the session
        }
        c.phase = Phase::Failed;
        return Outbound::Refuse;
    }
    case Phase::Failed:
        return Outbound::Refuse;
    }
    return Outbound::Pass;
}

} // namespace ClientGuard
