// Offline tests for the client guard: the engine plus the detour layer, driven with fake natives
// and a simulated server built on the same NovaChannel core. No game or network involved.
#define CLIENT_GUARD_TESTING
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../src/ClientGuard/ClientGuard.cpp"
#include "../src/ClientGuard/Engine.cpp"
#include "../shared/NovaChannel.cpp"
#include "../shared/NovaChannelCng.cpp"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

using namespace NovaChannel;
using ClientGuard::Mode;

namespace {
int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct WireFrame { std::uint16_t opcode; Bytes body; };
std::deque<WireFrame> fromServer;          // what the fake socket delivers to the client
std::vector<WireFrame> toServer;           // what the client put on the wire
std::vector<WireFrame> delivered;          // what the game's receive loop was handed
int lastRecvResult = 0;
bool failSendLocked = false;
int sendLockedCalls = 0;
void* const ConnA = reinterpret_cast<void*>(0x1000);
void* const ConnB = reinterpret_cast<void*>(0x2000);

int __fastcall FakeReadFrame(void*, void*, ClientGuard::Frame* out) {
    if (fromServer.empty()) return -1;
    const WireFrame& next = fromServer.front();
    if (next.body.size() > out->length) { fromServer.pop_front(); return -3; } // the real oversize check
    out->opcode = next.opcode;
    out->length = static_cast<std::uint16_t>(next.body.size());
    if (!next.body.empty()) std::memcpy(out->body, next.body.data(), next.body.size());
    fromServer.pop_front();
    return 0;
}
int __fastcall FakePushSend(void*, void*, ClientGuard::Frame* in) {
    toServer.push_back({in->opcode, Bytes(in->body, in->body + in->length)});
    return 3;
}
int __fastcall FakeSendLocked(void* conn, void*, ClientGuard::Frame* in) {
    ++sendLockedCalls;
    if (failSendLocked) return 0;
    return ClientGuard::Test::Push(conn, in);
}
// Stand-in for the game's receive loop (OnRecvBytes): read frames until no more.
int __fastcall FakeOnRecvBytes(void* conn, void*, void* ring, int) {
    static std::uint8_t buffer[8192];
    for (;;) {
        ClientGuard::Frame out{0, 8192, buffer};
        const int r = ClientGuard::Test::Read(ring, &out);
        if (r != 0) { lastRecvResult = r; return r; }
        delivered.push_back({out.opcode, Bytes(buffer, buffer + out.length)});
        (void)conn;
    }
}

void Reset(Mode mode, std::vector<PublicKey> pinned) {
    fromServer.clear(); toServer.clear(); delivered.clear();
    lastRecvResult = 0; failSendLocked = false; sendLockedCalls = 0;
    ClientGuard::Test::Reset();
    ClientGuard::Test::Install(mode, std::move(pinned),
        reinterpret_cast<ClientGuard::ReadFrameFn>(FakeReadFrame),
        reinterpret_cast<ClientGuard::PushSendFn>(FakePushSend),
        reinterpret_cast<ClientGuard::SendLockedFn>(FakeSendLocked),
        reinterpret_cast<ClientGuard::OnRecvBytesFn>(FakeOnRecvBytes));
}
// The game "receives bytes" on the worker thread.
void Receive(void* conn) { ClientGuard::Test::Recv(conn, reinterpret_cast<void*>(0x5000), 1); }
// The game sends a frame through the send path (PushSend hook).
int GameSends(void* conn, std::uint16_t opcode, const Bytes& body) {
    ClientGuard::Frame in{opcode, static_cast<std::uint16_t>(body.size()), const_cast<std::uint8_t*>(body.data())};
    return ClientGuard::Test::Push(conn, &in);
}

Bytes StockKeyFrame(bool offer) {
    Bytes body(StockKeyFrameBytes, 0);
    const std::string key = "0106F079-AE41-4AB9-870D-31183E7528F6";
    std::memcpy(body.data(), key.data(), key.size());
    if (offer) {
        std::memcpy(body.data() + OfferOffset, OfferMagic.data(), OfferMagic.size());
        body[OfferOffset + 3] = Version;
    }
    return body;
}

struct Server {
    PublicKey pub{};
    Bytes priv;
    Session session = Session::MakeServer({});
    Server() {
        Check(Crypto::EcdsaGenerate(priv, pub), "server identity");
        session = Session::MakeServer(priv);
    }
    // Consumes the client's hello from the wire and returns the reply frame.
    WireFrame Accept(const WireFrame& hello) {
        Bytes reply;
        Check(hello.opcode == OpcodeClientHello, "the client sent a hello");
        Check(session.ServerAcceptHello(hello.body.data(), hello.body.size(), reply, nullptr) == Status::Ok, "server accepts");
        return {OpcodeServerHello, reply};
    }
    WireFrame Seal(std::uint16_t opcode, const Bytes& plain) {
        Bytes sealed;
        Check(session.Seal(opcode, plain.data(), plain.size(), sealed) == Status::Ok, "server seals");
        return {opcode, sealed};
    }
};

// Drives a full handshake and returns the connected server.
void Connect(Server& server, void* conn) {
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(true)});
    Receive(conn);
    Check(toServer.size() == 1 && toServer[0].opcode == OpcodeClientHello, "the offer makes the client send a plaintext hello");
    fromServer.push_back(server.Accept(toServer[0]));
    toServer.clear();
    delivered.clear();
}

void DisabledPassesEverything() {
    Server server;
    Reset(Mode::Disabled, {server.pub});
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(true)});
    fromServer.push_back({0x1234, Text("hello game")});
    Receive(ConnA);
    Check(delivered.size() == 2 && delivered[1].body == Text("hello game"), "disabled: frames are untouched");
    Check(toServer.empty() && sendLockedCalls == 0, "disabled: the client never sends a hello");
    Check(GameSends(ConnA, 0x1111, Text("x")) == 3 && toServer.size() == 1 && toServer[0].body == Text("x"), "disabled: sends are untouched");
}

void StockServerIsLeftAlone() {
    Server server;
    Reset(Mode::Audit, {server.pub});
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(false)});
    fromServer.push_back({0x1234, Text("stock traffic")});
    Receive(ConnA);
    Check(delivered.size() == 2 && lastRecvResult == -1, "audit with a stock server: the game sees everything");
    Check(toServer.empty(), "no hello is sent to a server that did not offer the channel");
    GameSends(ConnA, 0x2222, Text("plain"));
    Check(toServer.size() == 1 && toServer[0].body == Text("plain"), "and sends stay plaintext");

    Reset(Mode::Enforce, {server.pub});
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(false)});
    Receive(ConnA);
    Check(lastRecvResult == -3 && delivered.empty(), "enforce refuses a server that does not offer the channel");
    Check(GameSends(ConnA, 1, Text("x")) == 1 && toServer.empty(), "and sends nothing on that connection");
}

void FullHandshakeAndTraffic() {
    Server server;
    Reset(Mode::Audit, {server.pub});
    Connect(server, ConnA);

    // A frame the game sends while the hello is outstanding is still plaintext.
    // (Connect cleared the wire; resend the pre-ready case separately below.)
    fromServer.push_back(server.Seal(0x1A44, Text("sealed one")));
    fromServer.push_back(server.Seal(0x1A45, Bytes{}));
    fromServer.push_back(server.Seal(0x1A46, Bytes(8176, 0x42)));
    Receive(ConnA);
    Check(toServer.size() == 1 && toServer[0].opcode == OpcodeChannelReady && toServer[0].body.size() == TagBytes,
          "after the server hello the client sends one sealed ready frame");
    Check(delivered.size() == 3, "the server hello is consumed; the game sees only the three data frames");
    Check(delivered[0].opcode == 0x1A44 && delivered[0].body == Text("sealed one"), "frame 1 decrypted for the game");
    Check(delivered[1].opcode == 0x1A45 && delivered[1].body.empty(), "an empty frame decrypts");
    Check(delivered[2].body == Bytes(8176, 0x42), "a maximum frame decrypts");

    Bytes plain;
    Check(server.session.Open(OpcodeChannelReady, toServer[0].body.data(), toServer[0].body.size(), plain) == Status::Ok, "server opens the ready frame first");
    toServer.clear();
    for (int i = 0; i < 50; ++i) {
        const Bytes msg = Text("game message " + std::to_string(i));
        Check(GameSends(ConnA, 0x3001, msg) == 3, "send accepted");
        Check(toServer.back().body.size() == msg.size() + TagBytes, "outbound frames carry the tag");
        Check(server.session.Open(0x3001, toServer.back().body.data(), toServer.back().body.size(), plain) == Status::Ok && plain == msg,
              "server opens every client frame in order");
    }
    Check(GameSends(ConnA, 0x3002, Bytes(MaxPlainClientToServer, 7)) == 3, "a maximum outbound frame is sent");
    Check(toServer.back().body.size() == MaxWireBodyClientToServer, "and fits the 4095-byte wire limit");
    const std::size_t before = toServer.size();
    Check(GameSends(ConnA, 0x3002, Bytes(MaxPlainClientToServer + 1, 7)) == 1 && toServer.size() == before, "one byte over is refused, not truncated");
    Check(GameSends(ConnA, 0x3003, Text("still alive")) == 3, "a refused oversize frame does not end the session");

    // Traffic keeps flowing in both directions.
    fromServer.push_back(server.Seal(0x1A47, Text("later")));
    Receive(ConnA);
    Check(delivered.back().body == Text("later"), "inbound still works later in the session");
}

void PlaintextBeforeReady() {
    Server server;
    Reset(Mode::Audit, {server.pub});
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(true)});
    Receive(ConnA);
    Check(GameSends(ConnA, 0x4001, Text("early")) == 3 && toServer.back().body == Text("early"),
          "a frame sent while the hello is outstanding stays plaintext");
}

void AttacksAreContained() {
    {   // tampered sealed frame
        Server server;
        Reset(Mode::Audit, {server.pub});
        Connect(server, ConnA);
        WireFrame bad = server.Seal(0x1A44, Text("payload"));
        bad.body[2] ^= 1;
        fromServer.push_back(bad);
        Receive(ConnA);
        Check(lastRecvResult == -3 && delivered.empty(), "a tampered frame ends the receive loop with an error");
    }
    {   // wrong server identity
        Server real, rogue;
        Reset(Mode::Audit, {real.pub});
        fromServer.push_back({OpcodeStockKey, StockKeyFrame(true)});
        Receive(ConnA);
        fromServer.push_back(rogue.Accept(toServer[0]));
        Receive(ConnA);
        Check(lastRecvResult == -3, "a server hello signed by an unpinned key fails the connection");
        Check(GameSends(ConnA, 1, Text("x")) == 1, "and nothing more is sent");
    }
    {   // replayed and reordered sealed frames
        Server server;
        Reset(Mode::Audit, {server.pub});
        Connect(server, ConnA);
        const WireFrame one = server.Seal(0x1A44, Text("one")), two = server.Seal(0x1A44, Text("two"));
        fromServer.push_back(two);
        fromServer.push_back(one);
        Receive(ConnA);
        Check(lastRecvResult == -3 && delivered.empty(), "frames delivered out of order are rejected");
    }
    {   // a second server hello mid-session
        Server server;
        Reset(Mode::Audit, {server.pub});
        Connect(server, ConnA);
        fromServer.push_back({OpcodeServerHello, Bytes(ServerHelloBytes, 0)});
        Receive(ConnA);
        Check(lastRecvResult == -3, "an unexpected second server hello is fatal");
    }
    {   // an unsealed frame after the channel is up (injected plaintext)
        Server server;
        Reset(Mode::Audit, {server.pub});
        Connect(server, ConnA);
        fromServer.push_back({0x1A44, Text("injected plaintext")});
        Receive(ConnA);
        Check(lastRecvResult == -3 && delivered.empty(), "a plaintext frame injected into a sealed session is rejected");
    }
}

void ConsumeKeepsReading() {
    // The server hello is consumed and the next frames in the same batch must still be read:
    // the caller's maximum-length field has to be restored between reads.
    Server server;
    Reset(Mode::Audit, {server.pub});
    Connect(server, ConnA);
    for (int i = 0; i < 5; ++i) fromServer.push_back(server.Seal(0x1A44, Bytes(4000, static_cast<std::uint8_t>(i))));
    Receive(ConnA);
    Check(delivered.size() == 5 && lastRecvResult == -1, "all five large frames after the consumed hello are delivered");
}

void ReconnectAndIndependence() {
    Server a, b;
    Reset(Mode::Audit, {a.pub, b.pub});
    Connect(a, ConnA);
    fromServer.push_back(a.Seal(0x1A44, Text("on a")));
    Receive(ConnA);
    Check(delivered.back().body == Text("on a"), "connection A works");

    fromServer.push_back({OpcodeStockKey, StockKeyFrame(false)});
    Receive(ConnB);
    GameSends(ConnB, 5, Text("plain on b"));
    Check(toServer.back().body == Text("plain on b"), "connection B (stock) is independent of A");
    Bytes plain;
    GameSends(ConnA, 5, Text("sealed on a"));
    Check(toServer.back().body.size() == Text("sealed on a").size() + TagBytes, "while A is sealed");
    (void)plain;

    // A reconnects (same connection slot): new key frame, new server, fresh handshake.
    toServer.clear();
    Connect(b, ConnA);
    fromServer.push_back(b.Seal(0x1A44, Text("on b server")));
    Receive(ConnA);
    Check(delivered.back().body == Text("on b server"), "a reconnect on the same slot starts a fresh channel");
}

void HelloSendFailureFallsBack() {
    Server server;
    Reset(Mode::Audit, {server.pub});
    failSendLocked = true;
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(true)});
    Receive(ConnA);
    Check(lastRecvResult == -1, "a hello that could not be queued does not fail the connection");
    failSendLocked = false;
    GameSends(ConnA, 9, Text("plain"));
    Check(toServer.back().body == Text("plain"), "the client keeps sending plaintext");

    // But failing to send the ready frame after a good server hello is fatal.
    Reset(Mode::Audit, {server.pub});
    fromServer.push_back({OpcodeStockKey, StockKeyFrame(true)});
    Receive(ConnA);
    fromServer.push_back(server.Accept(toServer[0]));
    failSendLocked = true;
    Receive(ConnA);
    Check(lastRecvResult == -3, "a lost ready frame is fatal because the server will already be sealing");
}

void FingerprintAndAttach() {
    static unsigned char* image = nullptr;
    image = static_cast<unsigned char*>(std::calloc(0xB10000, 1));
    Check(image != nullptr, "allocate a fake image");
    auto put = [&](std::uintptr_t address, std::initializer_list<unsigned char> bytes) {
        std::memcpy(image + (address - 0x400000), bytes.begin(), bytes.size());
    };
    put(0xaa9b20, {0x83, 0xec, 0x08, 0x8b, 0xc1, 0x53, 0x8b, 0x58, 0x18, 0x83, 0xfb, 0x04});
    put(0xaa8b50, {0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x30, 0x02, 0x74, 0x06, 0x33, 0xc0, 0x5e});
    put(0xaa8bc0, {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x08, 0x57, 0x83, 0xc0, 0x30, 0x50, 0xff});
    put(0xaa8d70, {0x83, 0xec, 0x0c, 0x53, 0x55, 0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x38, 0x57});
    Check(ClientGuard::MatchesReference(image), "the reference prologues are recognized");
    image[0xaa8bc0 - 0x400000 + 5] ^= 1;
    Check(!ClientGuard::MatchesReference(image), "one changed byte at a hook site is rejected");
    std::free(image);
    Check(!ClientGuard::MatchesReference(reinterpret_cast<const unsigned char*>(0x10)), "an unreadable image is rejected, not crashed on");

    Check(ClientGuard::ParseMode("audit") == Mode::Audit && ClientGuard::ParseMode("enforce") == Mode::Enforce, "modes parse");
    Check(ClientGuard::ParseMode("ENFORCE") == Mode::Disabled && ClientGuard::ParseMode("") == Mode::Disabled &&
          ClientGuard::ParseMode(nullptr) == Mode::Disabled, "anything else is disabled");
    // No novaclient.ini beside the test executable and no pinned key: Attach must do nothing.
    Check(ClientGuard::Attach() == NO_ERROR, "Attach with no config succeeds without hooking");
    Check(std::strstr(ClientGuard::StatusText(), "disabled") != nullptr, "and reports that it is disabled");
}
} // namespace

int main() {
    DisabledPassesEverything();
    StockServerIsLeftAlone();
    FullHandshakeAndTraffic();
    PlaintextBeforeReady();
    AttacksAreContained();
    ConsumeKeepsReading();
    ReconnectAndIndependence();
    HelloSendFailureFallsBack();
    FingerprintAndAttach();
    std::printf("ClientGuard tests passed (%d checks)\n", checks);
    return 0;
}
