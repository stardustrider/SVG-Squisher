#include "svg_dom.h"

#include <algorithm>
#include <string>
#include <vector>

namespace svg_squisher {

void SvgIdIndex::insert_first(const std::string& id, pugi::xml_node node) {
  nodes_.emplace(id, node);
}

std::optional<pugi::xml_node> SvgIdIndex::find(const std::string& id) const {
  const auto match = nodes_.find(id);
  if (match != nodes_.end()) return match->second;
  return std::nullopt;
}

bool should_skip_tag(const std::string& name) {
  static const std::vector<std::string> skipped = {
    "defs", "style", "script", "title", "desc", "metadata", "clipPath", "mask",
    "filter", "linearGradient", "radialGradient", "pattern", "symbol", "image",
    "foreignObject"
  };
  return std::find(skipped.begin(), skipped.end(), name) != skipped.end();
}

SvgIdIndex build_svg_id_index(const pugi::xml_node& root) {
  SvgIdIndex index;
  if (!root) return index;

  std::vector<pugi::xml_node> pending{root};
  while (!pending.empty()) {
    const pugi::xml_node node = pending.back();
    pending.pop_back();
    if (const pugi::xml_attribute id = node.attribute("id")) {
      // Preserve the prior depth-first lookup behavior for malformed documents
      // containing duplicate IDs: the first element in document order wins.
      index.insert_first(id.as_string(), node);
    }
    std::vector<pugi::xml_node> children;
    for (const pugi::xml_node child : node.children()) {
      if (child.type() == pugi::node_element) children.push_back(child);
    }
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
      pending.push_back(*child);
    }
  }
  return index;
}

std::optional<pugi::xml_node> find_by_id(const SvgIdIndex& index,
                                         const std::string& id) {
  return index.find(id);
}

}  // namespace svg_squisher
