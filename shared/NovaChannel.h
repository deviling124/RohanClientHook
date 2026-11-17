#ifndef NOVA_CHANNEL_H
#define NOVA_CHANNEL_H
// Encrypted channel core. Portable protocol and state machine; no game or Win32 types.
// Design: docs/ENCRYPTED_CHANNEL_DESIGN.md. Wire and hook facts: docs/TRANSPORT_MAPPING.md.
//
// Mirrored in both hook projects (RohanClient_Hook/shared and RohanMapServ_Hook/shared).
// A Session is NOT thread safe. The hooks serialize access: the client send path runs under
// the connection critical section, and each connection receives on one worker thread.
//
// Security properties (see the design for limits): ephemeral ECDH, server signature over the
// whole handshake transcript, AES-256-GCM with a strict per-direction counter, ratcheting.
// It does not make a re-implemented or hooked client trustworthy.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace NovaChannel {

// Version 2: automatic per-direction ratchet every RatchetEveryMessages frames (version 1 had none).
// The version byte is also part of the offer a server puts in the stock key frame, so a version 1
// client never matches a version 2 server's offer and simply keeps playing plaintext.
#ifdef NOVA_TEST_RATCHET_INTERVAL
// TEST BUILDS ONLY (cl /DNOVA_TEST_RATCHET_INTERVAL=N): a short re-key interval to watch the ratchet live,
// with a DIFFERENT version byte so a test build never negotiates with a normal one (it just stays plaintext).
inline constexpr std::uint8_t Version = 0xE2;
#else
inline constexpr std::uint8_t Version = 2;
#endif
inline constexpr std::size_t PublicKeyBytes = 64;   // X || Y, big endian, P-256
inline constexpr std::size_t NonceBytes = 32;
inline constexpr std::size_t SignatureBytes = 64;   // r || s, P-256 ECDSA
inline constexpr std::size_t KeyBytes = 32;
inline constexpr std::size_t TagBytes = 16;
inline constexpr std::size_t IvBytes = 12;
inline constexpr std::size_t HashBytes = 32;
inline constexpr std::size_t MaxAttestationBytes = 256;
inline constexpr std::size_t ClientHelloFixedBytes = 100;  // 4 + 64 + 32
inline constexpr std::size_t ServerHelloBytes = 164;       // 4 + 64 + 32 + 64
inline constexpr std::size_t ServerHelloSignedBytes = 100; // everything before the signature

// Largest frame body on the wire per direction (ciphertext plus tag). Measured:
// the client writes at most `length & 0xfff` bytes; the client reads at most 8192.
inline constexpr std::size_t MaxWireBodyClientToServer = 4095;
inline constexpr std::size_t MaxWireBodyServerToClient = 8192;
inline constexpr std::size_t MaxPlainClientToServer = MaxWireBodyClientToServer - TagBytes;
inline constexpr std::size_t MaxPlainServerToClient = MaxWireBodyServerToClient - TagBytes;

// Counters never wrap: a session ends (and re-keys or reconnects) long before this.
inline constexpr std::uint64_t MaxMessagesPerEpoch = 1ull << 40;

// Each direction re-keys ITSELF after exactly this many frames: the sender ratchets its sending key
// right after sealing frame number N-1 (counting from 0), and the receiver ratchets its receiving key
// right after opening that same frame. Counters are strict and TCP is ordered, so both ends reach
// the boundary at the same frame with no extra message, and frames in flight are never ambiguous.
// The new key is HKDF(old key || nonce prefix) with a direction and epoch label, and the old key is
// erased: this gives forward secrecy for earlier frames, but a key stolen NOW still unlocks all
// later epochs (no post-compromise security; that needs a fresh ECDH exchange, not built yet).
#ifdef NOVA_TEST_RATCHET_INTERVAL
inline constexpr std::uint64_t RatchetEveryMessages = NOVA_TEST_RATCHET_INTERVAL;
#else
inline constexpr std::uint64_t RatchetEveryMessages = 1ull << 16;
#endif

// PLACEHOLDER opcodes. They have NOT been checked against both dispatchers or the free
// opcode range (design open item 3). Do not ship them until verified.
inline constexpr std::uint16_t OpcodeClientHello = 0x3F01;
inline constexpr std::uint16_t OpcodeServerHello = 0x3F02;
// 0x3F03 is intentionally unused (it was reserved for an explicit rekey frame; re-keying is implicit now).
inline constexpr std::uint16_t OpcodeChannelReady = 0x3F04; // first SEALED client frame (empty body)

// The stock server already sends opcode 0x2001 (172 bytes) at the start of every connection;
// its first 40 bytes are the RC4 key string. A channel-capable server sets this 4-byte offer
// in the otherwise unused tail so old clients ignore it and new clients know to start the
// handshake. A server that does not set it is never sent a hello (a stock server would treat
// an unknown opcode as an error and drop the connection). The server must write the whole
// body deterministically (zero the rest): the stock code leaves stack bytes there.
inline constexpr std::uint16_t OpcodeStockKey = 0x2001;
inline constexpr std::size_t StockKeyFrameBytes = 0xAC;
inline constexpr std::size_t OfferOffset = 44;
inline constexpr std::array<std::uint8_t, 3> OfferMagic = {'N', 'C', 'H'};
inline constexpr std::size_t OfferBytes = 4; // magic + one version byte (== Version)

// Client to server: sealing starts with the sealed ChannelReady frame. Frames the client sent
// before it are plaintext; server to client: everything after ServerHello is sealed.

enum class Role { Client, Server };

enum class Status {
    Ok,
    BadState,        // call not valid in the current state, or the session already failed
    BadMessage,      // malformed, truncated or oversized input
    BadVersion,      // unsupported protocol version
    BadSignature,    // server handshake signature did not verify against a pinned key
    CryptoFailure,   // the crypto provider failed
    CounterMismatch, // counter exhausted
    TagMismatch,     // authentication failed (tamper, replay, reorder or wrong keys)
    TooLarge,        // body exceeds the direction's limit
};
const char* StatusName(Status status);

using PublicKey = std::array<std::uint8_t, PublicKeyBytes>;
using Bytes = std::vector<std::uint8_t>;

namespace Crypto {
// Implemented in NovaChannelCng.cpp (Windows CNG). Every function returns false on failure.
bool Random(std::uint8_t* out, std::size_t n);
bool Sha256(const std::uint8_t* data, std::size_t n, std::uint8_t out[HashBytes]);
bool HmacSha256(const std::uint8_t* key, std::size_t keyLen, const std::uint8_t* data,
                std::size_t n, std::uint8_t out[HashBytes]);
void Wipe(void* p, std::size_t n);
bool ConstantTimeEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t n);

// RFC 5869 HKDF-SHA256, built on HmacSha256 (NovaChannel.cpp). n <= 255 * 32.
bool Hkdf(const std::uint8_t* salt, std::size_t saltLen, const std::uint8_t* ikm, std::size_t ikmLen,
          const std::uint8_t* info, std::size_t infoLen, std::uint8_t* out, std::size_t n);

// AES-256-GCM. `out` receives n bytes of ciphertext followed by a 16-byte tag.
bool AesGcmSeal(const std::uint8_t key[KeyBytes], const std::uint8_t iv[IvBytes],
                const std::uint8_t* aad, std::size_t aadLen, const std::uint8_t* plain,
                std::size_t n, std::uint8_t* out);
enum class OpenResult { Ok, TagMismatch, Failure };
// `cipher` is n bytes of ciphertext followed by the tag (n + 16 bytes in all).
OpenResult AesGcmOpen(const std::uint8_t key[KeyBytes], const std::uint8_t iv[IvBytes],
                      const std::uint8_t* aad, std::size_t aadLen, const std::uint8_t* cipher,
                      std::size_t n, std::uint8_t* plainOut);

// Ephemeral ECDH on P-256. The private key never leaves this object.
class EcdhKey {
public:
    EcdhKey();
    ~EcdhKey();
    EcdhKey(const EcdhKey&) = delete;
    EcdhKey& operator=(const EcdhKey&) = delete;
    bool Generate();
    const PublicKey& Public() const { return pub_; }
    // Big endian X coordinate of the shared point. Fails for an invalid peer point.
    bool Agree(const PublicKey& peer, std::uint8_t secret[32]) const;
private:
    void* handle_ = nullptr;
    PublicKey pub_{};
};

// Server identity: ECDSA P-256. The private blob is CNG's ECCPRIVATE_BLOB; keep it on the server.
bool EcdsaGenerate(Bytes& privateBlob, PublicKey& pub);
bool EcdsaSign(const Bytes& privateBlob, const std::uint8_t hash[HashBytes],
               std::uint8_t sig[SignatureBytes]);
bool EcdsaVerify(const PublicKey& pub, const std::uint8_t hash[HashBytes],
                 const std::uint8_t sig[SignatureBytes]);
} // namespace Crypto

class Session {
public:
    // The client trusts only these server identity keys (current and next).
    static Session MakeClient(std::vector<PublicKey> pinnedServerKeys);
    // The server signs with this identity private blob (see Crypto::EcdsaGenerate).
    static Session MakeServer(Bytes identityPrivateBlob);

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    // Client: step 1. `attestation` is an opaque, bounded blob (hashes etc., decided later).
    Status ClientBuildHello(const Bytes& attestation, Bytes& helloOut);
    // Server: step 2. Verifies the format, builds and signs the reply, derives the keys.
    // `attestationOut` (optional) receives the client's attestation for policy checks.
    Status ServerAcceptHello(const std::uint8_t* hello, std::size_t n, Bytes& serverHelloOut,
                             Bytes* attestationOut);
    // Client: step 3. Verifies the signature against the pinned keys and derives the keys.
    Status ClientAcceptServerHello(const std::uint8_t* msg, std::size_t n);

    // Encrypts one frame body. `out` is n + 16 bytes. The header opcode and length are
    // authenticated as additional data. The caller writes `out.size()` as the frame length.
    Status Seal(std::uint16_t opcode, const std::uint8_t* plain, std::size_t n, Bytes& out);
    // Decrypts one frame body (ciphertext plus tag). Any failure ends the session.
    Status Open(std::uint16_t opcode, const std::uint8_t* cipher, std::size_t n, Bytes& plainOut);

    // Re-keying is automatic (see RatchetEveryMessages). These two re-key ONE direction explicitly and
    // exist for tests and for a future explicit rekey: the sender calls RatchetSend() right after it
    // sealed a frame and the receiver calls RatchetReceive() right after it opened that same frame.
    Status RatchetSend();
    Status RatchetReceive();
    // Tests only: change the automatic interval. Both ends MUST use the same value or they desynchronize.
    void SetRatchetInterval(std::uint64_t everyMessages);

    bool Established() const;
    bool Failed() const;
    Status LastError() const;
    std::uint32_t SendEpoch() const;
    std::uint32_t ReceiveEpoch() const;
    std::uint64_t SentCount() const;
    std::uint64_t ReceivedCount() const;

private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace NovaChannel
#endif
