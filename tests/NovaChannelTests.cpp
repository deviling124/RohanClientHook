// Offline tests for the NovaChannel core (shared/NovaChannel*.cpp). No game or network involved.
// Known-answer vectors were computed with Python hashlib/hmac and .NET AesGcm as independent oracles.
#include "../shared/NovaChannel.cpp"
#include "../shared/NovaChannelCng.cpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace NovaChannel;

namespace {
int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
Bytes Hex(const char* s) {
    Bytes out;
    for (std::size_t i = 0; s[i] && s[i + 1]; i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(s + i, 2), nullptr, 16)));
    return out;
}
std::string ToHex(const std::uint8_t* p, std::size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
std::string ToHex(const Bytes& b) { return ToHex(b.data(), b.size()); }
Bytes Text(const char* s) { return Bytes(s, s + std::strlen(s)); }

struct Pair {
    Session client, server;
    PublicKey identityPub{};
    Bytes identityPriv;
    Pair(Session c, Session s) : client(std::move(c)), server(std::move(s)) {}
};

// Builds an established pair. `pinned` overrides the key the client trusts when non-null.
Pair Handshake(const PublicKey* pinned = nullptr, Bytes attestation = {}, Bytes* seenAttestation = nullptr) {
    PublicKey pub{};
    Bytes priv;
    Check(Crypto::EcdsaGenerate(priv, pub), "identity key generation");
    Pair p(Session::MakeClient({pinned ? *pinned : pub}), Session::MakeServer(priv));
    p.identityPub = pub;
    p.identityPriv = priv;
    Bytes hello, reply;
    Check(p.client.ClientBuildHello(attestation, hello) == Status::Ok, "client hello");
    Check(p.server.ServerAcceptHello(hello.data(), hello.size(), reply, seenAttestation) == Status::Ok, "server accepts hello");
    return p;
}

void KnownAnswers() {
    std::uint8_t out[32];
    Check(Crypto::Sha256(reinterpret_cast<const std::uint8_t*>("abc"), 3, out), "sha256 runs");
    Check(ToHex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");

    const Bytes key(20, 0x0b);
    Check(Crypto::HmacSha256(key.data(), key.size(), reinterpret_cast<const std::uint8_t*>("Hi There"), 8, out), "hmac runs");
    Check(ToHex(out, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "HMAC-SHA256 RFC 4231 case 1");

    std::uint8_t okm[42];
    const Bytes ikm(22, 0x0b), salt = Hex("000102030405060708090a0b0c"), info = Hex("f0f1f2f3f4f5f6f7f8f9");
    Check(Crypto::Hkdf(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), okm, sizeof okm), "hkdf runs");
    Check(ToHex(okm, 42) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", "HKDF RFC 5869 case 1");
    Check(Crypto::Hkdf(nullptr, 0, ikm.data(), ikm.size(), nullptr, 0, okm, sizeof okm), "hkdf empty salt and info");
    Check(ToHex(okm, 42) == "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", "HKDF RFC 5869 case 3");
    Check(!Crypto::Hkdf(nullptr, 0, ikm.data(), ikm.size(), nullptr, 0, okm, 255 * 32 + 1), "hkdf rejects oversize output");

    const Bytes k = Hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const Bytes iv = Hex("000102030405060708090a0b"), aad = Hex("0102030405060708090a0b0c");
    const Bytes pt = Text("nova channel known answer");
    Bytes sealed(pt.size() + 16);
    Check(Crypto::AesGcmSeal(k.data(), iv.data(), aad.data(), aad.size(), pt.data(), pt.size(), sealed.data()), "gcm seal");
    Check(ToHex(sealed) == "296da07ae586aa7ae32ff2e791821602f4b8a7559e0828194a" "ce75c9f30e1d09abe33f04888c99d9ed", "AES-256-GCM known answer");
    Bytes back(pt.size());
    Check(Crypto::AesGcmOpen(k.data(), iv.data(), aad.data(), aad.size(), sealed.data(), pt.size(), back.data()) == Crypto::OpenResult::Ok && back == pt, "gcm open");
    Bytes empty(16);
    Check(Crypto::AesGcmSeal(k.data(), iv.data(), aad.data(), aad.size(), nullptr, 0, empty.data()), "gcm empty seal");
    Check(ToHex(empty) == "7b3ced63740a45a6c2e387792e34f3c8", "AES-256-GCM empty plaintext tag");
    Bytes tampered = sealed;
    tampered[3] ^= 1;
    Check(Crypto::AesGcmOpen(k.data(), iv.data(), aad.data(), aad.size(), tampered.data(), pt.size(), back.data()) == Crypto::OpenResult::TagMismatch, "gcm tamper detected");
}

void EcdhAndSignatures() {
    Crypto::EcdhKey a, b;
    Check(a.Generate() && b.Generate(), "ecdh keys");
    std::uint8_t s1[32], s2[32];
    Check(a.Agree(b.Public(), s1) && b.Agree(a.Public(), s2), "ecdh agree");
    Check(std::memcmp(s1, s2, 32) == 0, "both sides get the same secret");
    PublicKey bad{};
    bad.fill(1);
    Check(!a.Agree(bad, s1), "a point off the curve is rejected");

    Bytes priv;
    PublicKey pub{};
    Check(Crypto::EcdsaGenerate(priv, pub), "ecdsa keys");
    std::uint8_t hash[32], sig[64];
    Crypto::Sha256(reinterpret_cast<const std::uint8_t*>("handshake"), 9, hash);
    Check(Crypto::EcdsaSign(priv, hash, sig), "sign");
    Check(Crypto::EcdsaVerify(pub, hash, sig), "verify");
    sig[10] ^= 1;
    Check(!Crypto::EcdsaVerify(pub, hash, sig), "a changed signature fails");
}

void HandshakeAndTraffic() {
    Bytes seen;
    Pair p = Handshake(nullptr, Text("exe-hash-placeholder"), &seen);
    Check(seen == Text("exe-hash-placeholder"), "server receives the attestation");
    Check(p.server.Established() && !p.client.Established(), "server is established, client waits for the reply");

    // The client needs the server reply; rebuild with the real flow.
    PublicKey pub{};
    Bytes priv;
    Check(Crypto::EcdsaGenerate(priv, pub), "identity");
    Session c = Session::MakeClient({pub}), s = Session::MakeServer(priv);
    Bytes hello, reply;
    Check(c.ClientBuildHello({}, hello) == Status::Ok, "hello");
    Check(hello.size() == ClientHelloFixedBytes, "empty attestation size");
    Check(s.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr) == Status::Ok, "accept");
    Check(reply.size() == ServerHelloBytes, "server hello size");
    Check(c.ClientAcceptServerHello(reply.data(), reply.size()) == Status::Ok, "client accepts the reply");
    Check(c.Established() && s.Established(), "both established");

    for (int i = 0; i < 200; ++i) {
        const Bytes msg = Text(("client to server message " + std::to_string(i)).c_str());
        Bytes wire, plain;
        Check(c.Seal(0x1234, msg.data(), msg.size(), wire) == Status::Ok, "client seal");
        Check(wire.size() == msg.size() + TagBytes, "ciphertext is plaintext plus tag");
        Check(s.Open(0x1234, wire.data(), wire.size(), plain) == Status::Ok && plain == msg, "server open");
        Check(s.Seal(0x4321, msg.data(), msg.size(), wire) == Status::Ok, "server seal");
        Check(c.Open(0x4321, wire.data(), wire.size(), plain) == Status::Ok && plain == msg, "client open");
    }
    Check(c.SentCount() == 200 && s.ReceivedCount() == 200 && c.ReceivedCount() == 200, "counters advance together");

    Bytes wire, plain;
    Check(c.Seal(0x0001, nullptr, 0, wire) == Status::Ok && wire.size() == TagBytes, "empty body seals to a bare tag");
    Check(s.Open(0x0001, wire.data(), wire.size(), plain) == Status::Ok && plain.empty(), "empty body opens");
}

void SizeLimits() {
    Pair p = Handshake();
    Session& s = p.server;
    // Rebuild a full pair for traffic.
    PublicKey pub{};
    Bytes priv;
    Crypto::EcdsaGenerate(priv, pub);
    Session c = Session::MakeClient({pub}), srv = Session::MakeServer(priv);
    Bytes hello, reply, wire, plain;
    c.ClientBuildHello({}, hello);
    srv.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr);
    c.ClientAcceptServerHello(reply.data(), reply.size());
    (void)s;

    const Bytes biggest(MaxPlainClientToServer, 0x5a);
    Check(c.Seal(1, biggest.data(), biggest.size(), wire) == Status::Ok && wire.size() == MaxWireBodyClientToServer, "client maximum body seals to 4095");
    Check(srv.Open(1, wire.data(), wire.size(), plain) == Status::Ok && plain == biggest, "and opens");
    const Bytes over(MaxPlainClientToServer + 1, 0);
    Check(c.Seal(1, over.data(), over.size(), wire) == Status::TooLarge, "client body over 4079 is refused");
    Check(!c.Failed(), "a refused oversize send does not kill the session");
    const Bytes smax(MaxPlainServerToClient, 1);
    Check(srv.Seal(2, smax.data(), smax.size(), wire) == Status::Ok && wire.size() == MaxWireBodyServerToClient, "server maximum body seals to 8192");
    Check(c.Open(2, wire.data(), wire.size(), plain) == Status::Ok && plain == smax, "and the client opens it");
}

void TamperReplayReorder() {
    auto fresh = [](Session& c, Session& s) {
        PublicKey pub{};
        Bytes priv;
        Crypto::EcdsaGenerate(priv, pub);
        c = Session::MakeClient({pub});
        s = Session::MakeServer(priv);
        Bytes hello, reply;
        c.ClientBuildHello({}, hello);
        s.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr);
        c.ClientAcceptServerHello(reply.data(), reply.size());
    };
    Session c = Session::MakeClient({}), s = Session::MakeServer({});
    const Bytes msg = Text("pay 100 gold");
    Bytes w1, w2, plain;

    fresh(c, s);
    c.Seal(7, msg.data(), msg.size(), w1);
    Bytes flipped = w1;
    flipped[0] ^= 0x80;
    Check(s.Open(7, flipped.data(), flipped.size(), plain) == Status::TagMismatch, "a flipped ciphertext bit fails");
    Check(s.Failed() && s.LastError() == Status::TagMismatch, "the session is failed after a tag error");
    Check(s.Open(7, w1.data(), w1.size(), plain) == Status::BadState, "a failed session refuses even good input");

    fresh(c, s);
    c.Seal(7, msg.data(), msg.size(), w1);
    Check(s.Open(8, w1.data(), w1.size(), plain) == Status::TagMismatch, "a changed opcode fails (header is authenticated)");

    fresh(c, s);
    c.Seal(7, msg.data(), msg.size(), w1);
    Check(s.Open(7, w1.data(), w1.size(), plain) == Status::Ok, "first delivery is accepted");
    Check(s.Open(7, w1.data(), w1.size(), plain) == Status::TagMismatch, "replaying the same frame fails");

    fresh(c, s);
    c.Seal(7, msg.data(), msg.size(), w1);
    c.Seal(7, msg.data(), msg.size(), w2);
    Check(s.Open(7, w2.data(), w2.size(), plain) == Status::TagMismatch, "a frame delivered out of order fails");

    fresh(c, s);
    c.Seal(7, msg.data(), msg.size(), w1);
    Check(s.Open(7, w1.data(), w1.size() - 1, plain) == Status::TagMismatch, "a truncated frame fails");
    fresh(c, s);
    Check(s.Open(7, w1.data(), 5, plain) == Status::BadMessage, "a frame shorter than a tag is malformed");
    fresh(c, s);
    const Bytes huge(MaxWireBodyClientToServer + 1, 0);
    Check(s.Open(7, huge.data(), huge.size(), plain) == Status::TooLarge, "an oversize frame is refused");

    fresh(c, s);
    c.Seal(7, msg.data(), msg.size(), w1);
    Check(c.Open(7, w1.data(), w1.size(), plain) == Status::TagMismatch, "a frame reflected back to its sender fails");
}

void HandshakeAttacks() {
    PublicKey pub{}, other{};
    Bytes priv, otherPriv;
    Check(Crypto::EcdsaGenerate(priv, pub) && Crypto::EcdsaGenerate(otherPriv, other), "two identities");
    Bytes hello, reply;

    { // client pins the wrong key
        Session c = Session::MakeClient({other}), s = Session::MakeServer(priv);
        c.ClientBuildHello({}, hello);
        s.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr);
        Check(c.ClientAcceptServerHello(reply.data(), reply.size()) == Status::BadSignature, "an unpinned server is rejected");
        Check(c.Failed() && !c.Established(), "and the client does not establish");
    }
    { // two pinned keys: the second one matches
        Session c = Session::MakeClient({other, pub}), s = Session::MakeServer(priv);
        c.ClientBuildHello({}, hello);
        s.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr);
        Check(c.ClientAcceptServerHello(reply.data(), reply.size()) == Status::Ok, "either pinned key is accepted");
    }
    for (std::size_t at : {std::size_t{0}, std::size_t{5}, std::size_t{70}, std::size_t{110}, std::size_t{130}, std::size_t{163}}) {
        Session c = Session::MakeClient({pub}), s = Session::MakeServer(priv);
        c.ClientBuildHello({}, hello);
        s.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr);
        Bytes bad = reply;
        bad[at] ^= 0x01;
        const Status st = c.ClientAcceptServerHello(bad.data(), bad.size());
        Check(st != Status::Ok, ("tampered server hello byte " + std::to_string(at) + " is rejected").c_str());
        Check(!c.Established(), "client stays out of the established state");
    }
    { // a man in the middle swaps the client's hello: the server signs a different transcript
        Session c = Session::MakeClient({pub}), s = Session::MakeServer(priv), mitm = Session::MakeClient({pub});
        Bytes helloMitm;
        c.ClientBuildHello({}, hello);
        mitm.ClientBuildHello({}, helloMitm);
        s.ServerAcceptHello(helloMitm.data(), helloMitm.size(), reply, nullptr);
        Check(c.ClientAcceptServerHello(reply.data(), reply.size()) == Status::BadSignature, "a reply to a swapped hello does not verify");
    }
    { // malformed and hostile hellos
        Session s = Session::MakeServer(priv);
        Bytes r;
        Check(s.ServerAcceptHello(nullptr, 0, r, nullptr) == Status::BadMessage, "null hello");
        Session s2 = Session::MakeServer(priv);
        Bytes shortHello(ClientHelloFixedBytes - 1, 0);
        Check(s2.ServerAcceptHello(shortHello.data(), shortHello.size(), r, nullptr) == Status::BadMessage, "short hello");
        Session c = Session::MakeClient({pub});
        c.ClientBuildHello({}, hello);
        Session s3 = Session::MakeServer(priv);
        Bytes wrongVersion = hello;
        wrongVersion[0] = 9;
        Check(s3.ServerAcceptHello(wrongVersion.data(), wrongVersion.size(), r, nullptr) == Status::BadVersion, "wrong version");
        Session s4 = Session::MakeServer(priv);
        Bytes lying = hello;
        lying[2] = 7; // claims 7 attestation bytes but carries none
        Check(s4.ServerAcceptHello(lying.data(), lying.size(), r, nullptr) == Status::BadMessage, "attestation length must match exactly");
        Session s5 = Session::MakeServer(priv);
        Bytes badPoint = hello;
        std::fill(badPoint.begin() + 4, badPoint.begin() + 68, static_cast<std::uint8_t>(7));
        Check(s5.ServerAcceptHello(badPoint.data(), badPoint.size(), r, nullptr) == Status::BadMessage, "an invalid ECDH point is rejected");
        Session s6 = Session::MakeServer(priv);
        Bytes reserved = hello;
        reserved[1] = 1;
        Check(s6.ServerAcceptHello(reserved.data(), reserved.size(), r, nullptr) == Status::BadMessage, "reserved byte must be zero");
        Session c2 = Session::MakeClient({pub});
        Bytes tooMuch(MaxAttestationBytes + 1, 0);
        Check(c2.ClientBuildHello(tooMuch, hello) == Status::TooLarge, "attestation is bounded");
        Session c3 = Session::MakeClient({pub});
        Bytes maxAtt(MaxAttestationBytes, 3);
        Check(c3.ClientBuildHello(maxAtt, hello) == Status::Ok && hello.size() == ClientHelloFixedBytes + MaxAttestationBytes, "a maximum attestation is accepted");
    }
    { // calls in the wrong state
        Session c = Session::MakeClient({pub}), s = Session::MakeServer(priv);
        Bytes out;
        Check(c.Seal(1, nullptr, 0, out) == Status::BadState, "no sealing before the handshake");
        Check(c.ClientAcceptServerHello(nullptr, 0) == Status::BadState, "no server hello before a client hello");
        c.ClientBuildHello({}, hello);
        Check(c.ClientBuildHello({}, hello) == Status::BadState, "no second client hello");
        Check(s.ClientBuildHello({}, hello) == Status::BadState, "a server cannot build a client hello");
        Check(c.RatchetSend() == Status::BadState && c.RatchetReceive() == Status::BadState, "no ratchet before the handshake completes");
    }
    { // sessions do not share keys
        Session c1 = Session::MakeClient({pub}), s1 = Session::MakeServer(priv);
        Session c2 = Session::MakeClient({pub}), s2 = Session::MakeServer(priv);
        Bytes h1, r1, h2, r2, w, plain;
        c1.ClientBuildHello({}, h1); s1.ServerAcceptHello(h1.data(), h1.size(), r1, nullptr); c1.ClientAcceptServerHello(r1.data(), r1.size());
        c2.ClientBuildHello({}, h2); s2.ServerAcceptHello(h2.data(), h2.size(), r2, nullptr); c2.ClientAcceptServerHello(r2.data(), r2.size());
        const Bytes msg = Text("same message");
        c1.Seal(3, msg.data(), msg.size(), w);
        Check(s2.Open(3, w.data(), w.size(), plain) == Status::TagMismatch, "a frame from another session cannot be opened");
    }
}

void Ratcheting() {
    PublicKey pub{};
    Bytes priv;
    Crypto::EcdsaGenerate(priv, pub);
    auto build = [&](Session& c, Session& s) {
        c = Session::MakeClient({pub});
        s = Session::MakeServer(priv);
        Bytes hello, reply;
        c.ClientBuildHello({}, hello);
        s.ServerAcceptHello(hello.data(), hello.size(), reply, nullptr);
        c.ClientAcceptServerHello(reply.data(), reply.size());
    };
    const Bytes msg = Text("ratchet test message");
    Bytes w, plain;

    {   // Explicit, per direction, and safe with frames in flight.
        Session c = Session::MakeClient({}), s = Session::MakeServer({});
        build(c, s);
        Bytes a, b, cc, down;
        c.Seal(1, msg.data(), msg.size(), a);
        c.Seal(1, msg.data(), msg.size(), b);             // two frames sealed under epoch 0, not yet delivered
        Check(c.RatchetSend() == Status::Ok && c.SendEpoch() == 1 && c.SentCount() == 0, "the sender re-keys its own direction");
        c.Seal(1, msg.data(), msg.size(), cc);            // a frame under epoch 1
        Check(s.Seal(2, msg.data(), msg.size(), down) == Status::Ok &&
              c.Open(2, down.data(), down.size(), plain) == Status::Ok, "the opposite direction keeps working throughout");
        Check(s.Open(1, a.data(), a.size(), plain) == Status::Ok && s.Open(1, b.data(), b.size(), plain) == Status::Ok,
              "frames already sealed under the old key still open on a receiver that has not re-keyed yet");
        Check(s.RatchetReceive() == Status::Ok && s.ReceiveEpoch() == 1, "the receiver re-keys right after the last old frame");
        Check(s.Open(1, cc.data(), cc.size(), plain) == Status::Ok && plain == msg, "and the new epoch's first frame opens");
        Check(c.ReceiveEpoch() == 0 && s.SendEpoch() == 0, "the other direction's epoch is untouched");
    }
    {   // An old-epoch frame is rejected once the receiver moved on.
        Session c = Session::MakeClient({}), s = Session::MakeServer({});
        build(c, s);
        c.Seal(1, msg.data(), msg.size(), w);
        s.RatchetReceive();
        Check(s.Open(1, w.data(), w.size(), plain) == Status::TagMismatch, "an old-epoch frame fails after the receiver re-keys");
    }
    {   // One-sided re-keying is detected.
        Session c = Session::MakeClient({}), s = Session::MakeServer({});
        build(c, s);
        c.RatchetSend();
        c.Seal(1, msg.data(), msg.size(), w);
        Check(s.Open(1, w.data(), w.size(), plain) == Status::TagMismatch, "a sender that re-keys alone is detected");
    }
    {   // Automatic re-keying at an exact frame count, both directions, in step.
        Session c = Session::MakeClient({}), s = Session::MakeServer({});
        build(c, s);
        c.SetRatchetInterval(8);
        s.SetRatchetInterval(8);
        for (int i = 0; i < 100; ++i) {
            Check(c.Seal(1, msg.data(), msg.size(), w) == Status::Ok &&
                  s.Open(1, w.data(), w.size(), plain) == Status::Ok && plain == msg, "automatic ratchet keeps c2s traffic flowing");
            Check(s.Seal(2, msg.data(), msg.size(), w) == Status::Ok &&
                  c.Open(2, w.data(), w.size(), plain) == Status::Ok && plain == msg, "automatic ratchet keeps s2c traffic flowing");
        }
        Check(c.SendEpoch() == 12 && s.ReceiveEpoch() == 12 && s.SendEpoch() == 12 && c.ReceiveEpoch() == 12,
              "100 frames at an interval of 8 is 12 re-keys per direction");
        Check(c.SentCount() == 4 && s.ReceivedCount() == 4, "and the counter restarted at each boundary");
        // Different directions and epochs never share a key stream: the same plaintext and opcode seal differently.
        Bytes first, second;
        Session c2 = Session::MakeClient({}), s2 = Session::MakeServer({});
        build(c2, s2);
        c2.Seal(1, msg.data(), msg.size(), first);
        c2.RatchetSend();
        c2.Seal(1, msg.data(), msg.size(), second);
        Check(first != second, "the same message seals differently after a re-key");
    }
    {   // Both ends must agree on the interval; a mismatch is caught at the first boundary.
        Session c = Session::MakeClient({}), s = Session::MakeServer({});
        build(c, s);
        c.SetRatchetInterval(8);
        s.SetRatchetInterval(16);
        bool ok = true;
        for (int i = 0; i < 8; ++i) { c.Seal(1, msg.data(), msg.size(), w); ok &= s.Open(1, w.data(), w.size(), plain) == Status::Ok; }
        Check(ok, "frames before the boundary still open");
        c.Seal(1, msg.data(), msg.size(), w);
        Check(s.Open(1, w.data(), w.size(), plain) == Status::TagMismatch, "an interval mismatch fails at the first boundary");
    }
    {   // The real default interval: crossing 65536 frames in each direction.
        Session c = Session::MakeClient({}), s = Session::MakeServer({});
        build(c, s);
        const Bytes tiny = Text("x");
        std::uint64_t okUp = 0, okDown = 0;
        for (std::uint64_t i = 0; i < RatchetEveryMessages + 40; ++i) {
            if (c.Seal(1, tiny.data(), tiny.size(), w) == Status::Ok && s.Open(1, w.data(), w.size(), plain) == Status::Ok) ++okUp;
            if (s.Seal(2, tiny.data(), tiny.size(), w) == Status::Ok && c.Open(2, w.data(), w.size(), plain) == Status::Ok) ++okDown;
        }
        Check(okUp == RatchetEveryMessages + 40 && okDown == okUp, "every frame across the default boundary arrives, both directions");
        Check(c.SendEpoch() == 1 && s.ReceiveEpoch() == 1 && s.SendEpoch() == 1 && c.ReceiveEpoch() == 1, "one automatic re-key per direction");
        Check(c.SentCount() == 40, "the counter restarted at the boundary");
    }
}

} // namespace

int main() {
    KnownAnswers();
    EcdhAndSignatures();
    HandshakeAndTraffic();
    SizeLimits();
    TamperReplayReorder();
    HandshakeAttacks();
    Ratcheting();
    std::printf("NovaChannel tests passed (%d checks)\n", checks);
    return 0;
}
