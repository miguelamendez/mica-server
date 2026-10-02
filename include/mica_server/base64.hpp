#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mica {

inline std::string base64_encode(std::string_view input) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < input.size(); i += 3) {
    const auto first = static_cast<unsigned char>(input[i]);
    const auto second = i + 1 < input.size()
                            ? static_cast<unsigned char>(input[i + 1])
                            : 0;
    const auto third = i + 2 < input.size()
                           ? static_cast<unsigned char>(input[i + 2])
                           : 0;
    output.push_back(alphabet[first >> 2]);
    output.push_back(alphabet[((first & 0x03U) << 4) | (second >> 4)]);
    output.push_back(i + 1 < input.size()
                         ? alphabet[((second & 0x0fU) << 2) | (third >> 6)]
                         : '=');
    output.push_back(i + 2 < input.size() ? alphabet[third & 0x3fU] : '=');
  }
  return output;
}

}  // namespace mica
