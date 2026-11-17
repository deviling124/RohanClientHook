// Interop test: the REAL client engine (RohanClient_Hook/src/ClientGuard/Engine) talking to the REAL
// server engine (RohanMapServ_Hook/src/ServerGuard/Engine) through in-memory "sockets". This is what
// proves the two codebases agree on the offer format, the handshake order and the sealing rules.
// It does not touch the game, the detours or the network.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../src/ClientGuard/Engine.cpp"
#include "../../RohanMapServ_Hook/src/ServerGuard/Engine.cpp"
#include "../shared/NovaChannel.cpp"
#include "../shared/NovaChannelCng.cpp"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

using namespace NovaChannel;

namespace {
int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct Frame { std::uint16_t opcode; Bytes body; };
void* const Conn = reinterpret_cast<void*>(0x77);

// One end-to-end link with both engines and two queues.
struct Link {
    Bytes priv;
    PublicKey pub{};
    std::deque<Frame> toClient, toServer;
    std::vector<Frame> clientGame, serverGame; // what each game would see
    std::unique_ptr<ClientGuard::Engine> client;
    std::unique_ptr<ServerGuard::Engine> server;
    ClientGuard::Mode clientMode;

    explicit Link(ClientGuard::Mode cm, ServerGuard::Mode sm) : clientMode(cm) {
        Check(Crypto::EcdsaGenerate(priv, pub), "identity");
        ClientGuard::Config cc;
        cc.mode = cm;
        cc.pinned = {pub};
        cc.attestation = [] { return Text("client-attestation"); };
        client = std::make_unique<ClientGuard::Engine>(cc,
            [this](void* conn, std::uint16_t op, const std::uint8_t* b, std::size_t n) { ClientPush(conn, op, Bytes(b, b + n)); return true; },
            [](const char* t) { std::printf("  client: %s\n", t); });
        ServerGuard::Config sc;
        sc.mode = sm;
        sc.identityPrivateBlob = priv;
        server = std::make_unique<ServerGuard::Engine>(sc, [](const char* t) { std::printf("  server: %s\n", t); });
    }
    // The client's send path (PushSend hook): outbound transform, then onto the wire.
    void ClientPush(void* conn, std::uint16_t op, const Bytes& body) {
        Bytes replacement;
        switch (client->OnOutbound(conn, op, body.data(), body.size(), replacement)) {
        case ClientGuard::Engine::Outbound::Pass: toServer.push_back({op, body}); break;
        case ClientGuard::Engine::Outbound::Replace: toServer.push_back({op, replacement}); break;
        case ClientGuard::Engine::Outbound::Refuse: break;
        }
    }
    // The server's send path (SendPacketEx hook).
    bool ServerPush(std::uint16_t op, const Bytes& body) {
        return server->Send(Conn, op, body.data(), body.size(),
            [this](std::uint16_t o, const std::uint8_t* b, std::size_t n) { toClient.push_back({o, Bytes(b, b + n)}); });
    }
    // The client's receive loop (ReadFrame hook). Returns false when a frame failed the connection.
    bool ClientReceiveAll() {
        while (!toClient.empty()) {
            Frame f = std::move(toClient.front());
            toClient.pop_front();
            for (;;) {
                Bytes replacement;
                using In = ClientGuard::Engine::Inbound;
                const In r = client->OnInbound(Conn, f.opcode, f.body.data(), f.body.size(), replacement);
                if (r == In::Pass) { clientGame.push_back(f); break; }
                if (r == In::Consume) break;
                if (r == In::Replace) { clientGame.push_back({f.opcode, replacement}); break; }
                return false;
            }
        }
        return true;
    }
    // The server's receive entry (0x418CE0 hook).
    bool ServerReceiveAll() {
        while (!toServer.empty()) {
            Frame f = std::move(toServer.front());
            toServer.pop_front();
            Bytes plain;
            const auto raw = [this](std::uint16_t o, const std::uint8_t* b, std::size_t n) { toClient.push_back({o, Bytes(b, b + n)}); };
            using Rx = ServerGuard::Engine::Receive;
            const Rx r = server->OnReceive(Conn, f.opcode, f.body.data(), f.body.size(), plain, raw);
            if (r == Rx::Pass) serverGame.push_back(f);
            else if (r == Rx::Replace) serverGame.push_back({f.opcode, plain});
            else if (r == Rx::Fail) return false;
        }
        return true;
    }
    void Pump() { for (int i = 0; i < 4; ++i) { ClientReceiveAll(); ServerReceiveAll(); } }
    // The stock server's first frame.
    void Connect() {
        Bytes key(StockKeyFrameBytes, 0xEE);
        std::memset(key.data(), 0, 40);
        std::memcpy(key.data(), "0106F079-AE41-4AB9-870D-31183E7528F6", 36);
        ServerPush(OpcodeStockKey, key);
    }
};

void FullSessionBetweenTheRealEngines() {
    Link link(ClientGuard::Mode::Audit, ServerGuard::Mode::Audit);
    link.Connect();
    link.Pump();
    Check(link.server->Active(Conn) && link.client->Active(Conn), "both engines reach the active state through the real wire formats");
    Check(link.clientGame.size() == 1 && link.clientGame[0].opcode == OpcodeStockKey, "the client's game saw only the stock key frame");
    Check(link.serverGame.empty(), "the server's game saw no handshake frames");

    for (int i = 0; i < 100; ++i) {
        const Bytes down = Text("server says " + std::to_string(i)), up = Text("client says " + std::to_string(i));
        Check(link.ServerPush(0x1A44, down), "server sends");
        link.ClientPush(Conn, 0x3001, up);
        Check(link.ClientReceiveAll() && link.ServerReceiveAll(), "both directions accepted");
        Check(link.clientGame.back().body == down && link.serverGame.back().body == up, "plaintext arrives intact both ways");
    }
    // What is on the wire is not the plaintext.
    link.ServerPush(0x1A44, Text("secret-secret-secret"));
    Check(std::string(link.toClient.back().body.begin(), link.toClient.back().body.end()).find("secret") == std::string::npos,
          "the wire carries ciphertext, not the message");
}

void ClientAndServerModeMatrix() {
    {   // channel-capable server, client off: both sides keep playing plaintext
        Link link(ClientGuard::Mode::Disabled, ServerGuard::Mode::Audit);
        link.Connect();
        link.Pump();
        Check(!link.server->Active(Conn) && !link.client->Active(Conn), "a disabled client never starts the handshake");
        link.ServerPush(0x1A44, Text("plain"));
        link.ClientPush(Conn, 0x3001, Text("plain up"));
        Check(link.ClientReceiveAll() && link.ServerReceiveAll(), "plaintext keeps working");
        Check(link.clientGame.back().body == Text("plain") && link.serverGame.back().body == Text("plain up"), "and arrives unchanged");
    }
    {   // stock server (no offer), client in audit: nothing happens
        Link link(ClientGuard::Mode::Audit, ServerGuard::Mode::Disabled);
        link.Connect();
        link.Pump();
        Check(link.toServer.empty() && !link.client->Active(Conn), "a stock server is never sent a hello");
        link.ServerPush(0x1A44, Text("stock"));
        Check(link.ClientReceiveAll() && link.clientGame.back().body == Text("stock"), "and its traffic is left alone");
    }
    {   // stock server, client in enforce: the client refuses
        Link link(ClientGuard::Mode::Enforce, ServerGuard::Mode::Disabled);
        link.Connect();
        Check(!link.ClientReceiveAll(), "an enforcing client rejects a server that does not offer the channel");
    }
    {   // wrong pinned key on the client
        Link link(ClientGuard::Mode::Audit, ServerGuard::Mode::Audit);
        Bytes otherPriv;
        PublicKey otherPub{};
        Check(Crypto::EcdsaGenerate(otherPriv, otherPub), "second identity");
        ClientGuard::Config cc;
        cc.mode = ClientGuard::Mode::Audit;
        cc.pinned = {otherPub};
        link.client = std::make_unique<ClientGuard::Engine>(cc,
            [&link](void* conn, std::uint16_t op, const std::uint8_t* b, std::size_t n) { link.ClientPush(conn, op, Bytes(b, b + n)); return true; },
            [](const char*) {});
        link.Connect();
        link.ClientReceiveAll();
        link.ServerReceiveAll();
        Check(!link.ClientReceiveAll(), "a client that pins a different key fails the connection at the server hello");
    }
}

void TrafficBeforeReady() {
    // The client's game sends a frame after the hello but before the server hello arrives. It is
    // plaintext, the server accepts it while waiting for the ready frame, and ordering holds.
    Link link(ClientGuard::Mode::Audit, ServerGuard::Mode::Audit);
    link.Connect();
    link.ClientReceiveAll();                      // client sees the offer and sends the hello
    link.ClientPush(Conn, 0x4001, Text("early")); // game frame in the window before the server hello
    Check(link.ServerReceiveAll(), "server handles the hello and the early frame");
    Check(link.serverGame.size() == 1 && link.serverGame[0].body == Text("early"), "the early plaintext frame reached the server's game");
    Check(link.ClientReceiveAll() && link.ServerReceiveAll(), "handshake completes");
    Check(link.server->Active(Conn) && link.client->Active(Conn), "and both are active afterwards");
    link.ClientPush(Conn, 0x4002, Text("late"));
    Check(link.ServerReceiveAll() && link.serverGame.back().body == Text("late"), "later frames are sealed and still arrive");
}

// The real engines over a session long enough to cross the automatic re-key boundary (65536 frames per
// direction), with traffic interleaved in both directions. Before re-keying existed this was a gap.
void LongSessionCrossesRekeyBoundary() {
    Link link(ClientGuard::Mode::Audit, ServerGuard::Mode::Audit);
    link.Connect();
    link.Pump();
    Check(link.server->Active(Conn) && link.client->Active(Conn), "active before the long run");
    const std::uint64_t frames = RatchetEveryMessages + 100;
    std::uint64_t upOk = 0, downOk = 0;
    for (std::uint64_t i = 0; i < frames; ++i) {
        const Bytes msg = Text("frame " + std::to_string(i));
        link.ClientPush(Conn, 0x3001, msg);
        link.ServerPush(0x1A44, msg);
        if (!link.ClientReceiveAll() || !link.ServerReceiveAll()) break;
        upOk += link.serverGame.back().body == msg;
        downOk += link.clientGame.back().body == msg;
        // The games' own logs are not needed here; keep memory flat.
        if (link.serverGame.size() > 1000) link.serverGame.clear();
        if (link.clientGame.size() > 1000) link.clientGame.clear();
    }
    Check(upOk == frames && downOk == frames, "every frame in both directions arrives intact across the re-key boundary");
    Check(link.server->Active(Conn) && link.client->Active(Conn), "and the connection is still active afterwards");
}

void ReconnectOnTheSameSlot() {
    Link link(ClientGuard::Mode::Audit, ServerGuard::Mode::Audit);
    link.Connect();
    link.Pump();
    Check(link.server->Active(Conn), "first connection active");
    link.Connect();   // the same slot is reused for a new connection: new stock key frame
    link.Pump();
    Check(link.server->Active(Conn) && link.client->Active(Conn), "a second connection on the same slot negotiates afresh");
    link.ServerPush(0x1A44, Text("after reconnect"));
    Check(link.ClientReceiveAll() && link.clientGame.back().body == Text("after reconnect"), "and traffic flows");
}
} // namespace

int main() {
    FullSessionBetweenTheRealEngines();
    ClientAndServerModeMatrix();
    TrafficBeforeReady();
    LongSessionCrossesRekeyBoundary();
    ReconnectOnTheSameSlot();
    std::printf("Channel interop tests passed (%d checks)\n", checks);
    return 0;
}
