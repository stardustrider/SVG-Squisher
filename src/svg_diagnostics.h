#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "svg_squisher.h"

namespace pugi {
class xml_node;
}

namespace svg_squisher {

class SvgIdIndex;

inline constexpr std::size_t kMaxSvgStructuralDepth = 256;
inline constexpr std::size_t kMaxUseReferenceDepth = 64;
inline constexpr std::size_t kMaxExpandedTraversalDepth = 256;
inline constexpr std::size_t kMaxExpandedNodeCount = 16384;
inline constexpr std::size_t kMaxOutputPathCount = 8192;

void validate_svg_structure_depth(const pugi::xml_node& svg_root);
std::vector<Diagnostic> inspect_svg_capabilities(const pugi::xml_node& svg_root,
                                                 const SvgIdIndex& id_index);
std::size_t count_svg_elements(const pugi::xml_node& svg_root);
const char* diagnostic_severity_name(DiagnosticSeverity severity);

}  // namespace svg_squisher
