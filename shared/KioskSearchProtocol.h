#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace KioskSearchProtocol {
constexpr std::uint16_t LookupOpcode = 0x1217;
constexpr std::uint32_t LookupMagic = 0x31534b52; // "RKS1", little endian
constexpr std::size_t NameBytes = 21;
constexpr std::size_t NativeReplyBytes = 38;
enum class Result : std::uint8_t { Found = 0, Disabled = 1, Unavailable = 3 };
#pragma pack(push, 1)
struct LookupRequest { char name[NameBytes]; std::uint32_t magic; };
struct LookupReply {
    Result result;
    char name[NameBytes];
    std::uint32_t magic;
    float position[3]; // Reserved native envelope; remote shopping does not move the buyer.
    std::int32_t seller;
};
#pragma pack(pop)
static_assert(sizeof(LookupRequest) == 25 && offsetof(LookupRequest, magic) == 21);
static_assert(sizeof(LookupReply) == 42 && offsetof(LookupReply, magic) == 22 &&
    offsetof(LookupReply, seller) == NativeReplyBytes);
inline bool ValidName(const char* name) {
    for (std::size_t i = 0; i < NameBytes; ++i) {
        const auto value = static_cast<unsigned char>(name[i]);
        if (!value) return i != 0;
        if (i == NameBytes - 1 || value < 0x20 || value == 0x7f) return false;
    }
    return false;
}
inline bool TaggedRequest(const void* body, std::size_t size) {
    if (!body || size < sizeof(LookupRequest)) return false;
    std::uint32_t magic{};
    std::memcpy(&magic, static_cast<const unsigned char*>(body) + offsetof(LookupRequest, magic), sizeof magic);
    return magic == LookupMagic;
}
} // namespace KioskSearchProtocol
