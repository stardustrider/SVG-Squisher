#pragma once

#include <optional>
#include <vector>

#include <pugixml.hpp>

#include "svg_squisher.h"
#include "svg_style.h"

namespace svg_squisher {

void collect_paths_from_svg(const pugi::xml_node& svg_node,
                            const std::vector<CssRule>& rules,
                            const StyleState& root_style,
                            const std::optional<std::string>& font_path,
                            std::vector<PathEntry>& out_paths,
                            bool font_path_is_authoritative = false,
                            std::vector<std::string>* fonts_used = nullptr,
                            std::vector<Diagnostic>* diagnostics = nullptr,
                            std::size_t* missing_glyphs = nullptr);

}  // namespace svg_squisher
