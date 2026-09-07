#include <cstddef>
#include <cstdint>
#include <string>

#include "svg_squisher.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 1024 * 1024) return 0;
  const std::string input(reinterpret_cast<const char*>(data), size);
  svg_squisher::Options options;
  options.precision = 4;
  const svg_squisher::ConversionResult result =
      svg_squisher::SvgSquisher{}.convert_string(input, options);
  if (result.success && result.svg.empty()) __builtin_trap();
  return 0;
}
