// NovaChannel protocol and state machine. Platform independent; crypto is in NovaChannelCng.cpp.
#include "NovaChannel.h"
#include <algorithm>
#include <cstring>

namespace NovaChannel {

const char* StatusName(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::BadState: return "bad state";
    case Status::BadMessage: return "bad message";
    case Status::BadVersion: return "unsupported version";
    case Status::BadSignature: return "bad server signature";
    case Status::CryptoFailure: return "crypto failure";
    case Status::CounterMismatch: return "counter exhausted";
    case Status::TagMismatch: return "authentication failed";
    case Status::TooLarge: return "frame too large";
    }
    return "unknown";
}

namespace Crypto {
bool Hkdf(const std::uint8_t* salt, std::size_t saltLen, const std::uint8_t* ikm, std::size_t ikmLen,
          const std::uint8_t* info, std::size_t infoLen, std::uint8_t* out, std::size_t n) {
    if (n > 255 * HashBytes) return false;
    std::uint8_t zeroSalt[HashBytes] = {};
    if (saltLen == 0) { salt = zeroSalt; saltLen = HashBytes; } // RFC 5869: empty salt means a zero block
    std::uint8_t prk[HashBytes];
    if (!HmacSha256(salt, saltLen, ikm, ikmLen, prk)) return false;
    Bytes block;
    std::uint8_t t[HashBytes] = {};
    std::size_t have = 0, tLen = 0;
    bool ok = true;
    for (std::uint8_t counter = 1; have < n && ok; ++counter) {
        block.assign(t, t + tLen);
        block.insert(block.end(), info, info + infoLen);
        block.push_back(counter);
        ok = HmacSha256(prk, sizeof prk, block.data(), block.size(), t);
        tLen = HashBytes;
        const std::size_t take = std::min<std::size_t>(HashBytes, n - have);
        std::memcpy(out + have, t, take);
        have += take;
    }
    Wipe(prk, sizeof prk);
    Wipe(t, sizeof t);
    return ok;
}
} // namespace Crypto

namespace {
constexpr char TranscriptLabel[] = "nova-channel-v2 transcript";
constexpr char KeyLabel[] = "nova-channel-v2 keys";
constexpr char RatchetLabel[] = "nova-channel-v2 ratchet";
constexpr std::size_t KeyBlockBytes = 2 * KeyBytes + 2 * 4; // c2s key, s2c key, c2s prefix, s2c prefix

void PutU16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
void PutU32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }
void PutU64(std::uint8_t* p, std::uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }
std::uint16_t GetU16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
} // namespace

struct Session::Impl {
    Role role = Role::Client;
    enum class State { Idle, HelloSent, Established, Failed } state = State::Idle;
    Status error = Status::Ok;
    // client
    std::vector<PublicKey> pinned;
    Bytes helloBytes;
    std::unique_ptr<Crypto::EcdhKey> eph;
    // server
    Bytes identity;
    // keys
    std::array<std::uint8_t, KeyBytes> sendKey{}, recvKey{};
    std::array<std::uint8_t, 4> sendPrefix{}, recvPrefix{};
    std::uint64_t sendCount = 0, recvCount = 0;
    std::uint32_t sendEpoch = 0, recvEpoch = 0;
    std::uint64_t ratchetEvery = RatchetEveryMessages;

    // Re-keys ONE direction: key' || prefix' = HKDF(ikm = key || prefix, info = label || direction || epoch').
    // `toServer` names the direction absolutely (client to server or server to client), so both ends
    // derive the same value for the same direction. The old key is erased and the counter restarts.
    bool RatchetDirection(std::array<std::uint8_t, KeyBytes>& key, std::array<std::uint8_t, 4>& prefix,
                          std::uint64_t& counter, std::uint32_t& epoch, bool toServer) {
        std::uint8_t ikm[KeyBytes + 4], out[KeyBytes + 4];
        std::memcpy(ikm, key.data(), KeyBytes);
        std::memcpy(ikm + KeyBytes, prefix.data(), 4);
        std::uint8_t info[sizeof RatchetLabel - 1 + 1 + 4];
        std::memcpy(info, RatchetLabel, sizeof RatchetLabel - 1);
        info[sizeof RatchetLabel - 1] = toServer ? 0 : 1;
        PutU32(info + sizeof RatchetLabel, epoch + 1);
        const bool ok = Crypto::Hkdf(nullptr, 0, ikm, sizeof ikm, info, sizeof info, out, sizeof out);
        Crypto::Wipe(ikm, sizeof ikm);
        if (ok) {
            std::memcpy(key.data(), out, KeyBytes);
            std::memcpy(prefix.data(), out + KeyBytes, 4);
            counter = 0;
            ++epoch;
        }
        Crypto::Wipe(out, sizeof out);
        return ok;
    }
    bool SendsToServer() const { return role == Role::Client; }

    ~Impl() {
        Wipe();
        Crypto::Wipe(identity.data(), identity.size()); // the server's private key copy
    }
    void Wipe() {
        Crypto::Wipe(sendKey.data(), sendKey.size());
        Crypto::Wipe(recvKey.data(), recvKey.size());
        Crypto::Wipe(sendPrefix.data(), sendPrefix.size());
        Crypto::Wipe(recvPrefix.data(), recvPrefix.size());
    }
    Status Fail(Status s) {
        if (state != State::Failed) { state = State::Failed; error = s; }
        Wipe();
        return s;
    }
    void LoadKeyBlock(const std::uint8_t block[KeyBlockBytes]) {
        const bool client = role == Role::Client;
        const std::uint8_t* c2s = block;
        const std::uint8_t* s2c = block + KeyBytes;
        const std::uint8_t* c2sPrefix = block + 2 * KeyBytes;
        const std::uint8_t* s2cPrefix = c2sPrefix + 4;
        std::memcpy(sendKey.data(), client ? c2s : s2c, KeyBytes);
        std::memcpy(recvKey.data(), client ? s2c : c2s, KeyBytes);
        std::memcpy(sendPrefix.data(), client ? c2sPrefix : s2cPrefix, 4);
        std::memcpy(recvPrefix.data(), client ? s2cPrefix : c2sPrefix, 4);
        sendCount = recvCount = 0;
    }
    // transcript = SHA-256(label || client hello (all bytes) || server hello without the signature)
    bool Transcript(const Bytes& clientHello, const std::uint8_t* serverHello, std::uint8_t out[HashBytes]) const {
        Bytes all(TranscriptLabel, TranscriptLabel + sizeof TranscriptLabel);
        all.insert(all.end(), clientHello.begin(), clientHello.end());
        all.insert(all.end(), serverHello, serverHello + ServerHelloSignedBytes);
        return Crypto::Sha256(all.data(), all.size(), out);
    }
    bool Derive(const std::uint8_t secret[32], const std::uint8_t transcript[HashBytes]) {
        std::uint8_t block[KeyBlockBytes];
        const bool ok = Crypto::Hkdf(transcript, HashBytes, secret, 32,
                                     reinterpret_cast<const std::uint8_t*>(KeyLabel), sizeof KeyLabel - 1,
                                     block, sizeof block);
        if (ok) LoadKeyBlock(block);
        Crypto::Wipe(block, sizeof block);
        return ok;
    }
    static void MakeIv(const std::array<std::uint8_t, 4>& prefix, std::uint64_t counter, std::uint8_t iv[IvBytes]) {
        std::memcpy(iv, prefix.data(), 4);
        PutU64(iv + 4, counter);
    }
    static void MakeAad(std::uint16_t opcode, std::size_t cipherLen, std::uint64_t counter, std::uint8_t aad[12]) {
        PutU16(aad, opcode);
        PutU16(aad + 2, static_cast<std::uint16_t>(cipherLen));
        PutU64(aad + 4, counter);
    }
};

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

Session Session::MakeClient(std::vector<PublicKey> pinnedServerKeys) {
    auto impl = std::make_unique<Impl>();
    impl->role = Role::Client;
    impl->pinned = std::move(pinnedServerKeys);
    return Session(std::move(impl));
}
Session Session::MakeServer(Bytes identityPrivateBlob) {
    auto impl = std::make_unique<Impl>();
    impl->role = Role::Server;
    impl->identity = std::move(identityPrivateBlob);
    return Session(std::move(impl));
}

bool Session::Established() const { return impl_ && impl_->state == Impl::State::Established; }
bool Session::Failed() const { return impl_ && impl_->state == Impl::State::Failed; }
Status Session::LastError() const { return impl_ ? impl_->error : Status::BadState; }
std::uint32_t Session::SendEpoch() const { return impl_ ? impl_->sendEpoch : 0; }
std::uint32_t Session::ReceiveEpoch() const { return impl_ ? impl_->recvEpoch : 0; }
void Session::SetRatchetInterval(std::uint64_t everyMessages) {
    if (impl_ && everyMessages > 0) impl_->ratchetEvery = everyMessages;
}
std::uint64_t Session::SentCount() const { return impl_ ? impl_->sendCount : 0; }
std::uint64_t Session::ReceivedCount() const { return impl_ ? impl_->recvCount : 0; }

Status Session::ClientBuildHello(const Bytes& attestation, Bytes& helloOut) {
    Impl& s = *impl_;
    if (s.role != Role::Client || s.state != Impl::State::Idle) return Status::BadState;
    if (attestation.size() > MaxAttestationBytes) return Status::TooLarge;
    auto eph = std::make_unique<Crypto::EcdhKey>();
    if (!eph->Generate()) return s.Fail(Status::CryptoFailure);
    Bytes hello(ClientHelloFixedBytes + attestation.size(), 0);
    hello[0] = Version;
    hello[1] = 0;
    PutU16(hello.data() + 2, static_cast<std::uint16_t>(attestation.size()));
    std::memcpy(hello.data() + 4, eph->Public().data(), PublicKeyBytes);
    if (!Crypto::Random(hello.data() + 4 + PublicKeyBytes, NonceBytes)) return s.Fail(Status::CryptoFailure);
    if (!attestation.empty()) std::memcpy(hello.data() + ClientHelloFixedBytes, attestation.data(), attestation.size());
    s.helloBytes = hello;
    s.eph = std::move(eph);
    s.state = Impl::State::HelloSent;
    helloOut = std::move(hello);
    return Status::Ok;
}

Status Session::ServerAcceptHello(const std::uint8_t* hello, std::size_t n, Bytes& serverHelloOut,
                                  Bytes* attestationOut) {
    Impl& s = *impl_;
    if (s.role != Role::Server || s.state != Impl::State::Idle) return Status::BadState;
    if (!hello || n < ClientHelloFixedBytes) return s.Fail(Status::BadMessage);
    if (hello[0] != Version) return s.Fail(Status::BadVersion);
    const std::size_t attLen = GetU16(hello + 2);
    if (hello[1] != 0 || attLen > MaxAttestationBytes || n != ClientHelloFixedBytes + attLen)
        return s.Fail(Status::BadMessage);

    PublicKey clientPub;
    std::memcpy(clientPub.data(), hello + 4, PublicKeyBytes);
    Crypto::EcdhKey eph;
    if (!eph.Generate()) return s.Fail(Status::CryptoFailure);

    Bytes reply(ServerHelloBytes, 0);
    reply[0] = Version;
    std::memcpy(reply.data() + 4, eph.Public().data(), PublicKeyBytes);
    if (!Crypto::Random(reply.data() + 4 + PublicKeyBytes, NonceBytes)) return s.Fail(Status::CryptoFailure);

    const Bytes clientHello(hello, hello + n);
    std::uint8_t transcript[HashBytes];
    if (!s.Transcript(clientHello, reply.data(), transcript)) return s.Fail(Status::CryptoFailure);
    if (!Crypto::EcdsaSign(s.identity, transcript, reply.data() + ServerHelloSignedBytes))
        return s.Fail(Status::CryptoFailure);

    std::uint8_t secret[32];
    if (!eph.Agree(clientPub, secret)) return s.Fail(Status::BadMessage); // invalid peer point
    const bool derived = s.Derive(secret, transcript);
    Crypto::Wipe(secret, sizeof secret);
    if (!derived) return s.Fail(Status::CryptoFailure);

    if (attestationOut) attestationOut->assign(hello + ClientHelloFixedBytes, hello + n);
    serverHelloOut = std::move(reply);
    s.state = Impl::State::Established;
    return Status::Ok;
}

Status Session::ClientAcceptServerHello(const std::uint8_t* msg, std::size_t n) {
    Impl& s = *impl_;
    if (s.role != Role::Client || s.state != Impl::State::HelloSent) return Status::BadState;
    if (!msg || n != ServerHelloBytes) return s.Fail(Status::BadMessage);
    if (msg[0] != Version) return s.Fail(Status::BadVersion);
    if (msg[1] != 0 || GetU16(msg + 2) != 0) return s.Fail(Status::BadMessage);

    std::uint8_t transcript[HashBytes];
    if (!s.Transcript(s.helloBytes, msg, transcript)) return s.Fail(Status::CryptoFailure);
    bool trusted = false;
    for (const PublicKey& key : s.pinned)
        trusted |= Crypto::EcdsaVerify(key, transcript, msg + ServerHelloSignedBytes);
    if (!trusted) return s.Fail(Status::BadSignature);

    PublicKey serverPub;
    std::memcpy(serverPub.data(), msg + 4, PublicKeyBytes);
    std::uint8_t secret[32];
    if (!s.eph->Agree(serverPub, secret)) return s.Fail(Status::BadMessage);
    const bool derived = s.Derive(secret, transcript);
    Crypto::Wipe(secret, sizeof secret);
    if (!derived) return s.Fail(Status::CryptoFailure);
    s.eph.reset(); // forget the ephemeral private key
    s.helloBytes.clear();
    s.state = Impl::State::Established;
    return Status::Ok;
}

Status Session::Seal(std::uint16_t opcode, const std::uint8_t* plain, std::size_t n, Bytes& out) {
    Impl& s = *impl_;
    if (s.state != Impl::State::Established) return Status::BadState;
    const std::size_t limit = s.role == Role::Client ? MaxPlainClientToServer : MaxPlainServerToClient;
    if (n > limit || (n && !plain)) return Status::TooLarge;
    if (s.sendCount >= MaxMessagesPerEpoch) return s.Fail(Status::CounterMismatch);
    std::uint8_t iv[IvBytes], aad[12];
    Impl::MakeIv(s.sendPrefix, s.sendCount, iv);
    Impl::MakeAad(opcode, n + TagBytes, s.sendCount, aad);
    Bytes result(n + TagBytes);
    if (!Crypto::AesGcmSeal(s.sendKey.data(), iv, aad, sizeof aad, plain, n, result.data()))
        return s.Fail(Status::CryptoFailure);
    ++s.sendCount;
    out = std::move(result);
    // Automatic re-key at an exact frame count; the receiver does the same right after opening it.
    if (s.sendCount >= s.ratchetEvery &&
        !s.RatchetDirection(s.sendKey, s.sendPrefix, s.sendCount, s.sendEpoch, s.SendsToServer()))
        return s.Fail(Status::CryptoFailure);
    return Status::Ok;
}

Status Session::Open(std::uint16_t opcode, const std::uint8_t* cipher, std::size_t n, Bytes& plainOut) {
    Impl& s = *impl_;
    if (s.state != Impl::State::Established) return Status::BadState;
    const std::size_t wireLimit = s.role == Role::Client ? MaxWireBodyServerToClient : MaxWireBodyClientToServer;
    if (!cipher || n < TagBytes) return s.Fail(Status::BadMessage);
    if (n > wireLimit) return s.Fail(Status::TooLarge);
    if (s.recvCount >= MaxMessagesPerEpoch) return s.Fail(Status::CounterMismatch);
    const std::size_t plainLen = n - TagBytes;
    std::uint8_t iv[IvBytes], aad[12];
    Impl::MakeIv(s.recvPrefix, s.recvCount, iv);
    Impl::MakeAad(opcode, n, s.recvCount, aad);
    Bytes result(plainLen);
    const Crypto::OpenResult r = Crypto::AesGcmOpen(s.recvKey.data(), iv, aad, sizeof aad, cipher, plainLen,
                                                    result.data());
    if (r == Crypto::OpenResult::TagMismatch) return s.Fail(Status::TagMismatch);
    if (r != Crypto::OpenResult::Ok) return s.Fail(Status::CryptoFailure);
    ++s.recvCount;
    plainOut = std::move(result);
    if (s.recvCount >= s.ratchetEvery &&
        !s.RatchetDirection(s.recvKey, s.recvPrefix, s.recvCount, s.recvEpoch, !s.SendsToServer()))
        return s.Fail(Status::CryptoFailure);
    return Status::Ok;
}

Status Session::RatchetSend() {
    Impl& s = *impl_;
    if (s.state != Impl::State::Established) return Status::BadState;
    if (!s.RatchetDirection(s.sendKey, s.sendPrefix, s.sendCount, s.sendEpoch, s.SendsToServer()))
        return s.Fail(Status::CryptoFailure);
    return Status::Ok;
}

Status Session::RatchetReceive() {
    Impl& s = *impl_;
    if (s.state != Impl::State::Established) return Status::BadState;
    if (!s.RatchetDirection(s.recvKey, s.recvPrefix, s.recvCount, s.recvEpoch, !s.SendsToServer()))
        return s.Fail(Status::CryptoFailure);
    return Status::Ok;
}

} // namespace NovaChannel
