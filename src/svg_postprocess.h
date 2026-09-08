#pragma once

#include <optional>
#include <vector>

#include <pugixml.hpp>

#include "svg_squisher.h"

namespace svg_squisher {

std::vector<PathEntry> prepare_output_paths(const pugi::xml_node& svg_node,
                                            const std::vector<PathEntry>& paths,
                                            bool remove_background);

}  // namespace svg_squisher
