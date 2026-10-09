#ifndef tjsKnownByteCodeCompatibilityH
#define tjsKnownByteCodeCompatibilityH

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TJS { namespace known_bytecode {
using Digest = std::array<uint8_t, 32>;
struct DirectReadSignature {
    uint32_t ip;
    int16_t result, receiver;
    uint16_t localConstant;
};
// A rule is an immutable fingerprint plus two exact reads of one local string.
// The generic entry is also used by original compiled test fixtures. Production
// loading calls TryApplyKnownCompatibility, whose rule cannot be overridden.
struct OptionalMemberRule {
    const char* basename;
    size_t length;
    Digest digest;
    const char* parentClass;
    const char* method;
    uint32_t codeWords, localConstants;
    std::array<DirectReadSignature, 2> reads;
};
Digest SoftwareSHA256(const uint8_t* bytes, size_t length) noexcept;
bool TryApplyOptionalMemberRule(const OptionalMemberRule& rule, const char* name,
    const uint8_t* bytes, size_t length, std::vector<uint8_t>& copy) noexcept;
bool TryApplyKnownCompatibility(const char* name, const uint8_t* bytes, size_t length,
    std::vector<uint8_t>& copy) noexcept;
} }
#endif
