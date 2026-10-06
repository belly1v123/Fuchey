#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <cstdint>

namespace Fuchey {
namespace Crypto {

class Base64 {
public:
    static std::string encode(std::span<const uint8_t> data);
    // Strict standard Base64 (with '=' padding). Returns false on any
    // invalid character, bad padding or length not a multiple of 4.
    static bool decode(std::string_view in, std::vector<uint8_t>& out);
};

} // namespace Crypto
} // namespace Fuchey