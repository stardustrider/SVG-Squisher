#pragma once

#include <string>

#include "svg_squisher.h"

namespace svg_squisher {

FontIdentity identify_font_file(const std::string& utf8_path);

}  // namespace svg_squisher
