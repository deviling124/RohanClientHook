// Windows CNG implementation of NovaChannel::Crypto. No third-party dependency.
// Needs Windows 10 or later (AES-GCM, ECDH/ECDSA P-256, one-shot BCryptHash).
#include "NovaChannel.h"
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <cstring>

#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002L)
#endif

namespace NovaChannel::Crypto {
namespace {
constexpr ULONG EcdhPublicMagic = 0x314B4345;   // "ECK1": ECDH P-256 public
constexpr ULONG EcdsaPublicMagic = 0x31534345;  // "ECS1": ECDSA P-256 public

BCRYPT_ALG_HANDLE OpenAlg(LPCWSTR id, ULONG flags, LPCWSTR chainMode = nullptr) {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, id, nullptr, flags))) return nullptr;
    if (chainMode &&
        !BCRYPT_SUCCESS(BCryptSetProperty(h, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(chainMode)),
            static_cast<ULONG>((wcslen(chainMode) + 1) * sizeof(wchar_t)), 0))) {
        BCryptCloseAlgorithmProvider(h, 0);
        return nullptr;
    }
    return h;
}
// Process-lifetime handles (never closed; the OS reclaims them at exit).
BCRYPT_ALG_HANDLE Sha256Alg() { static BCRYPT_ALG_HANDLE h = OpenAlg(BCRYPT_SHA256_ALGORITHM, 0); return h; }
BCRYPT_ALG_HANDLE HmacAlg() { static BCRYPT_ALG_HANDLE h = OpenAlg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG); return h; }
BCRYPT_ALG_HANDLE EcdhAlg() { static BCRYPT_ALG_HANDLE h = OpenAlg(BCRYPT_ECDH_P256_ALGORITHM, 0); return h; }
BCRYPT_ALG_HANDLE EcdsaAlg() { static BCRYPT_ALG_HANDLE h = OpenAlg(BCRYPT_ECDSA_P256_ALGORITHM, 0); return h; }
BCRYPT_ALG_HANDLE AesGcmAlg() { static BCRYPT_ALG_HANDLE h = OpenAlg(BCRYPT_AES_ALGORITHM, 0, BCRYPT_CHAIN_MODE_GCM); return h; }

bool MakePublicBlob(ULONG magic, const PublicKey& pub, std::uint8_t blob[8 + PublicKeyBytes]) {
    const ULONG cb = 32;
    std::memcpy(blob, &magic, 4);
    std::memcpy(blob + 4, &cb, 4);
    std::memcpy(blob + 8, pub.data(), PublicKeyBytes);
    return true;
}
bool ImportPublic(BCRYPT_ALG_HANDLE alg, ULONG magic, const PublicKey& pub, BCRYPT_KEY_HANDLE& key) {
    std::uint8_t blob[8 + PublicKeyBytes];
    MakePublicBlob(magic, pub, blob);
    return alg && BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                                     sizeof blob, 0));
}
bool ExportPublic(BCRYPT_KEY_HANDLE key, PublicKey& pub) {
    std::uint8_t blob[8 + PublicKeyBytes];
    ULONG got = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &got, 0)) ||
        got != sizeof blob) return false;
    std::memcpy(pub.data(), blob + 8, PublicKeyBytes);
    return true;
}
} // namespace

bool Random(std::uint8_t* out, std::size_t n) {
    return n == 0 || BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool Sha256(const std::uint8_t* data, std::size_t n, std::uint8_t out[HashBytes]) {
    static const std::uint8_t empty = 0;
    auto alg = Sha256Alg();
    return alg && BCRYPT_SUCCESS(BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(n ? data : &empty),
                                            static_cast<ULONG>(n), out, HashBytes));
}

bool HmacSha256(const std::uint8_t* key, std::size_t keyLen, const std::uint8_t* data, std::size_t n,
                std::uint8_t out[HashBytes]) {
    static const std::uint8_t empty = 0;
    auto alg = HmacAlg();
    return alg && BCRYPT_SUCCESS(BCryptHash(alg, const_cast<PUCHAR>(keyLen ? key : &empty),
                                            static_cast<ULONG>(keyLen), const_cast<PUCHAR>(n ? data : &empty),
                                            static_cast<ULONG>(n), out, HashBytes));
}

void Wipe(void* p, std::size_t n) { if (p && n) SecureZeroMemory(p, n); }

bool ConstantTimeEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

bool AesGcmSeal(const std::uint8_t key[KeyBytes], const std::uint8_t iv[IvBytes], const std::uint8_t* aad,
                std::size_t aadLen, const std::uint8_t* plain, std::size_t n, std::uint8_t* out) {
    static std::uint8_t dummy[1] = {0};
    auto alg = AesGcmAlg();
    if (!alg) return false;
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<PUCHAR>(key), KeyBytes, 0)))
        return false;
    std::uint8_t tag[TagBytes] = {};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(iv);
    info.cbNonce = IvBytes;
    info.pbAuthData = const_cast<PUCHAR>(aadLen ? aad : dummy);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag = tag;
    info.cbTag = TagBytes;
    ULONG got = 0;
    const NTSTATUS st = BCryptEncrypt(k, const_cast<PUCHAR>(n ? plain : dummy), static_cast<ULONG>(n), &info,
                                      nullptr, 0, n ? out : dummy, static_cast<ULONG>(n), &got, 0);
    BCryptDestroyKey(k);
    if (!BCRYPT_SUCCESS(st) || got != n) return false;
    std::memcpy(out + n, tag, TagBytes);
    return true;
}

OpenResult AesGcmOpen(const std::uint8_t key[KeyBytes], const std::uint8_t iv[IvBytes], const std::uint8_t* aad,
                      std::size_t aadLen, const std::uint8_t* cipher, std::size_t n, std::uint8_t* plainOut) {
    static std::uint8_t dummy[1] = {0};
    auto alg = AesGcmAlg();
    if (!alg) return OpenResult::Failure;
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<PUCHAR>(key), KeyBytes, 0)))
        return OpenResult::Failure;
    std::uint8_t tag[TagBytes];
    std::memcpy(tag, cipher + n, TagBytes);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(iv);
    info.cbNonce = IvBytes;
    info.pbAuthData = const_cast<PUCHAR>(aadLen ? aad : dummy);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag = tag;
    info.cbTag = TagBytes;
    ULONG got = 0;
    const NTSTATUS st = BCryptDecrypt(k, const_cast<PUCHAR>(n ? cipher : dummy), static_cast<ULONG>(n), &info,
                                      nullptr, 0, n ? plainOut : dummy, static_cast<ULONG>(n), &got, 0);
    BCryptDestroyKey(k);
    if (st == STATUS_AUTH_TAG_MISMATCH) {
        if (n) Wipe(plainOut, n);
        return OpenResult::TagMismatch;
    }
    return BCRYPT_SUCCESS(st) && got == n ? OpenResult::Ok : OpenResult::Failure;
}

EcdhKey::EcdhKey() = default;
EcdhKey::~EcdhKey() {
    if (handle_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(handle_));
}
bool EcdhKey::Generate() {
    if (handle_) { BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(handle_)); handle_ = nullptr; }
    auto alg = EcdhAlg();
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!alg || !BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &k, 256, 0))) return false;
    if (!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(k, 0)) || !ExportPublic(k, pub_)) {
        BCryptDestroyKey(k);
        return false;
    }
    handle_ = k;
    return true;
}
bool EcdhKey::Agree(const PublicKey& peer, std::uint8_t secret[32]) const {
    if (!handle_) return false;
    BCRYPT_KEY_HANDLE peerKey = nullptr;
    if (!ImportPublic(EcdhAlg(), EcdhPublicMagic, peer, peerKey)) return false; // rejects points off the curve
    BCRYPT_SECRET_HANDLE s = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(handle_), peerKey, &s, 0));
    std::uint8_t raw[32] = {};
    ULONG got = 0;
    if (ok) ok = BCRYPT_SUCCESS(BCryptDeriveKey(s, BCRYPT_KDF_RAW_SECRET, nullptr, raw, 32, &got, 0)) && got == 32;
    if (s) BCryptDestroySecret(s);
    BCryptDestroyKey(peerKey);
    if (ok) {
        // CNG returns the raw secret little endian; use the usual big endian X coordinate.
        for (int i = 0; i < 32; ++i) secret[i] = raw[31 - i];
    }
    Wipe(raw, sizeof raw);
    return ok;
}

bool EcdsaGenerate(Bytes& privateBlob, PublicKey& pub) {
    auto alg = EcdsaAlg();
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!alg || !BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &k, 256, 0)) ||
        !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(k, 0))) {
        if (k) BCryptDestroyKey(k);
        return false;
    }
    ULONG need = 0;
    bool ok = BCRYPT_SUCCESS(BCryptExportKey(k, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &need, 0)) && need > 0;
    if (ok) {
        privateBlob.assign(need, 0);
        ULONG got = 0;
        ok = BCRYPT_SUCCESS(BCryptExportKey(k, nullptr, BCRYPT_ECCPRIVATE_BLOB, privateBlob.data(), need, &got, 0)) &&
             got == need && ExportPublic(k, pub);
    }
    BCryptDestroyKey(k);
    if (!ok) { Wipe(privateBlob.data(), privateBlob.size()); privateBlob.clear(); }
    return ok;
}

bool EcdsaSign(const Bytes& privateBlob, const std::uint8_t hash[HashBytes], std::uint8_t sig[SignatureBytes]) {
    auto alg = EcdsaAlg();
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!alg || privateBlob.empty() ||
        !BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &k,
                                            const_cast<PUCHAR>(privateBlob.data()),
                                            static_cast<ULONG>(privateBlob.size()), 0))) return false;
    ULONG got = 0;
    const bool ok = BCRYPT_SUCCESS(BCryptSignHash(k, nullptr, const_cast<PUCHAR>(hash), HashBytes, sig,
                                                  SignatureBytes, &got, 0)) && got == SignatureBytes;
    BCryptDestroyKey(k);
    return ok;
}

bool EcdsaVerify(const PublicKey& pub, const std::uint8_t hash[HashBytes], const std::uint8_t sig[SignatureBytes]) {
    BCRYPT_KEY_HANDLE k = nullptr;
    if (!ImportPublic(EcdsaAlg(), EcdsaPublicMagic, pub, k)) return false;
    const bool ok = BCRYPT_SUCCESS(BCryptVerifySignature(k, nullptr, const_cast<PUCHAR>(hash), HashBytes,
                                                         const_cast<PUCHAR>(sig), SignatureBytes, 0));
    BCryptDestroyKey(k);
    return ok;
}

} // namespace NovaChannel::Crypto
