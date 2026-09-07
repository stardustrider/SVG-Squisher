#pragma once

#include <string>
#include <vector>

#include <pugixml.hpp>

#include "svg_squisher.h"

namespace svg_squisher {

struct StyleState {
  std::string fill = "black";
  std::string fill_opacity = "1";
  std::string stroke = "none";
  std::string stroke_opacity = "1";
  std::string stroke_width = "1";
  std::string stroke_dasharray;
  std::string stroke_linecap = "butt";
  std::string stroke_linejoin = "miter";
  std::string stroke_miterlimit = "4";
  std::string fill_rule = "nonzero";
  std::string opacity = "1";
  std::string display = "inline";
  std::string visibility = "visible";
  std::string font_size = "16";
  std::string font_family;
  std::string font_weight = "400";
  std::string font_style = "normal";
  std::string text_anchor = "start";
  std::string letter_spacing = "0";
};

std::vector<CssRule> parse_css_rules(const pugi::xml_node& svg_node);

bool node_has_local_property(const pugi::xml_node& node,
                             const std::vector<CssRule>& rules,
                             const std::string& property);

StyleState resolve_style(const pugi::xml_node& node,
                         const std::vector<CssRule>& rules,
                         const StyleState& inherited);

}  // namespace svg_squisher
