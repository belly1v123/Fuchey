#pragma once
#include <string>
#include <span>
#include <cstdint>

namespace Fuchey {
namespace Crypto {

class Base64 {
public:
    static std::string encode(std::span<const uint8_t> data);
};

} // namespace Crypto
} // namespace Fuchey