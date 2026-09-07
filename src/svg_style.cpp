#include "svg_style.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "svg_util.h"

namespace svg_squisher {
namespace {

struct ParsedDeclaration {
  std::string property;
  std::string value;
  bool important = false;
};

struct CascadedValue {
  std::string value;
  bool important = false;
  bool inline_style = false;
  int specificity = 0;
  std::size_t order = 0;
  bool set = false;
};

const std::vector<const char*>& supported_properties() {
  static const std::vector<const char*> properties = {
    "fill", "fill-opacity", "stroke", "stroke-opacity", "stroke-width",
    "stroke-dasharray", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
    "fill-rule", "opacity", "display", "visibility", "font-size", "font-family",
    "font-weight", "font-style", "text-anchor", "letter-spacing",
  };
  return properties;
}

std::string strip_css_comments(const std::string& css) {
  std::string result;
  result.reserve(css.size());
  std::size_t cursor = 0;
  while (cursor < css.size()) {
    const std::size_t open = css.find("/*", cursor);
    if (open == std::string::npos) {
      result.append(css, cursor, std::string::npos);
      break;
    }
    result.append(css, cursor, open - cursor);
    const std::size_t close = css.find("*/", open + 2);
    if (close == std::string::npos) break;
    cursor = close + 2;
  }
  return result;
}

bool remove_important_suffix(std::string& value) {
  value = trim(value);
  const std::string lowered = lower_copy(value);
  const std::size_t bang = lowered.rfind('!');
  if (bang == std::string::npos || trim(lowered.substr(bang + 1)) != "important") {
    return false;
  }
  value = trim(value.substr(0, bang));
  return true;
}

std::vector<ParsedDeclaration> parse_declarations(const std::string& style_text) {
  std::vector<ParsedDeclaration> declarations;
  for (const std::string& raw_declaration : split(style_text, ';')) {
    const std::size_t colon = raw_declaration.find(':');
    if (colon == std::string::npos) continue;

    ParsedDeclaration declaration;
    declaration.property = lower_copy(trim(raw_declaration.substr(0, colon)));
    declaration.value = trim(raw_declaration.substr(colon + 1));
    declaration.important = remove_important_suffix(declaration.value);
    if (!declaration.property.empty() && !declaration.value.empty()) {
      declarations.push_back(std::move(declaration));
    }
  }
  return declarations;
}

std::vector<std::string> class_tokens(const pugi::xml_node& node) {
  std::vector<std::string> classes;
  std::istringstream stream(node.attribute("class").as_string());
  std::string token;
  while (stream >> token) classes.push_back(token);
  return classes;
}

bool has_class(const std::vector<std::string>& classes, const std::string& wanted) {
  return std::find(classes.begin(), classes.end(), wanted) != classes.end();
}

bool is_selector_name_char(char ch) {
  const unsigned char value = static_cast<unsigned char>(ch);
  return std::isalnum(value) || ch == '_' || ch == '-';
}

bool parse_compound_selector(const std::string& raw_selector,
                             const pugi::xml_node& node,
                             int* specificity) {
  const std::string selector = trim(raw_selector);
  if (selector.empty()) return false;
  if (selector.find_first_of(" >+~[:") != std::string::npos) return false;

  const std::vector<std::string> classes = class_tokens(node);
  const std::string node_name = node.name();
  const std::string node_id = node.attribute("id").as_string();
  std::size_t cursor = 0;
  int score = 0;

  if (selector[cursor] == '*') {
    ++cursor;
  } else if (selector[cursor] != '.' && selector[cursor] != '#') {
    const std::size_t begin = cursor;
    while (cursor < selector.size() && is_selector_name_char(selector[cursor])) ++cursor;
    if (cursor == begin || selector.substr(begin, cursor - begin) != node_name) return false;
    score += 1;
  }

  while (cursor < selector.size()) {
    const char prefix = selector[cursor++];
    if (prefix != '.' && prefix != '#') return false;
    const std::size_t begin = cursor;
    while (cursor < selector.size() && is_selector_name_char(selector[cursor])) ++cursor;
    if (cursor == begin) return false;
    const std::string token = selector.substr(begin, cursor - begin);
    if (prefix == '.') {
      if (!has_class(classes, token)) return false;
      score += 10;
    } else {
      if (node_id != token) return false;
      score += 100;
    }
  }

  if (specificity) *specificity = score;
  return true;
}

bool selector_matches(const CssRule& rule, const pugi::xml_node& node, int* specificity = nullptr) {
  return parse_compound_selector(rule.selector, node, specificity);
}

void apply_decl(StyleState& style, const std::string& key, const std::string& value) {
  if (key == "fill") style.fill = value;
  else if (key == "fill-opacity") style.fill_opacity = value;
  else if (key == "stroke") style.stroke = value;
  else if (key == "stroke-opacity") style.stroke_opacity = value;
  else if (key == "stroke-width") style.stroke_width = value;
  else if (key == "stroke-dasharray") style.stroke_dasharray = value;
  else if (key == "stroke-linecap") style.stroke_linecap = value;
  else if (key == "stroke-linejoin") style.stroke_linejoin = value;
  else if (key == "stroke-miterlimit") style.stroke_miterlimit = value;
  else if (key == "fill-rule") style.fill_rule = value;
  else if (key == "opacity") style.opacity = value;
  else if (key == "display") style.display = value;
  else if (key == "visibility") style.visibility = value;
  else if (key == "font-size") style.font_size = value;
  else if (key == "font-family") style.font_family = value;
  else if (key == "font-weight") style.font_weight = value;
  else if (key == "font-style") style.font_style = value;
  else if (key == "text-anchor") style.text_anchor = value;
  else if (key == "letter-spacing") style.letter_spacing = value;
}

std::string initial_value(const StyleState& initial, const std::string& property) {
  if (property == "fill") return initial.fill;
  if (property == "fill-opacity") return initial.fill_opacity;
  if (property == "stroke") return initial.stroke;
  if (property == "stroke-opacity") return initial.stroke_opacity;
  if (property == "stroke-width") return initial.stroke_width;
  if (property == "stroke-dasharray") return initial.stroke_dasharray;
  if (property == "stroke-linecap") return initial.stroke_linecap;
  if (property == "stroke-linejoin") return initial.stroke_linejoin;
  if (property == "stroke-miterlimit") return initial.stroke_miterlimit;
  if (property == "fill-rule") return initial.fill_rule;
  if (property == "opacity") return initial.opacity;
  if (property == "display") return initial.display;
  if (property == "visibility") return initial.visibility;
  if (property == "font-size") return initial.font_size;
  if (property == "font-family") return initial.font_family;
  if (property == "font-weight") return initial.font_weight;
  if (property == "font-style") return initial.font_style;
  if (property == "text-anchor") return initial.text_anchor;
  if (property == "letter-spacing") return initial.letter_spacing;
  return {};
}

bool is_inherited_property(const std::string& property) {
  return property != "display" && property != "opacity";
}

void offer_value(std::unordered_map<std::string, CascadedValue>& winners,
                 const ParsedDeclaration& declaration,
                 bool inline_style,
                 int specificity,
                 std::size_t order) {
  CascadedValue& current = winners[declaration.property];
  const bool wins = !current.set ||
    (declaration.important != current.important
      ? declaration.important
      : inline_style != current.inline_style
          ? inline_style
          : specificity > current.specificity ||
              (specificity == current.specificity && order >= current.order));
  if (!wins) return;

  current.value = declaration.value;
  current.important = declaration.important;
  current.inline_style = inline_style;
  current.specificity = specificity;
  current.order = order;
  current.set = true;
}

}  // namespace

std::vector<CssRule> parse_css_rules(const pugi::xml_node& svg_node) {
  std::vector<CssRule> rules;
  for (const pugi::xpath_node& style_node : svg_node.select_nodes(".//style")) {
    const std::string css = strip_css_comments(style_node.node().text().as_string());
    std::size_t cursor = 0;
    while (cursor < css.size()) {
      const std::size_t open = css.find('{', cursor);
      if (open == std::string::npos) break;
      const std::size_t close = css.find('}', open + 1);
      if (close == std::string::npos) break;

      const std::string selector_text = trim(css.substr(cursor, open - cursor));
      const std::vector<ParsedDeclaration> declarations =
        parse_declarations(css.substr(open + 1, close - open - 1));
      for (const std::string& selector_raw : split(selector_text, ',')) {
        CssRule rule;
        rule.selector = trim(selector_raw);
        for (const ParsedDeclaration& declaration : declarations) {
          std::string value = declaration.value;
          if (declaration.important) value += " !important";
          rule.declarations.push_back({declaration.property, std::move(value)});
        }
        if (!rule.selector.empty()) rules.push_back(std::move(rule));
      }
      cursor = close + 1;
    }
  }
  return rules;
}

bool node_has_local_property(const pugi::xml_node& node,
                             const std::vector<CssRule>& rules,
                             const std::string& property) {
  const std::string normalized_property = lower_copy(trim(property));
  if (node.attribute(normalized_property.c_str())) return true;
  if (node.attribute("style")) {
    for (const ParsedDeclaration& declaration : parse_declarations(node.attribute("style").as_string())) {
      if (declaration.property == normalized_property) return true;
    }
  }

  for (const CssRule& rule : rules) {
    if (!selector_matches(rule, node)) continue;
    for (const auto& [key, value] : rule.declarations) {
      (void)value;
      if (lower_copy(trim(key)) == normalized_property) return true;
    }
  }
  return false;
}

StyleState resolve_style(const pugi::xml_node& node,
                         const std::vector<CssRule>& rules,
                         const StyleState& inherited) {
  const StyleState initial;
  StyleState style = inherited;
  style.opacity = initial.opacity;
  style.display = initial.display;
  std::unordered_map<std::string, CascadedValue> winners;
  std::size_t order = 0;

  // Presentation attributes enter the author cascade with zero specificity.
  for (const char* property : supported_properties()) {
    if (!node.attribute(property)) continue;
    offer_value(winners, {property, node.attribute(property).as_string(), false}, false, 0, order++);
  }

  for (const CssRule& rule : rules) {
    int specificity = 0;
    if (!selector_matches(rule, node, &specificity)) continue;
    for (const auto& [raw_property, raw_value] : rule.declarations) {
      ParsedDeclaration declaration;
      declaration.property = lower_copy(trim(raw_property));
      declaration.value = raw_value;
      declaration.important = remove_important_suffix(declaration.value);
      offer_value(winners, declaration, false, specificity, order++);
    }
  }

  // Inline declarations outrank selector-based declarations at the author origin.
  if (node.attribute("style")) {
    for (const ParsedDeclaration& declaration : parse_declarations(node.attribute("style").as_string())) {
      offer_value(winners, declaration, true, 0, order++);
    }
  }

  for (const auto& [property, winner] : winners) {
    if (!winner.set) continue;
    const std::string keyword = lower_copy(trim(winner.value));
    if (keyword == "inherit") {
      apply_decl(style, property, initial_value(inherited, property));
      continue;
    }
    if (keyword == "unset" && is_inherited_property(property)) {
      continue;
    }
    if (keyword == "initial" || keyword == "unset") {
      apply_decl(style, property, initial_value(initial, property));
    } else {
      apply_decl(style, property, winner.value);
    }
  }

  return style;
}

}  // namespace svg_squisher
