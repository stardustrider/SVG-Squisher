#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include <pugixml.hpp>

namespace svg_squisher {

class SvgIdIndex {
public:
  void insert_first(const std::string& id, pugi::xml_node node);
  std::optional<pugi::xml_node> find(const std::string& id) const;

private:
  std::unordered_map<std::string, pugi::xml_node> nodes_;
};

bool should_skip_tag(const std::string& name);

SvgIdIndex build_svg_id_index(const pugi::xml_node& root);
std::optional<pugi::xml_node> find_by_id(const SvgIdIndex& index,
                                         const std::string& id);

}  // namespace svg_squisher
